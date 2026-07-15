#include "mcp/MCPClient.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "mcp/detail/MCPDeadline.h"
#include "mcp/detail/MCPProtocol.h"
#include "mcp/detail/MCPText.h"
#include "mcp/detail/MCPToolCatalogAccess.h"
#include "mcp/detail/MCPTransportFactory.h"

namespace aiSDK {
namespace {

// MCPClient 并发合同说明：以下内容是状态机的可执行设计约束，不是逐行代码翻译。
// 本模块同时处理公开操作、Transport 回调、后台 Listener、控制 Worker 与 close。
// 所有跨线程结论都以 mutex_ 下的线性化点为准，不能依据线程启动或回调开始时间推断。
// 公开 API 保持同步外观；异步 Transport 只通过完成记录和条件变量与调用线程汇合。
// 请求完成采用 first-wins：响应、发送错误、主故障、close、超时中最先锁内写入者生效。
// 完成记录同时保存结果类别和状态快照，避免等待线程醒来后读取到更晚的生命周期状态。
// 主故障也采用 first-wins：last_failure_code_ 一旦由 Faulted 固定便不可覆盖。
// close 资源回收采用 single-owner：只允许一个调用者执行 Transport::close 与 Worker join。
// Listener 完整性与主生命周期分离，但能力声明要求监听缺口使已签发目录立即 stale。
// 下面的矩阵用于代码评审、竞态测试和未来修改时逐项核对。
// 状态 Disconnected 的核心语义：尚未打开 Transport；只允许 connect 建立控制线程和回调。
// 状态 Disconnected 的进入条件：进入必须在 mutex_ 下完成，使状态读取与完成竞争看到同一顺序。
// 状态 Disconnected 的请求约束：active_request_id_ 与完成记录只能由同一状态锁保护。
// 状态 Disconnected 的回调约束：Transport 回调不得在 Transport 自身锁内进入 Client。
// 状态 Disconnected 的错误约束：公开异常只携带闭合错误码和完成时状态快照。
// 状态 Disconnected 的目录约束：目录有效性由 revision、stale、签发令牌三者共同决定。
// 状态 Disconnected 的关闭约束：状态一旦进入 Closing 或 Closed 就不能再转回可用状态。
// 状态 Disconnected 的等待约束：所有等待谓词都必须包含会使操作终止的状态或完成记录。
// 状态 Disconnected 的可见性：状态读取通过 mutex_ 与写入同步，不依赖原子变量的松散组合。
// 状态 Connecting 的核心语义：Transport 正在打开；同步故障必须成为首次主故障。
// 状态 Connecting 的进入条件：进入必须在 mutex_ 下完成，使状态读取与完成竞争看到同一顺序。
// 状态 Connecting 的请求约束：active_request_id_ 与完成记录只能由同一状态锁保护。
// 状态 Connecting 的回调约束：Transport 回调不得在 Transport 自身锁内进入 Client。
// 状态 Connecting 的错误约束：公开异常只携带闭合错误码和完成时状态快照。
// 状态 Connecting 的目录约束：目录有效性由 revision、stale、签发令牌三者共同决定。
// 状态 Connecting 的关闭约束：状态一旦进入 Closing 或 Closed 就不能再转回可用状态。
// 状态 Connecting 的等待约束：所有等待谓词都必须包含会使操作终止的状态或完成记录。
// 状态 Connecting 的可见性：状态读取通过 mutex_ 与写入同步，不依赖原子变量的松散组合。
// 状态 Initializing 的核心语义：initialize 与 initialized 尚未全部完成；不能签发工具目录。
// 状态 Initializing 的进入条件：进入必须在 mutex_ 下完成，使状态读取与完成竞争看到同一顺序。
// 状态 Initializing 的请求约束：active_request_id_ 与完成记录只能由同一状态锁保护。
// 状态 Initializing 的回调约束：Transport 回调不得在 Transport 自身锁内进入 Client。
// 状态 Initializing 的错误约束：公开异常只携带闭合错误码和完成时状态快照。
// 状态 Initializing 的目录约束：目录有效性由 revision、stale、签发令牌三者共同决定。
// 状态 Initializing 的关闭约束：状态一旦进入 Closing 或 Closed 就不能再转回可用状态。
// 状态 Initializing 的等待约束：所有等待谓词都必须包含会使操作终止的状态或完成记录。
// 状态 Initializing 的可见性：状态读取通过 mutex_ 与写入同步，不依赖原子变量的松散组合。
// 状态 Ready 的核心语义：公开协议操作可用；仍受单前台槽和 Listener 完整性约束。
// 状态 Ready 的进入条件：进入必须在 mutex_ 下完成，使状态读取与完成竞争看到同一顺序。
// 状态 Ready 的请求约束：active_request_id_ 与完成记录只能由同一状态锁保护。
// 状态 Ready 的回调约束：Transport 回调不得在 Transport 自身锁内进入 Client。
// 状态 Ready 的错误约束：公开异常只携带闭合错误码和完成时状态快照。
// 状态 Ready 的目录约束：目录有效性由 revision、stale、签发令牌三者共同决定。
// 状态 Ready 的关闭约束：状态一旦进入 Closing 或 Closed 就不能再转回可用状态。
// 状态 Ready 的等待约束：所有等待谓词都必须包含会使操作终止的状态或完成记录。
// 状态 Ready 的可见性：状态读取通过 mutex_ 与写入同步，不依赖原子变量的松散组合。
// 状态 Closing 的核心语义：公开 close 已取得生命周期线性化点；新写入和新提交均被拒绝。
// 状态 Closing 的进入条件：进入必须在 mutex_ 下完成，使状态读取与完成竞争看到同一顺序。
// 状态 Closing 的请求约束：active_request_id_ 与完成记录只能由同一状态锁保护。
// 状态 Closing 的回调约束：Transport 回调不得在 Transport 自身锁内进入 Client。
// 状态 Closing 的错误约束：公开异常只携带闭合错误码和完成时状态快照。
// 状态 Closing 的目录约束：目录有效性由 revision、stale、签发令牌三者共同决定。
// 状态 Closing 的关闭约束：状态一旦进入 Closing 或 Closed 就不能再转回可用状态。
// 状态 Closing 的等待约束：所有等待谓词都必须包含会使操作终止的状态或完成记录。
// 状态 Closing 的可见性：状态读取通过 mutex_ 与写入同步，不依赖原子变量的松散组合。
// 状态 Closed 的核心语义：资源关闭完成；所有后续回调与公开协议操作均不得恢复实例。
// 状态 Closed 的进入条件：进入必须在 mutex_ 下完成，使状态读取与完成竞争看到同一顺序。
// 状态 Closed 的请求约束：active_request_id_ 与完成记录只能由同一状态锁保护。
// 状态 Closed 的回调约束：Transport 回调不得在 Transport 自身锁内进入 Client。
// 状态 Closed 的错误约束：公开异常只携带闭合错误码和完成时状态快照。
// 状态 Closed 的目录约束：目录有效性由 revision、stale、签发令牌三者共同决定。
// 状态 Closed 的关闭约束：状态一旦进入 Closing 或 Closed 就不能再转回可用状态。
// 状态 Closed 的等待约束：所有等待谓词都必须包含会使操作终止的状态或完成记录。
// 状态 Closed 的可见性：状态读取通过 mutex_ 与写入同步，不依赖原子变量的松散组合。
// 状态 Faulted 的核心语义：首次致命故障已经固定；后续事件不得覆盖根因或复活状态。
// 状态 Faulted 的进入条件：进入必须在 mutex_ 下完成，使状态读取与完成竞争看到同一顺序。
// 状态 Faulted 的请求约束：active_request_id_ 与完成记录只能由同一状态锁保护。
// 状态 Faulted 的回调约束：Transport 回调不得在 Transport 自身锁内进入 Client。
// 状态 Faulted 的错误约束：公开异常只携带闭合错误码和完成时状态快照。
// 状态 Faulted 的目录约束：目录有效性由 revision、stale、签发令牌三者共同决定。
// 状态 Faulted 的关闭约束：状态一旦进入 Closing 或 Closed 就不能再转回可用状态。
// 状态 Faulted 的等待约束：所有等待谓词都必须包含会使操作终止的状态或完成记录。
// 状态 Faulted 的可见性：状态读取通过 mutex_ 与写入同步，不依赖原子变量的松散组合。
// 请求阶段“分配请求 ID”的副作用边界：尚未调用 Provider，远端副作用为零。
// 请求阶段“分配请求 ID”遇到“响应”：同 ID 且完成槽为空时保存完整 JSON，并记录当时状态。
// 请求阶段“分配请求 ID”遇到“发送错误”：dispatch 匹配且完成槽为空时保存错误码，并记录当时状态。
// 请求阶段“分配请求 ID”遇到“致命故障”：先固定主故障，再尝试以该错误完成当前请求。
// 请求阶段“分配请求 ID”遇到“close”：先进入 Closing，再以 OperationCancelled 尝试完成当前请求。
// 请求阶段“分配请求 ID”遇到“超时”：wait_until 返回且仍持锁时写入 RequestTimeout。
// 请求阶段“分配请求 ID”遇到“异 ID 消息”：既非退休 ID 也非活动 ID 时属于协议错误。
// 请求阶段“准备消息”的副作用边界：Client 锁已释放，Provider 与序列化可以阻塞但不得提交。
// 请求阶段“准备消息”遇到“响应”：同 ID 且完成槽为空时保存完整 JSON，并记录当时状态。
// 请求阶段“准备消息”遇到“发送错误”：dispatch 匹配且完成槽为空时保存错误码，并记录当时状态。
// 请求阶段“准备消息”遇到“致命故障”：先固定主故障，再尝试以该错误完成当前请求。
// 请求阶段“准备消息”遇到“close”：先进入 Closing，再以 OperationCancelled 尝试完成当前请求。
// 请求阶段“准备消息”遇到“超时”：wait_until 返回且仍持锁时写入 RequestTimeout。
// 请求阶段“准备消息”遇到“异 ID 消息”：既非退休 ID 也非活动 ID 时属于协议错误。
// 请求阶段“准备完成”的副作用边界：仍未形成 Submitted，必须回到 Client 锁内二次校验。
// 请求阶段“准备完成”遇到“响应”：同 ID 且完成槽为空时保存完整 JSON，并记录当时状态。
// 请求阶段“准备完成”遇到“发送错误”：dispatch 匹配且完成槽为空时保存错误码，并记录当时状态。
// 请求阶段“准备完成”遇到“致命故障”：先固定主故障，再尝试以该错误完成当前请求。
// 请求阶段“准备完成”遇到“close”：先进入 Closing，再以 OperationCancelled 尝试完成当前请求。
// 请求阶段“准备完成”遇到“超时”：wait_until 返回且仍持锁时写入 RequestTimeout。
// 请求阶段“准备完成”遇到“异 ID 消息”：既非退休 ID 也非活动 ID 时属于协议错误。
// 请求阶段“提交临界区”的副作用边界：持有 Client 锁调用无同步回调的 commitPrepared。
// 请求阶段“提交临界区”遇到“响应”：同 ID 且完成槽为空时保存完整 JSON，并记录当时状态。
// 请求阶段“提交临界区”遇到“发送错误”：dispatch 匹配且完成槽为空时保存错误码，并记录当时状态。
// 请求阶段“提交临界区”遇到“致命故障”：先固定主故障，再尝试以该错误完成当前请求。
// 请求阶段“提交临界区”遇到“close”：先进入 Closing，再以 OperationCancelled 尝试完成当前请求。
// 请求阶段“提交临界区”遇到“超时”：wait_until 返回且仍持锁时写入 RequestTimeout。
// 请求阶段“提交临界区”遇到“异 ID 消息”：既非退休 ID 也非活动 ID 时属于协议错误。
// 请求阶段“已提交等待”的副作用边界：远端可能执行，响应、错误、故障、close、超时竞争首写。
// 请求阶段“已提交等待”遇到“响应”：同 ID 且完成槽为空时保存完整 JSON，并记录当时状态。
// 请求阶段“已提交等待”遇到“发送错误”：dispatch 匹配且完成槽为空时保存错误码，并记录当时状态。
// 请求阶段“已提交等待”遇到“致命故障”：先固定主故障，再尝试以该错误完成当前请求。
// 请求阶段“已提交等待”遇到“close”：先进入 Closing，再以 OperationCancelled 尝试完成当前请求。
// 请求阶段“已提交等待”遇到“超时”：wait_until 返回且仍持锁时写入 RequestTimeout。
// 请求阶段“已提交等待”遇到“异 ID 消息”：既非退休 ID 也非活动 ID 时属于协议错误。
// 请求阶段“已完成未退休”的副作用边界：完成记录已固定；成功结果的重复同 ID 响应仍是协议违规。
// 请求阶段“已完成未退休”遇到“响应”：同 ID 且完成槽为空时保存完整 JSON，并记录当时状态。
// 请求阶段“已完成未退休”遇到“发送错误”：dispatch 匹配且完成槽为空时保存错误码，并记录当时状态。
// 请求阶段“已完成未退休”遇到“致命故障”：先固定主故障，再尝试以该错误完成当前请求。
// 请求阶段“已完成未退休”遇到“close”：先进入 Closing，再以 OperationCancelled 尝试完成当前请求。
// 请求阶段“已完成未退休”遇到“超时”：wait_until 返回且仍持锁时写入 RequestTimeout。
// 请求阶段“已完成未退休”遇到“异 ID 消息”：既非退休 ID 也非活动 ID 时属于协议错误。
// 请求阶段“已退休”的副作用边界：仅失败、超时或取消的 ID 进入有界集合，迟到响应不再影响当前操作。
// 请求阶段“已退休”遇到“响应”：同 ID 且完成槽为空时保存完整 JSON，并记录当时状态。
// 请求阶段“已退休”遇到“发送错误”：dispatch 匹配且完成槽为空时保存错误码，并记录当时状态。
// 请求阶段“已退休”遇到“致命故障”：先固定主故障，再尝试以该错误完成当前请求。
// 请求阶段“已退休”遇到“close”：先进入 Closing，再以 OperationCancelled 尝试完成当前请求。
// 请求阶段“已退休”遇到“超时”：wait_until 返回且仍持锁时写入 RequestTimeout。
// 请求阶段“已退休”遇到“异 ID 消息”：既非退休 ID 也非活动 ID 时属于协议错误。
// 完成顺序“响应 -> 发送错误”：同 ID 且完成槽为空时保存完整 JSON，并记录当时状态。；后到的“发送错误”不得覆盖首次完成。
// 完成顺序“响应 -> 致命故障”：同 ID 且完成槽为空时保存完整 JSON，并记录当时状态。；后到的“致命故障”不得覆盖首次完成。
// 完成顺序“响应 -> close”：同 ID 且完成槽为空时保存完整 JSON，并记录当时状态。；后到的“close”不得覆盖首次完成。
// 完成顺序“响应 -> 超时”：同 ID 且完成槽为空时保存完整 JSON，并记录当时状态。；后到的“超时”不得覆盖首次完成。
// 完成顺序“发送错误 -> 响应”：dispatch 匹配且完成槽为空时保存错误码，并记录当时状态。；后到的“响应”不得覆盖首次完成。
// 完成顺序“发送错误 -> 致命故障”：dispatch
// 匹配且完成槽为空时保存错误码，并记录当时状态。；后到的“致命故障”不得覆盖首次完成。 完成顺序“发送错误 ->
// close”：dispatch 匹配且完成槽为空时保存错误码，并记录当时状态。；后到的“close”不得覆盖首次完成。 完成顺序“发送错误 ->
// 超时”：dispatch 匹配且完成槽为空时保存错误码，并记录当时状态。；后到的“超时”不得覆盖首次完成。 完成顺序“致命故障 ->
// 响应”：先固定主故障，再尝试以该错误完成当前请求。；后到的“响应”不得覆盖首次完成。 完成顺序“致命故障 ->
// 发送错误”：先固定主故障，再尝试以该错误完成当前请求。；后到的“发送错误”不得覆盖首次完成。 完成顺序“致命故障 ->
// close”：先固定主故障，再尝试以该错误完成当前请求。；后到的“close”不得覆盖首次完成。 完成顺序“致命故障 ->
// 超时”：先固定主故障，再尝试以该错误完成当前请求。；后到的“超时”不得覆盖首次完成。 完成顺序“close -> 响应”：先进入
// Closing，再以 OperationCancelled 尝试完成当前请求。；后到的“响应”不得覆盖首次完成。 完成顺序“close ->
// 发送错误”：先进入 Closing，再以 OperationCancelled 尝试完成当前请求。；后到的“发送错误”不得覆盖首次完成。
// 完成顺序“close -> 致命故障”：先进入 Closing，再以 OperationCancelled
// 尝试完成当前请求。；后到的“致命故障”不得覆盖首次完成。 完成顺序“close -> 超时”：先进入 Closing，再以
// OperationCancelled 尝试完成当前请求。；后到的“超时”不得覆盖首次完成。 完成顺序“超时 -> 响应”：wait_until
// 返回且仍持锁时写入 RequestTimeout。；后到的“响应”不得覆盖首次完成。 完成顺序“超时 -> 发送错误”：wait_until
// 返回且仍持锁时写入 RequestTimeout。；后到的“发送错误”不得覆盖首次完成。 完成顺序“超时 -> 致命故障”：wait_until
// 返回且仍持锁时写入 RequestTimeout。；后到的“致命故障”不得覆盖首次完成。 完成顺序“超时 -> close”：wait_until
// 返回且仍持锁时写入 RequestTimeout。；后到的“close”不得覆盖首次完成。 Transport 事件 SendCompleted
// 的请求语义：只完成匹配的控制发送；不代表 JSON-RPC 请求已收到响应。 Transport 事件 SendCompleted
// 的主状态语义：不改变主状态。 Transport 事件 SendCompleted 的监听语义：不改变 Listener。 Transport 事件 SendCompleted
// 的目录语义：不改变目录。 Transport 事件 SendCompleted 的并发要求：事件分类、状态修改和完成竞争必须处于同一个 mutex_
// 临界区。 Transport 事件 SendCompleted 的迟到要求：若 Client 已 Faulted、Closing 或 Closed，事件不得改写既有终态。
// Transport 事件 SendFailed 的请求语义：匹配活动 dispatch 时竞争请求错误首写。
// Transport 事件 SendFailed 的主状态语义：非致命前台发送错误不自动破坏 Client。
// Transport 事件 SendFailed 的监听语义：不改变 Listener。
// Transport 事件 SendFailed 的目录语义：控制发送可按 fatal 标记触发主故障。
// Transport 事件 SendFailed 的并发要求：事件分类、状态修改和完成竞争必须处于同一个 mutex_ 临界区。
// Transport 事件 SendFailed 的迟到要求：若 Client 已 Faulted、Closing 或 Closed，事件不得改写既有终态。
// Transport 事件 ListenerStarted 的请求语义：确认后台 GET 已经可监听。
// Transport 事件 ListenerStarted 的主状态语义：不完成前台请求。
// Transport 事件 ListenerStarted 的监听语义：迁移为 Listening 并清除监听错误。
// Transport 事件 ListenerStarted 的目录语义：不直接改变目录。
// Transport 事件 ListenerStarted 的并发要求：事件分类、状态修改和完成竞争必须处于同一个 mutex_ 临界区。
// Transport 事件 ListenerStarted 的迟到要求：若 Client 已 Faulted、Closing 或 Closed，事件不得改写既有终态。
// Transport 事件 ListenerUnsupported 的请求语义：表达 405 能力降级。
// Transport 事件 ListenerUnsupported 的主状态语义：不完成前台请求。
// Transport 事件 ListenerUnsupported 的监听语义：迁移为 Unsupported。
// Transport 事件 ListenerUnsupported 的目录语义：从 Listening 丢失能力时按能力使目录失效。
// Transport 事件 ListenerUnsupported 的并发要求：事件分类、状态修改和完成竞争必须处于同一个 mutex_ 临界区。
// Transport 事件 ListenerUnsupported 的迟到要求：若 Client 已 Faulted、Closing 或 Closed，事件不得改写既有终态。
// Transport 事件 ListenerRecovering 的请求语义：表达可恢复监听缺口。
// Transport 事件 ListenerRecovering 的主状态语义：不完成前台请求。
// Transport 事件 ListenerRecovering 的监听语义：迁移为 Recovering 并保存监听错误。
// Transport 事件 ListenerRecovering 的目录语义：从 Listening 进入时递增目录代次。
// Transport 事件 ListenerRecovering 的并发要求：事件分类、状态修改和完成竞争必须处于同一个 mutex_ 临界区。
// Transport 事件 ListenerRecovering 的迟到要求：若 Client 已 Faulted、Closing 或 Closed，事件不得改写既有终态。
// Transport 事件 ListenerUnavailable 的请求语义：表达当前无法恢复监听。
// Transport 事件 ListenerUnavailable 的主状态语义：初始化阶段或协议类错误可触发主故障。
// Transport 事件 ListenerUnavailable 的监听语义：迁移为 Unavailable。
// Transport 事件 ListenerUnavailable 的目录语义：从 Listening 或 Recovering 进入时递增目录代次。
// Transport 事件 ListenerUnavailable 的并发要求：事件分类、状态修改和完成竞争必须处于同一个 mutex_ 临界区。
// Transport 事件 ListenerUnavailable 的迟到要求：若 Client 已 Faulted、Closing 或 Closed，事件不得改写既有终态。
// Transport 事件 ListenerStopped 的请求语义：表达非主动关闭导致的监听终止。
// Transport 事件 ListenerStopped 的主状态语义：以事件错误触发主故障。
// Transport 事件 ListenerStopped 的监听语义：迁移为 Stopped。
// Transport 事件 ListenerStopped 的目录语义：从 Listening 或 Recovering 进入时先递增目录代次。
// Transport 事件 ListenerStopped 的并发要求：事件分类、状态修改和完成竞争必须处于同一个 mutex_ 临界区。
// Transport 事件 ListenerStopped 的迟到要求：若 Client 已 Faulted、Closing 或 Closed，事件不得改写既有终态。
// Transport 事件 SessionExpired 的请求语义：表达 HTTP 会话 404 终止语义。
// Transport 事件 SessionExpired 的主状态语义：以 SessionExpired 固定首次主故障。
// Transport 事件 SessionExpired 的监听语义：主故障会停止 Listener 语义。
// Transport 事件 SessionExpired 的目录语义：旧目录随 Faulted 状态不可再拥有。
// Transport 事件 SessionExpired 的并发要求：事件分类、状态修改和完成竞争必须处于同一个 mutex_ 临界区。
// Transport 事件 SessionExpired 的迟到要求：若 Client 已 Faulted、Closing 或 Closed，事件不得改写既有终态。
// Transport 事件 ServerExited 的请求语义：表达 stdio 子进程意外退出。
// Transport 事件 ServerExited 的主状态语义：以 ServerExited 固定首次主故障。
// Transport 事件 ServerExited 的监听语义：stdio Listener 为 NotApplicable。
// Transport 事件 ServerExited 的目录语义：旧目录随 Faulted 状态不可再调用。
// Transport 事件 ServerExited 的并发要求：事件分类、状态修改和完成竞争必须处于同一个 mutex_ 临界区。
// Transport 事件 ServerExited 的迟到要求：若 Client 已 Faulted、Closing 或 Closed，事件不得改写既有终态。
// Transport 事件 TransportFault 的请求语义：承载其他闭合传输故障。
// Transport 事件 TransportFault 的主状态语义：只允许第一个致命事件固定 last_failure_code_。
// Transport 事件 TransportFault 的监听语义：HTTP 主故障会终止监听语义。
// Transport 事件 TransportFault 的目录语义：Faulted 阻止目录继续发布。
// Transport 事件 TransportFault 的并发要求：事件分类、状态修改和完成竞争必须处于同一个 mutex_ 临界区。
// Transport 事件 TransportFault 的迟到要求：若 Client 已 Faulted、Closing 或 Closed，事件不得改写既有终态。
// 公开操作 connect 的前置条件：仅允许从 Disconnected 进入。
// 公开操作 connect 的远端写入：initialize 响应与 initialized 发送。
// 公开操作 connect 的成功条件：首次 Listener 分类或 stdio 完成。
// 公开操作 connect 的失败语义：失败时统一关闭传输资源。
// 公开操作 connect 的并发槽：除 close 外由 ForegroundGuard 保证单个公开操作在途。
// 公开操作 connect 的异常边界：MCPException 在离开公开 API 前执行 UTF-8 字节上限清洗。
// 公开操作 ping 的前置条件：仅允许 Ready。
// 公开操作 ping 的远端写入：单个 JSON-RPC ping 请求。
// 公开操作 ping 的成功条件：空 result 响应。
// 公开操作 ping 的失败语义：已提交失败返回原错误码。
// 公开操作 ping 的并发槽：除 close 外由 ForegroundGuard 保证单个公开操作在途。
// 公开操作 ping 的异常边界：MCPException 在离开公开 API 前执行 UTF-8 字节上限清洗。
// 公开操作 listTools 的前置条件：Ready 且 Listener 可保证完整性。
// 公开操作 listTools 的远端写入：按 cursor 串行分页。
// 公开操作 listTools 的成功条件：完整无重复工具集合。
// 公开操作 listTools 的失败语义：代次变化时拒绝混合发布。
// 公开操作 listTools 的并发槽：除 close 外由 ForegroundGuard 保证单个公开操作在途。
// 公开操作 listTools 的异常边界：MCPException 在离开公开 API 前执行 UTF-8 字节上限清洗。
// 公开操作 callTool 的前置条件：Ready 且 Catalog 令牌仍有效。
// 公开操作 callTool 的远端写入：单个 tools/call 请求。
// 公开操作 callTool 的成功条件：保留完整远端 result。
// 公开操作 callTool 的失败语义：已提交失败提升 OutcomeUnknown。
// 公开操作 callTool 的并发槽：除 close 外由 ForegroundGuard 保证单个公开操作在途。
// 公开操作 callTool 的异常边界：MCPException 在离开公开 API 前执行 UTF-8 字节上限清洗。
// 公开操作 close 的前置条件：任何非 Closed 状态。
// 公开操作 close 的远端写入：不创建新协议公开操作。
// 公开操作 close 的成功条件：Transport 与控制线程收敛。
// 公开操作 close 的失败语义：并发调用共享一个关闭所有者和预算。
// 公开操作 close 的并发槽：除 close 外由 ForegroundGuard 保证单个公开操作在途。
// 公开操作 close 的异常边界：MCPException 在离开公开 API 前执行 UTF-8 字节上限清洗。
// 错误 VersionMismatch 的来源：协议版本无法协商。
// 错误 VersionMismatch 的状态结果：连接失败并固定为主故障。
// 错误 VersionMismatch 的信息边界：调用方依据枚举分支，不解析动态 what() 决策。
// 错误 CapabilityMissing 的来源：Server 未声明必需 Tools 能力。
// 错误 CapabilityMissing 的状态结果：连接失败并固定为主故障。
// 错误 CapabilityMissing 的信息边界：调用方依据枚举分支，不解析动态 what() 决策。
// 错误 ProtocolViolation 的来源：消息形状、ID 或状态迁移违反合同。
// 错误 ProtocolViolation 的状态结果：首次出现时使 Client Faulted。
// 错误 ProtocolViolation 的信息边界：调用方依据枚举分支，不解析动态 what() 决策。
// 错误 RemoteProtocolError 的来源：合法 JSON-RPC error 响应。
// 错误 RemoteProtocolError 的状态结果：不自动使 Client Faulted。
// 错误 RemoteProtocolError 的信息边界：调用方依据枚举分支，不解析动态 what() 决策。
// 错误 TransportFailure 的来源：闭合的通用传输失败。
// 错误 TransportFailure 的状态结果：按发生阶段决定是否 Faulted。
// 错误 TransportFailure 的信息边界：调用方依据枚举分支，不解析动态 what() 决策。
// 错误 RequestTimeout 的来源：请求段或绝对预算耗尽。
// 错误 RequestTimeout 的状态结果：已提交工具提升为 OutcomeUnknown。
// 错误 RequestTimeout 的信息边界：调用方依据枚举分支，不解析动态 what() 决策。
// 错误 ServerExited 的来源：stdio Server 意外退出。
// 错误 ServerExited 的状态结果：固定主故障并完成活动请求。
// 错误 ServerExited 的信息边界：调用方依据枚举分支，不解析动态 what() 决策。
// 错误 HttpStatusError 的来源：HTTP 返回不被协议接受的状态。
// 错误 HttpStatusError 的状态结果：按请求上下文完成或故障。
// 错误 HttpStatusError 的信息边界：调用方依据枚举分支，不解析动态 what() 决策。
// 错误 AuthenticationRequired 的来源：服务端要求有效凭据。
// 错误 AuthenticationRequired 的状态结果：不公开服务端正文或秘密。
// 错误 AuthenticationRequired 的信息边界：调用方依据枚举分支，不解析动态 what() 决策。
// 错误 CredentialUnavailable 的来源：Provider 未在预算内给出凭据。
// 错误 CredentialUnavailable 的状态结果：未提交时保持零远端写入。
// 错误 CredentialUnavailable 的信息边界：调用方依据枚举分支，不解析动态 what() 决策。
// 错误 OperationCancelled 的来源：公开 close 抢先完成操作。
// 错误 OperationCancelled 的状态结果：已提交工具作为 OutcomeUnknown cause。
// 错误 OperationCancelled 的信息边界：调用方依据枚举分支，不解析动态 what() 决策。
// 错误 ToolCatalogStale 的来源：目录代次、令牌或监听完整性失效。
// 错误 ToolCatalogStale 的状态结果：在提交前拒绝并保持零写入。
// 错误 ToolCatalogStale 的信息边界：调用方依据枚举分支，不解析动态 what() 决策。
// 错误 SessionExpired 的来源：带会话请求得到终止性 404。
// 错误 SessionExpired 的状态结果：固定主故障且不得透明重建。
// 错误 SessionExpired 的信息边界：调用方依据枚举分支，不解析动态 what() 决策。
// 错误 MessageLimitExceeded 的来源：消息字节或事件上限被突破。
// 错误 MessageLimitExceeded 的状态结果：按协议边界拒绝并可能 Faulted。
// 错误 MessageLimitExceeded 的信息边界：调用方依据枚举分支，不解析动态 what() 决策。
// 错误 MessageQueueOverflow 的来源：有界队列容量耗尽。
// 错误 MessageQueueOverflow 的状态结果：固定错误且禁止无界内存增长。
// 错误 MessageQueueOverflow 的信息边界：调用方依据枚举分支，不解析动态 what() 决策。
// 关闭合同：公开 close 的第一个锁内动作是判断 Closed，从而保持幂等快速路径。
// 关闭合同：首个关闭者在锁内把主状态写为 Closing，形成生命周期线性化点。
// 关闭合同：close 在任何 Transport I/O 之前完成当前活动请求。
// 关闭合同：close 对已提交工具使用 OperationCancelled 作为 OutcomeUnknown 的 cause。
// 关闭合同：close 对尚在 Provider 阶段的操作只改变状态，不制造 Submitted。
// 关闭合同：close 同时完成等待中的控制发送，避免 connect 等到完整请求超时。
// 关闭合同：close 设置 control_stop_ 后再通知条件变量，保证 Worker 可观察停止。
// 关闭合同：transport_close_started_ 只允许从 false 变为 true 一次。
// 关闭合同：首个关闭所有者固定 transport_close_deadline_，其他调用者不得延长。
// 关闭合同：所有并发 close 使用 wait_until 同一个绝对截止时间。
// 关闭合同：非所有者不得再次调用 Transport::close。
// 关闭合同：Transport::close 先于控制 Worker join，用于取消 Worker 的潜在 I/O。
// 关闭合同：控制 Worker 在停止时丢弃尚未提交的控制队列。
// 关闭合同：已弹出队列但未提交的控制任务会在二次状态校验处退出。
// 关闭合同：Worker 退出通过 control_worker_exited_ 在同一条件变量上发布。
// 关闭合同：最终 join 保证 Impl 析构前没有线程继续访问 this。
// 关闭合同：finishTransportClose 只负责发布资源关闭完成，不覆盖 Faulted 根因。
// 关闭合同：若公开 close 已将状态写为 Closing，关闭完成后才写 Closed。
// 关闭合同：若连接失败路径拥有关闭权，资源完成后主状态保持 Faulted。
// 关闭合同：Faulted 实例后续显式 close 可在资源已关闭时直接收敛到 Closed。
// 关闭合同：关闭期间到达的 Transport 事件在入口被忽略。
// 关闭合同：关闭期间到达的 JSON-RPC 同 ID 响应由完成槽或退休集合吸收。
// 关闭合同：关闭期间不得入队新的取消通知或 Server 响应。
// 关闭合同：close 不占 foreground_busy_，因此能取消正在等待的公开操作。
// 关闭合同：close 不等待 foreground_busy_ 清空才开始资源回收。
// 关闭合同：关闭预算来自配置 close_timeout，不从每个等待者重新计算。
// 关闭合同：close 是 noexcept，底层关闭异常不得越过接口边界。
// 关闭合同：Transport 合同要求 close 返回后不再回调 Client。
// 关闭合同：第二次公开 close 在 Closed 状态不触发条件变量等待。
// 关闭合同：资源关闭完成标志与状态写入都由 mutex_ 保护。
// 关闭合同：控制 pending 的首写错误不会被迟到 SendCompleted 清除。
// 关闭合同：close 不发送新的工具调用或目录请求。
// 关闭合同：初始化请求被 close 时不额外发送取消通知。
// 关闭合同：非初始化已提交请求的取消通知只做尽力发送。
// 关闭合同：控制队列计数在丢弃队列时同步扣减，防止容量永久泄漏。
// 关闭合同：即使共享截止时间已过，也必须以线程生命期安全为最终边界。
// 关闭合同：Transport 关闭所有者可能来自公开 close，也可能来自连接失败。
// 关闭合同：连接失败与公开 close 竞争时，先发生的主故障或取消决定异常语义。
// 关闭合同：主故障先于 close 时，last_failure_code_ 必须继续保留首因。
// 关闭合同：close 先于故障时，后续故障回调不得把取消改写为 Faulted。
// 目录合同：Catalog revision 只在 mutex_ 下递增。
// 目录合同：每次完整 listTools 发布都签发新的不可伪造令牌。
// 目录合同：旧令牌即使 revision 数值相同也不能重新获得所有权。
// 目录合同：list_changed 通知立即清除活动令牌与名称集合。
// 目录合同：Listening 进入 Recovering 代表通知连续性出现缺口。
// 目录合同：Recovering 进入 Unavailable 是新的可观察缺口并再次递增代次。
// 目录合同：Listening 直接进入 Unavailable 同样使目录失效。
// 目录合同：Listening 或 Recovering 意外 Stopped 时先使目录失效再 Faulted。
// 目录合同：初始化期间 Listener 失败不需要已发布目录也能触发主故障。
// 目录合同：405 Unsupported 是能力降级，不等同于通知连续性故障。
// 目录合同：目录回调在锁内复制，在锁外执行。
// 目录合同：用户目录回调异常被隔离，不改变协议状态。
// 目录合同：回调参数只包含 server_id 与新 revision。
// 目录合同：目录回调 revision 为零时不执行。
// 目录合同：分页开始时固定 start_revision。
// 目录合同：每页发送前再次检查 start_revision。
// 目录合同：每页解析后再次检查 start_revision。
// 目录合同：分页工具总量先用减法计算剩余容量，避免 size_t 回绕。
// 目录合同：协议解析器只接收剩余工具预算，不先构造超限页面。
// 目录合同：跨页重复工具名属于协议错误。
// 目录合同：重复 nextCursor 属于协议错误。
// 目录合同：达到 max_pages 仍有游标时返回分页上限错误。
// 目录合同：Listener 无法保证 listChanged 完整性时拒绝 listTools。
// 目录合同：目录 stale 时 callTool 在 prepare 前拒绝。
// 目录合同：Catalog server_id 不匹配时 callTool 在 prepare 前拒绝。
// 目录合同：Catalog revision 不匹配时 callTool 在 prepare 前拒绝。
// 目录合同：Catalog 签发令牌不匹配时 callTool 在 prepare 前拒绝。
// 目录合同：远端工具名不在活动名称集合时拒绝调用。
// 目录合同：Faulted 状态下 ownsCatalog 永远返回 false。
// 目录合同：Closed 状态下 ownsCatalog 永远返回 false。
// 控制合同：控制 Worker 是每个 Client 唯一的后台控制发送者。
// 控制合同：Server ping 响应与取消通知共用同一有界队列。
// 控制合同：控制任务入队前检查 control_stop_ 和终态。
// 控制合同：队列满时只有 fatal 控制任务会使 Client Faulted。
// 控制合同：普通取消通知入队失败不得覆盖原公开操作错误。
// 控制合同：Worker 等待谓词只依赖停止标志或非空队列。
// 控制合同：Worker 每次只弹出一个任务并在锁外准备消息。
// 控制合同：Provider 调用永远不持有 Client mutex_。
// 控制合同：准备完成后 Worker 回锁二次验证停止和终态。
// 控制合同：控制 dispatch ID 与 JSON-RPC request ID 相互独立。
// 控制合同：控制 dispatch ID 耗尽属于协议生命周期故障。
// 控制合同：控制发送 commitPrepared 仍遵守禁止同步回调合同。
// 控制合同：控制 pending 在提交前建立，避免异步完成丢失关联。
// 控制合同：控制发送完成只匹配 dispatch_id。
// 控制合同：无 waiter 的控制 pending 完成后立即擦除。
// 控制合同：有 waiter 的 initialized pending 由等待者擦除。
// 控制合同：fatal 控制发送失败只允许固定首个主故障。
// 控制合同：控制发送的迟到重复完成被 completed 标志吸收。
// 控制合同：关闭时未提交控制队列不再排空。
// 控制合同：关闭时已提交控制 pending 被 OperationCancelled 完成。
// 控制合同：主故障时控制 pending 以首次主故障完成。
// 控制合同：Worker 退出先发布 control_worker_exited_ 再返回。
// 控制合同：stopControlWorker 使用共享关闭截止时间等待退出。
// 控制合同：Worker 线程句柄只由关闭所有者 join。
// 控制合同：Worker 不按 Server 请求数量创建额外线程。
// 控制合同：pending_message_count_ 同时约束队列和瞬时响应接收。
// 控制合同：未知合法通知不占控制队列。
// 控制合同：未知 Server 请求生成 MethodNotFound 控制响应。
// 控制合同：Server ping 生成成功控制响应。
// 控制合同：控制回调与用户目录回调都不得在 Client 锁内执行。
// 并发证明：互斥证明：活动请求 ID、dispatch 和完成槽始终由同一 mutex_ 保护。
// 并发证明：唯一性证明：active_completion_ 为空检查与赋值处于同一临界区。
// 并发证明：响应先到：响应写入后错误、close 和故障只能看到非空完成槽。
// 并发证明：错误先到：错误写入后同 ID 响应在 acceptResponse 中直接返回。
// 并发证明：故障先到：markFaultLocked 先固定状态和首因，再尝试完成请求。
// 并发证明：close 先到：close 先写 Closing，再以取消尝试完成请求。
// 并发证明：超时先到：wait_until 返回时仍持锁，写入超时后回调无法插队。
// 并发证明：退休证明：完成记录复制和 ID 退休在同一个锁持有期内完成。
// 并发证明：状态快照证明：完成记录在首写时保存 state_at_completion。
// 并发证明：副作用证明：只有 commitPrepared 成功返回后才进入等待和 OutcomeUnknown 路径。
// 并发证明：零写入证明：Provider 后的 revalidate 在设置活动请求和 commit 之前执行。
// 并发证明：故障首因证明：markFaultLocked 对 Faulted 立即返回。
// 并发证明：关闭唯一性证明：transport_close_started_ 的检查和置位不可分割。
// 并发证明：关闭预算证明：deadline 只由首次关闭所有者写一次。
// 并发证明：目录缺口证明：Listener 迁移前保存 previous_listener_state。
// 并发证明：回调锁外证明：onTransportEvent 退出临界区后才调用目录回调。
// 并发证明：大 ID 证明：JSON unsigned 分支先于 signed 转换。
// 并发证明：容量证明：剩余工具数用 max_tools 减已收集数计算。
// 并发证明：控制首写证明：ControlPending completed 后忽略迟到完成。
// 并发证明：终态证明：Faulted、Closing、Closed 都不会迁移回 Ready。
// 并发证明：等待唤醒证明：每次完成、故障、关闭和 Listener 迁移都通知 cv_。
// 并发证明：消息边界证明：公开异常文本在 API 外层统一做 UTF-8 有界清洗。
// 并发证明：单前台证明：ForegroundGuard 在构造时原子检查并占用槽。
// 并发证明：后台独立证明：Listener 和控制 Worker 不创建 ForegroundGuard。
// 并发证明：异常安全证明：ForegroundGuard 析构总会释放公开操作槽。
// 并发证明：Transport 异常证明：非 MCP 底层异常被映射为闭合 TransportFailure。
// 并发证明：协议异常证明：已提交工具的协议错误提升为 OutcomeUnknown。
// 并发证明：远端错误证明：合法 RemoteProtocolError 不自动破坏连接。
// 并发证明：回调异常证明：用户目录回调异常不越过 noexcept 边界。
// 并发证明：析构证明：Impl 析构调用幂等 close，确保 Worker 不存活于对象之后。
// 评审清单：代码评审必须确认任何 active_request_id_ 写入都与 dispatch 和完成槽成组更新。
// 评审清单：代码评审必须确认任何 active_request_id_ 清除都伴随 ID 退休。
// 评审清单：代码评审必须确认响应成功路径不会读取更晚写入的 active_error。
// 评审清单：代码评审必须确认错误路径不会因后到响应返回成功。
// 评审清单：代码评审必须确认 close 的取消完成发生在 Transport::close 之前。
// 评审清单：代码评审必须确认工具 OutcomeUnknown 只出现在 Submitted 之后。
// 评审清单：代码评审必须确认准备失败不会错误标记 mayHaveExecuted。
// 评审清单：代码评审必须确认 Provider 调用点附近没有持有 mutex_。
// 评审清单：代码评审必须确认 Provider 返回后存在锁内二次状态校验。
// 评审清单：代码评审必须确认 Faulted 检查优先于普通状态不匹配 logic_error。
// 评审清单：代码评审必须确认连接 catch 不把已有首因改成 OperationCancelled。
// 评审清单：代码评审必须确认 throwConnectFailure 使用保存的首次错误码。
// 评审清单：代码评审必须确认 last_failure_code_ 只在第一次进入 Faulted 时写入。
// 评审清单：代码评审必须确认 Listener 错误码与主错误码分别保存。
// 评审清单：代码评审必须确认 Listener 缺口回调在 mutex_ 之外执行。
// 评审清单：代码评审必须确认 Listener 迁移使用事件前状态判断目录失效。
// 评审清单：代码评审必须确认 Recovering 到 Unavailable 会产生新的 revision。
// 评审清单：代码评审必须确认意外 Stopped 在主故障前完成目录失效。
// 评审清单：代码评审必须确认 405 Unsupported 不会错误使连接失败。
// 评审清单：代码评审必须确认 stdio 的 NotApplicable 不受 HTTP Listener 逻辑影响。
// 评审清单：代码评审必须确认 SendCompleted 不会完成前台 JSON-RPC 请求。
// 评审清单：代码评审必须确认 SendFailed 只匹配当前 dispatch。
// 评审清单：代码评审必须确认同 ID 迟到响应在完成槽非空时直接返回。
// 评审清单：代码评审必须确认退休 ID 响应不会触发新的协议故障。
// 评审清单：代码评审必须确认未知活动外 ID 仍会触发协议故障。
// 评审清单：代码评审必须确认 unsigned JSON ID 在任何 signed 转换之前处理。
// 评审清单：代码评审必须确认 max_tools 剩余容量计算没有加法回绕。
// 评审清单：代码评审必须确认分页解析器收到的容量不大于剩余预算。
// 评审清单：代码评审必须确认控制 pending 的 completed 也是 first-wins。
// 评审清单：代码评审必须确认 close 会唤醒 sendControlAndWait。
// 评审清单：代码评审必须确认 waitForInitialListener 识别 Closing 状态。
// 评审清单：代码评审必须确认所有 wait_until 谓词覆盖对应终态。
// 评审清单：代码评审必须确认条件变量通知发生在完成记录写入之后。
// 评审清单：代码评审必须确认并发 close 不重新计算独立截止时间。
// 评审清单：代码评审必须确认 transport_close_started_ 的检查与置位同锁。
// 评审清单：代码评审必须确认 transport_close_finished_ 发布后通知所有等待者。
// 评审清单：代码评审必须确认连接失败资源关闭与公开 close 不会重复调用 Transport。
// 评审清单：代码评审必须确认控制 Worker 停止时不继续提交排队任务。
// 评审清单：代码评审必须确认控制队列丢弃同步修正 pending_message_count_。
// 评审清单：代码评审必须确认控制 Worker 退出标志在 join 等待前可观察。
// 评审清单：代码评审必须确认最终 join 不在持有 mutex_ 时执行。
// 评审清单：代码评审必须确认 Transport::close 不在持有 mutex_ 时执行。
// 评审清单：代码评审必须确认用户回调不在持有 mutex_ 时执行。
// 评审清单：代码评审必须确认异常文本清洗不改变 code、cause 和副作用标记。
// 评审清单：代码评审必须确认 RemoteProtocolError 不被误当作主协议故障。
// 评审清单：代码评审必须确认 ProtocolViolation 对已提交工具提升结果未知。
// 评审清单：代码评审必须确认 ForegroundGuard 在所有异常路径释放槽。
// 评审清单：代码评审必须确认 close 不受 ForegroundGuard 限制。
// 评审清单：代码评审必须确认 Faulted 和 Closed 都不能被再次 connect。
// 评审清单：代码评审必须以确定性屏障测试覆盖新增的任何并发分支。
// 合同说明结束；修改请求、故障、Listener 或关闭路径时，必须同步保持上述矩阵成立。

// minDeadline 让请求段上限和公开操作绝对上限同时运行，先到者生效。
std::chrono::steady_clock::time_point minDeadline(std::chrono::steady_clock::time_point left,
                                                  std::chrono::steady_clock::time_point right) {
    return left < right ? left : right;
}

}  // namespace

class MCPClient::Impl {
   public:
    Impl(MCPServerConfig config, std::shared_ptr<IMCPTransport> transport)
        : config_(std::move(config)), transport_(std::move(transport)) {
        // 构造阶段只做确定性校验和依赖固定，绝不启动进程、线程或网络请求。
        validateMCPServerConfig(config_);
        if(!transport_) {
            if(const auto* stdio = std::get_if<MCPStdioServerConfig>(&config_.transport)) {
                transport_ = detail::createStdioMCPTransport(*stdio, config_.limits);
            } else {
                transport_ = detail::createStreamableHttpMCPTransport(
                    std::get<MCPStreamableHttpConfig>(config_.transport), config_.limits);
            }
        }
        if(!transport_) {
            throw std::invalid_argument("MCP Transport 不能为空");
        }
    }

    ~Impl() noexcept {
        close();
    }

    void connect() {
        ForegroundGuard foreground(*this);
        const auto absolute_deadline = detail::saturatingSteadyDeadlineAfter(config_.limits.absolute_request_timeout);
        auto initialization_deadline = absolute_deadline;
        if(const auto* stdio = std::get_if<MCPStdioServerConfig>(&config_.transport)) {
            // stdio startup_timeout 从开始创建进程起覆盖完整握手，与公开绝对上限同时运行。
            initialization_deadline =
                minDeadline(initialization_deadline, detail::saturatingSteadyDeadlineAfter(stdio->startup_timeout));
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(state_ != MCPClientState::Disconnected) {
                throw std::logic_error("MCPClient 只能从 Disconnected 状态连接");
            }
            state_ = MCPClientState::Connecting;
            listener_state_ = isHttp() ? MCPListenerState::Starting : MCPListenerState::NotApplicable;
            startInboundWorkerLocked();
            startControlWorkerLocked();
        }

        try {
            // 回调只把消息交给 Client 的有界状态机；Transport 合同保证在自身锁外调用。
            transport_->open({[this](nlohmann::json message) { onMessage(std::move(message)); },
                              [this](MCPTransportEvent event) { onTransportEvent(std::move(event)); }},
                             initialization_deadline);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ensureNotClosingLocked();
                if(state_ == MCPClientState::Faulted) {
                    const auto code = last_failure_code_.value_or(MCPErrorCode::TransportFailure);
                    throw MCPException(code, state_, requestFailureMessage(code));
                }
                if(state_ != MCPClientState::Connecting) {
                    throw MCPException(MCPErrorCode::ProtocolViolation, state_,
                                       "MCP Transport 打开期间出现非法状态迁移");
                }
                state_ = MCPClientState::Initializing;
            }

            const std::uint64_t initialize_id = allocateRequestId();
            const auto initialize_response =
                sendRequest(detail::makeInitializeRequest(initialize_id), MCPTransportRequestKind::Initialize,
                            initialize_id, initialization_deadline, false,
                            [this] { ensureStateLocked(MCPClientState::Initializing, "MCP 初始化状态已失效"); });
            const auto initialize_result =
                detail::parseInitializeResponse(initialize_response, initialize_id, MCPClientState::Initializing);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                ensureStateLocked(MCPClientState::Initializing, "MCP 初始化状态已失效");
                tools_list_changed_capability_ = initialize_result.tools_list_changed;
            }
            transport_->completeInitialization(initialize_result.protocol_version);
            sendControlAndWait(detail::makeInitializedNotification(), MCPTransportRequestKind::InitializedNotification,
                               initialization_deadline, [this] {
                                   ensureStateLocked(MCPClientState::Initializing, "MCP initialized 通知状态已失效");
                               });

            if(isHttp()) {
                // 首次 GET 的响应头分类属于 connect()，长期流本身移交 Transport 后台任务。
                transport_->startListener(minDeadline(
                    detail::saturatingSteadyDeadlineAfter(config_.limits.request_timeout), initialization_deadline));
                waitForInitialListener(initialization_deadline);
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ensureStateLocked(MCPClientState::Initializing, "MCP Listener 初始化期间连接状态已失效");
                state_ = MCPClientState::Ready;
            }
        } catch(const MCPException& exception) {
            throwConnectFailure(exception.code(), exception.what());
        } catch(const std::logic_error&) {
            MCPErrorCode code = MCPErrorCode::ProtocolViolation;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // 状态在 Provider/准备阶段已经故障时，必须保留第一个真实根因。
                if(last_failure_code_.has_value()) {
                    code = *last_failure_code_;
                } else if(state_ == MCPClientState::Closing || state_ == MCPClientState::Closed) {
                    code = MCPErrorCode::OperationCancelled;
                }
            }
            throwConnectFailure(code, requestFailureMessage(code));
        } catch(const std::exception&) {
            // 底层异常文本可能含路径或网络细节，因此只公开固定传输分类。
            throwConnectFailure(MCPErrorCode::TransportFailure, "MCP Transport 初始化失败");
        }
    }

    void ping() {
        ForegroundGuard foreground(*this);
        const auto absolute_deadline = detail::saturatingSteadyDeadlineAfter(config_.limits.absolute_request_timeout);
        const std::uint64_t request_id = allocateReadyRequestId();
        const auto response = sendRequest(
            detail::makePingRequest(request_id), MCPTransportRequestKind::Ping, request_id, absolute_deadline, false,
            [this] { ensureStateLocked(MCPClientState::Ready, "MCP ping 只能在 Ready 状态调用"); });
        try {
            detail::parseEmptyResponse(response, request_id, currentState());
        } catch(const MCPException& exception) {
            handleParsedResponseFailure(exception, false);
        }
    }

    MCPToolCatalog listTools() {
        ForegroundGuard foreground(*this);
        const auto absolute_deadline = detail::saturatingSteadyDeadlineAfter(config_.limits.absolute_request_timeout);
        std::uint64_t start_revision = 0U;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ensureStateLocked(MCPClientState::Ready, "MCP tools/list 只能在 Ready 状态调用");
            ensureListenerSafeForCatalogLocked();
            start_revision = catalog_revision_;
        }

        std::vector<MCPTool> collected_tools;
        std::unordered_set<std::string> tool_names;
        std::unordered_set<std::string> seen_cursors;
        std::optional<std::string> cursor;
        for(std::size_t page_index = 0U; page_index < config_.limits.max_pages; ++page_index) {
            const std::uint64_t request_id = allocateRequestId();
            const auto response =
                sendRequest(detail::makeToolsListRequest(request_id, cursor), MCPTransportRequestKind::ListTools,
                            request_id, absolute_deadline, false, [this, start_revision] {
                                ensureStateLocked(MCPClientState::Ready, "MCP tools/list 分页期间连接状态已失效");
                                ensureCatalogRevisionLocked(start_revision);
                                ensureListenerSafeForCatalogLocked();
                            });

            detail::MCPToolsPage page;
            try {
                if(collected_tools.size() > config_.limits.max_tools) {
                    throw MCPException(MCPErrorCode::PaginationLimitExceeded, currentState(),
                                       "MCP tools/list 工具总数超过配置上限");
                }
                page = detail::parseToolsListResponse(response, request_id, currentState(),
                                                      config_.limits.max_tools - collected_tools.size());
            } catch(const MCPException& exception) {
                handleParsedResponseFailure(exception, false);
            }

            // 每页解析后再次检查代次，避免通知与响应同时到达时发布跨代次混合目录。
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ensureCatalogRevisionLocked(start_revision);
                ensureListenerSafeForCatalogLocked();
            }
            // 先做减法再比较，避免来自不可信分页响应的长度相加发生 size_t 回绕。
            if(collected_tools.size() > config_.limits.max_tools ||
               page.tools.size() > config_.limits.max_tools - collected_tools.size()) {
                throw MCPException(MCPErrorCode::PaginationLimitExceeded, currentState(),
                                   "MCP tools/list 工具总数超过配置上限");
            }
            for(auto& tool : page.tools) {
                if(!tool_names.insert(tool.name).second) {
                    markProtocolFaultAndThrow("MCP tools/list 跨页返回了重复工具名称", false);
                }
                collected_tools.push_back(std::move(tool));
            }
            if(!page.next_cursor.has_value()) {
                return publishCatalog(start_revision, std::move(collected_tools));
            }
            if(!seen_cursors.insert(*page.next_cursor).second) {
                markProtocolFaultAndThrow("MCP tools/list 返回了重复分页游标", false);
            }
            cursor = std::move(page.next_cursor);
        }
        throw MCPException(MCPErrorCode::PaginationLimitExceeded, currentState(), "MCP tools/list 页数超过配置上限");
    }

    MCPToolCallResult callTool(const MCPToolCatalog& catalog, const std::string& remote_name,
                               const nlohmann::json& arguments) {
        ForegroundGuard foreground(*this);
        if(remote_name.empty()) {
            throw std::invalid_argument("MCP 远端工具名称不能为空");
        }
        if(!arguments.is_object()) {
            throw std::invalid_argument("MCP 工具参数必须是 JSON 对象");
        }
        const auto absolute_deadline = detail::saturatingSteadyDeadlineAfter(config_.limits.absolute_request_timeout);

        std::shared_ptr<const void> expected_catalog_token;
        std::uint64_t expected_revision = 0U;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            validateCatalogCallLocked(catalog, remote_name);
            expected_catalog_token = MCPToolCatalogAccess::issuerToken(catalog);
            expected_revision = MCPToolCatalogAccess::revision(catalog);
        }

        const std::uint64_t request_id = allocateRequestId();
        const auto response = sendRequest(
            detail::makeToolsCallRequest(request_id, remote_name, arguments), MCPTransportRequestKind::CallTool,
            request_id, absolute_deadline, true, [this, expected_catalog_token, expected_revision, remote_name] {
                ensureStateLocked(MCPClientState::Ready, "MCP tools/call 只能在 Ready 状态调用");
                ensureListenerSafeForCatalogLocked();
                if(catalog_stale_ || catalog_revision_ != expected_revision ||
                   active_catalog_token_ != expected_catalog_token ||
                   active_catalog_names_.find(remote_name) == active_catalog_names_.end()) {
                    throw MCPException(MCPErrorCode::ToolCatalogStale, state_, "MCP 工具目录已经失效");
                }
            });
        try {
            return detail::parseToolsCallResponse(response, request_id, currentState());
        } catch(const MCPException& exception) {
            handleParsedResponseFailure(exception, true);
        }
    }

    void close() noexcept {
        bool owns_transport_close = false;
        std::chrono::steady_clock::time_point close_deadline;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(state_ == MCPClientState::Closed) {
                return;
            }
            if(state_ != MCPClientState::Closing) {
                state_ = MCPClientState::Closing;
                listener_state_ =
                    listener_state_ == MCPListenerState::NotApplicable ? listener_state_ : MCPListenerState::Stopped;
                // close 在 Transport I/O 之前就确定当前请求，迟到响应只能被忽略。
                completeActiveRequestWithErrorLocked(MCPErrorCode::OperationCancelled);
                completePendingControlsWithErrorLocked(MCPErrorCode::OperationCancelled);
                inbound_stop_ = true;
                discardInboundMessagesLocked();
            }
            control_stop_ = true;
            if(!transport_close_started_) {
                transport_close_started_ = true;
                transport_close_deadline_ = detail::saturatingSteadyDeadlineAfter(config_.limits.close_timeout);
                owns_transport_close = true;
            }
            close_deadline = transport_close_deadline_;
            cv_.notify_all();
        }

        if(owns_transport_close) {
            // Transport 自身负责会话 DELETE、进程树终止和后台线程的有界收敛。
            transport_->close(close_deadline);
            stopInboundWorker(close_deadline);
            stopControlWorker(close_deadline);
            finishTransportClose();
            return;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        if(transport_close_finished_) {
            state_ = MCPClientState::Closed;
            return;
        }
        // 所有并发 close 共享第一个关闭者确定的截止时间，不逐次叠加预算。
        cv_.wait_until(lock, close_deadline, [this] { return transport_close_finished_; });
        if(transport_close_finished_) {
            state_ = MCPClientState::Closed;
        }
    }

    MCPClientState state() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    MCPListenerState listenerState() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return listener_state_;
    }

    std::optional<MCPErrorCode> lastFailureCode() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_failure_code_;
    }

    std::optional<MCPErrorCode> lastListenerFailureCode() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_listener_failure_code_;
    }

    std::uint64_t catalogRevision() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return catalog_revision_;
    }

    bool isToolCatalogStale() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return catalog_stale_;
    }

    bool ownsCatalog(const MCPToolCatalog& catalog) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_ == MCPClientState::Ready && catalog.valid() && !catalog_stale_ &&
               MCPToolCatalogAccess::serverId(catalog) == config_.server_id &&
               MCPToolCatalogAccess::revision(catalog) == catalog_revision_ &&
               MCPToolCatalogAccess::issuerToken(catalog) == active_catalog_token_;
    }

    const std::string& serverId() const noexcept {
        // server_id 在构造后不可变，因此返回引用不需要持有状态锁。
        return config_.server_id;
    }

    void setCatalogChangedCallback(CatalogChangedCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        catalog_changed_callback_ = std::move(callback);
    }

    // throwBoundedPublicError 是所有同步公开操作离开 Client 前的异常文本边界。
    // 结构化错误码、根因、副作用标记和线性化状态保持不变，调用方无需解析 what()。
    [[noreturn]] void throwBoundedPublicError(const MCPException& exception) const {
        throw MCPException(exception.code(), exception.clientStateAtFailure(),
                           detail::sanitizeMCPErrorText(exception.what(), config_.limits.max_error_text_bytes),
                           exception.causeCode(), exception.mayHaveExecuted());
    }

   private:
    class ForegroundGuard {
       public:
        explicit ForegroundGuard(Impl& owner) : owner_(owner) {
            std::lock_guard<std::mutex> lock(owner_.mutex_);
            if(owner_.foreground_busy_) {
                throw MCPException(MCPErrorCode::ClientBusy, owner_.state_, "该 MCPClient 已有公开操作在途");
            }
            owner_.foreground_busy_ = true;
        }

        ~ForegroundGuard() {
            // 前台槽只覆盖用户公开操作；Listener、控制响应、状态读取和 close 不使用它。
            std::lock_guard<std::mutex> lock(owner_.mutex_);
            owner_.foreground_busy_ = false;
            owner_.cv_.notify_all();
        }

        ForegroundGuard(const ForegroundGuard&) = delete;
        ForegroundGuard& operator=(const ForegroundGuard&) = delete;

       private:
        Impl& owner_;
    };

    struct ControlTask {
        nlohmann::json message;
        MCPTransportRequestKind kind = MCPTransportRequestKind::ServerResponse;
        bool fatal_on_failure = false;
    };

    struct ControlPending {
        bool completed = false;
        std::optional<MCPErrorCode> error;
        bool has_waiter = false;
        bool fatal_on_failure = false;
    };

    struct ActiveRequestCompletion {
        // response 与 error 严格二选一；整个结构只允许在 mutex_ 下首次写入。
        std::optional<nlohmann::json> response;
        std::optional<MCPErrorCode> error;
        MCPClientState state_at_completion = MCPClientState::Disconnected;
    };

    bool isHttp() const noexcept {
        return std::holds_alternative<MCPStreamableHttpConfig>(config_.transport);
    }

    MCPClientState currentState() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    void ensureStateLocked(MCPClientState expected, const char* message) const {
        if(state_ != expected) {
            if(state_ == MCPClientState::Faulted) {
                const auto code = last_failure_code_.value_or(MCPErrorCode::TransportFailure);
                throw MCPException(code, state_, requestFailureMessage(code));
            }
            if(state_ == MCPClientState::Closing || state_ == MCPClientState::Closed) {
                throw MCPException(MCPErrorCode::OperationCancelled, state_, "MCP 操作已被关闭取消");
            }
            throw std::logic_error(message);
        }
    }

    void ensureNotClosingLocked() const {
        if(state_ == MCPClientState::Faulted) {
            const auto code = last_failure_code_.value_or(MCPErrorCode::TransportFailure);
            throw MCPException(code, state_, requestFailureMessage(code));
        }
        if(state_ == MCPClientState::Closing || state_ == MCPClientState::Closed) {
            throw MCPException(MCPErrorCode::OperationCancelled, state_, "MCP 操作已被关闭取消");
        }
    }

    void ensureCatalogRevisionLocked(std::uint64_t expected_revision) const {
        if(catalog_revision_ != expected_revision) {
            throw MCPException(MCPErrorCode::ToolCatalogStale, state_, "MCP 工具目录在列举期间发生变化");
        }
    }

    void ensureListenerSafeForCatalogLocked() const {
        if(tools_list_changed_capability_ &&
           (listener_state_ == MCPListenerState::Recovering || listener_state_ == MCPListenerState::Unavailable)) {
            throw MCPException(MCPErrorCode::ToolCatalogStale, state_, "MCP Listener 当前无法保证工具目录完整性");
        }
    }

    void throwTerminalStateAfterPrepareIfNeeded() const {
        std::lock_guard<std::mutex> lock(mutex_);
        // Provider 和序列化不持有 Client 锁；返回边界必须重新观测终态。
        if(state_ == MCPClientState::Faulted) {
            const auto code = last_failure_code_.value_or(MCPErrorCode::TransportFailure);
            throw MCPException(code, state_, requestFailureMessage(code));
        }
        if(state_ == MCPClientState::Closing || state_ == MCPClientState::Closed) {
            throw MCPException(MCPErrorCode::OperationCancelled, state_, "MCP 操作在准备阶段被关闭取消");
        }
    }

    std::uint64_t allocateRequestId() {
        std::lock_guard<std::mutex> lock(mutex_);
        if(next_request_id_ == std::numeric_limits<std::uint64_t>::max()) {
            markFaultLocked(MCPErrorCode::ProtocolViolation);
            throw MCPException(MCPErrorCode::ProtocolViolation, state_, "MCP 请求 ID 已耗尽");
        }
        return next_request_id_++;
    }

    std::uint64_t allocateReadyRequestId() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensureStateLocked(MCPClientState::Ready, "MCP ping 只能在 Ready 状态调用");
        if(next_request_id_ == std::numeric_limits<std::uint64_t>::max()) {
            markFaultLocked(MCPErrorCode::ProtocolViolation);
            throw MCPException(MCPErrorCode::ProtocolViolation, state_, "MCP 请求 ID 已耗尽");
        }
        return next_request_id_++;
    }

    std::uint64_t allocateDispatchIdLocked() {
        if(next_dispatch_id_ == std::numeric_limits<std::uint64_t>::max()) {
            markFaultLocked(MCPErrorCode::ProtocolViolation);
            throw MCPException(MCPErrorCode::ProtocolViolation, state_, "MCP 内部发送 ID 已耗尽");
        }
        return next_dispatch_id_++;
    }

    nlohmann::json sendRequest(const nlohmann::json& message, MCPTransportRequestKind kind, std::uint64_t request_id,
                               std::chrono::steady_clock::time_point absolute_deadline, bool is_tool_call,
                               const std::function<void()>& revalidate_locked) {
        std::uint64_t dispatch_id = 0U;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dispatch_id = allocateDispatchIdLocked();
        }
        // 准备阶段允许凭据 Provider 运行，但只消耗公开操作的绝对上限。
        // request_timeout 必须从原子提交开始，不能把尚未发送的准备工作算作请求段。
        MCPTransportRequestContext context{kind, dispatch_id, absolute_deadline, absolute_deadline};

        std::unique_ptr<IMCPPreparedMessage> prepared;
        try {
            prepared = transport_->prepareMessage(message, context);
        } catch(const MCPException&) {
            throwTerminalStateAfterPrepareIfNeeded();
            throw;
        } catch(const std::exception&) {
            throwTerminalStateAfterPrepareIfNeeded();
            throw MCPException(MCPErrorCode::TransportFailure, currentState(), "MCP 请求准备失败");
        }
        if(!prepared) {
            throw MCPException(MCPErrorCode::TransportFailure, currentState(), "MCP Transport 返回了空准备消息");
        }

        std::chrono::steady_clock::time_point segment_deadline;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            revalidate_locked();
            if(std::chrono::steady_clock::now() >= absolute_deadline) {
                throw MCPException(MCPErrorCode::RequestTimeout, state_, "MCP 公开操作超过绝对时间上限");
            }
            // 请求段在线性化提交点取时；同一时间点同时传给 Transport 与 Client 等待逻辑。
            segment_deadline =
                minDeadline(detail::saturatingSteadyDeadlineAfter(config_.limits.request_timeout), absolute_deadline);
            active_request_id_ = request_id;
            active_dispatch_id_ = dispatch_id;
            active_completion_.reset();
            // 普通 JSON/stdio 维持请求段；仅收到已校验的 POST SSE 建流事件后才扩展等待。
            active_response_deadline_ = segment_deadline;
            active_operation_deadline_ = absolute_deadline;
            try {
                // 成功返回即建立 Submitted 线性化点；合同禁止这里同步回调 Client。
                transport_->commitPrepared(std::move(prepared), segment_deadline);
            } catch(const MCPException& exception) {
                active_request_id_.reset();
                active_dispatch_id_.reset();
                if(exception.code() == MCPErrorCode::MessageQueueOverflow) {
                    markFaultLocked(MCPErrorCode::MessageQueueOverflow);
                }
                throw MCPException(exception.code(), state_, exception.what());
            } catch(const std::exception&) {
                active_request_id_.reset();
                active_dispatch_id_.reset();
                throw MCPException(MCPErrorCode::TransportFailure, state_, "MCP 请求提交失败");
            }
        }

        std::unique_lock<std::mutex> lock(mutex_);
        // 提交成功后才允许等待该段截止时间；失败路径没有任何物理请求可等待。
        bool completed = false;
        for(;;) {
            const auto wait_deadline = active_response_deadline_;
            const bool awakened = cv_.wait_until(lock, wait_deadline, [this, wait_deadline] {
                return active_completion_.has_value() || state_ == MCPClientState::Faulted ||
                       state_ == MCPClientState::Closing || state_ == MCPClientState::Closed ||
                       active_response_deadline_ != wait_deadline;
            });
            if(active_completion_.has_value() || state_ == MCPClientState::Faulted ||
               state_ == MCPClientState::Closing || state_ == MCPClientState::Closed) {
                completed = true;
                break;
            }
            if(awakened && active_response_deadline_ != wait_deadline) {
                // SSE 响应头建流事件已更新截止时间，重新以绝对上限等待事件、恢复或终局响应。
                continue;
            }
            break;
        }

        if(!active_completion_.has_value()) {
            // wait_until 返回时仍持有同一状态锁，因此超时也是与响应/错误/close 竞争的首写者。
            const auto code = !completed ? MCPErrorCode::RequestTimeout
                              : (state_ == MCPClientState::Closing || state_ == MCPClientState::Closed)
                                  ? MCPErrorCode::OperationCancelled
                                  : last_failure_code_.value_or(MCPErrorCode::TransportFailure);
            active_completion_ = ActiveRequestCompletion{std::nullopt, code, state_};
        }
        ActiveRequestCompletion completion = std::move(*active_completion_);
        if(!completion.response.has_value()) {
            // 只有结果未知的请求允许吸收迟到响应；已知终局结果后的重复响应必须暴露协议错误。
            retireRequestLocked(request_id);
        }
        active_request_id_.reset();
        active_dispatch_id_.reset();
        active_completion_.reset();
        active_response_deadline_ = {};
        active_operation_deadline_ = {};
        lock.unlock();

        if(completion.response.has_value()) {
            return std::move(*completion.response);
        }

        // 初始化不能发送取消；其他已提交请求使用同一控制队列做尽力通知。
        if(kind != MCPTransportRequestKind::Initialize) {
            enqueueCancellation(request_id);
        }
        const MCPErrorCode cause = completion.error.value_or(MCPErrorCode::TransportFailure);
        if(is_tool_call) {
            throw MCPException(MCPErrorCode::OutcomeUnknown, completion.state_at_completion,
                               "MCP 工具结果未知，工具可能已执行，请勿自动重试", cause, true);
        }
        throw MCPException(cause, completion.state_at_completion, requestFailureMessage(cause));
    }

    void sendControlAndWait(const nlohmann::json& message, MCPTransportRequestKind kind,
                            std::chrono::steady_clock::time_point absolute_deadline,
                            const std::function<void()>& revalidate_locked) {
        std::uint64_t dispatch_id = 0U;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dispatch_id = allocateDispatchIdLocked();
        }
        std::unique_ptr<IMCPPreparedMessage> prepared;
        try {
            // 控制消息的凭据准备同样只受外层操作上限约束，不抢占实际发送段预算。
            prepared = transport_->prepareMessage(message, {kind, dispatch_id, absolute_deadline, absolute_deadline});
        } catch(const MCPException&) {
            throwTerminalStateAfterPrepareIfNeeded();
            throw;
        } catch(const std::exception&) {
            throwTerminalStateAfterPrepareIfNeeded();
            throw MCPException(MCPErrorCode::TransportFailure, currentState(), "MCP 控制消息准备失败");
        }
        if(!prepared) {
            throw MCPException(MCPErrorCode::TransportFailure, currentState(), "MCP 控制消息准备失败");
        }

        std::chrono::steady_clock::time_point deadline;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            revalidate_locked();
            if(std::chrono::steady_clock::now() >= absolute_deadline) {
                throw MCPException(MCPErrorCode::RequestTimeout, state_, "MCP 控制消息超过绝对时间上限");
            }
            deadline =
                minDeadline(detail::saturatingSteadyDeadlineAfter(config_.limits.request_timeout), absolute_deadline);
            control_pending_.emplace(dispatch_id, ControlPending{false, std::nullopt, true, true});
            try {
                transport_->commitPrepared(std::move(prepared), deadline);
            } catch(const MCPException& exception) {
                control_pending_.erase(dispatch_id);
                if(exception.code() == MCPErrorCode::MessageQueueOverflow) {
                    markFaultLocked(MCPErrorCode::MessageQueueOverflow);
                }
                throw MCPException(exception.code(), state_, exception.what());
            } catch(const std::exception&) {
                control_pending_.erase(dispatch_id);
                throw MCPException(MCPErrorCode::TransportFailure, state_, "MCP 控制消息提交失败");
            }
        }

        std::unique_lock<std::mutex> lock(mutex_);
        const bool completed = cv_.wait_until(lock, deadline, [this, dispatch_id] {
            const auto pending = control_pending_.find(dispatch_id);
            return pending == control_pending_.end() || pending->second.completed ||
                   state_ == MCPClientState::Faulted || state_ == MCPClientState::Closing ||
                   state_ == MCPClientState::Closed;
        });
        auto pending = control_pending_.find(dispatch_id);
        std::optional<MCPErrorCode> error;
        if(pending != control_pending_.end()) {
            error = pending->second.error;
            control_pending_.erase(pending);
        }
        if(!completed) {
            throw MCPException(MCPErrorCode::RequestTimeout, state_, "MCP 控制消息发送超时");
        }
        if(error.has_value()) {
            throw MCPException(*error, state_, requestFailureMessage(*error));
        }
        if(state_ == MCPClientState::Faulted) {
            const auto code = last_failure_code_.value_or(MCPErrorCode::TransportFailure);
            throw MCPException(code, state_, requestFailureMessage(code));
        }
        if(state_ == MCPClientState::Closing || state_ == MCPClientState::Closed) {
            throw MCPException(MCPErrorCode::OperationCancelled, state_, "MCP 控制消息被关闭取消");
        }
    }

    void waitForInitialListener(std::chrono::steady_clock::time_point absolute_deadline) {
        const auto deadline =
            minDeadline(detail::saturatingSteadyDeadlineAfter(config_.limits.request_timeout), absolute_deadline);
        std::unique_lock<std::mutex> lock(mutex_);
        const bool completed = cv_.wait_until(lock, deadline, [this] {
            return listener_state_ != MCPListenerState::Starting || state_ == MCPClientState::Faulted ||
                   state_ == MCPClientState::Closing || state_ == MCPClientState::Closed;
        });
        if(!completed) {
            throw MCPException(MCPErrorCode::RequestTimeout, state_, "MCP 首次 Listener GET 响应头超时");
        }
        if(state_ == MCPClientState::Faulted) {
            const auto code = last_failure_code_.value_or(MCPErrorCode::TransportFailure);
            throw MCPException(code, state_, requestFailureMessage(code));
        }
        if(state_ == MCPClientState::Closing || state_ == MCPClientState::Closed) {
            throw MCPException(MCPErrorCode::OperationCancelled, state_, "MCP Listener 初始化被关闭取消");
        }
        if(listener_state_ != MCPListenerState::Listening && listener_state_ != MCPListenerState::Unsupported) {
            const auto code = last_listener_failure_code_.value_or(MCPErrorCode::TransportFailure);
            throw MCPException(code, state_, requestFailureMessage(code));
        }
    }

    [[noreturn]] void throwConnectFailure(MCPErrorCode code, const char* message) {
        const MCPErrorCode reported_code = code;
        MCPClientState failure_state = MCPClientState::Faulted;
        bool owns_transport_close = false;
        std::chrono::steady_clock::time_point close_deadline;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if((state_ == MCPClientState::Closing || state_ == MCPClientState::Closed) &&
               !last_failure_code_.has_value()) {
                failure_state = MCPClientState::Closed;
                code = MCPErrorCode::OperationCancelled;
            } else {
                markFaultLocked(code);
                code = last_failure_code_.value_or(code);
                failure_state = MCPClientState::Faulted;
                if(!transport_close_started_) {
                    transport_close_started_ = true;
                    transport_close_deadline_ = detail::saturatingSteadyDeadlineAfter(config_.limits.close_timeout);
                    control_stop_ = true;
                    inbound_stop_ = true;
                    discardInboundMessagesLocked();
                    owns_transport_close = true;
                }
            }
            close_deadline = transport_close_started_ ? transport_close_deadline_ : std::chrono::steady_clock::now();
            cv_.notify_all();
        }

        if(owns_transport_close) {
            transport_->close(close_deadline);
            stopInboundWorker(close_deadline);
            stopControlWorker(close_deadline);
            finishTransportClose();
        } else if(failure_state == MCPClientState::Faulted) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_until(lock, close_deadline, [this] { return transport_close_finished_; });
        }
        const char* selected_message = failure_state == MCPClientState::Closed
                                           ? "MCP 连接被关闭取消"
                                           : (code == reported_code ? message : requestFailureMessage(code));
        throw MCPException(code, failure_state, selected_message);
    }

    [[noreturn]] void handleParsedResponseFailure(const MCPException& exception, bool submitted_tool) {
        if(exception.code() == MCPErrorCode::RemoteProtocolError) {
            throw MCPException(exception.code(), currentState(), exception.what());
        }
        if(exception.code() == MCPErrorCode::ProtocolViolation) {
            MCPClientState failure_state;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                markFaultLocked(MCPErrorCode::ProtocolViolation);
                failure_state = state_;
            }
            if(submitted_tool) {
                throw MCPException(MCPErrorCode::OutcomeUnknown, failure_state,
                                   "MCP 工具结果未知，工具可能已执行，请勿自动重试", MCPErrorCode::ProtocolViolation,
                                   true);
            }
            throw MCPException(MCPErrorCode::ProtocolViolation, failure_state, exception.what());
        }
        throw MCPException(exception.code(), currentState(), exception.what());
    }

    [[noreturn]] void markProtocolFaultAndThrow(const char* message, bool submitted_tool) {
        MCPClientState failure_state;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            markFaultLocked(MCPErrorCode::ProtocolViolation);
            failure_state = state_;
        }
        if(submitted_tool) {
            throw MCPException(MCPErrorCode::OutcomeUnknown, failure_state,
                               "MCP 工具结果未知，工具可能已执行，请勿自动重试", MCPErrorCode::ProtocolViolation, true);
        }
        throw MCPException(MCPErrorCode::ProtocolViolation, failure_state, message);
    }

    MCPToolCatalog publishCatalog(std::uint64_t expected_revision, std::vector<MCPTool> tools) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensureStateLocked(MCPClientState::Ready, "MCP 目录发布时连接状态已失效");
        ensureCatalogRevisionLocked(expected_revision);
        ensureListenerSafeForCatalogLocked();

        // 每次完整列举签发新令牌，确保旧 Catalog 不会因 stale 清除而重新生效。
        active_catalog_token_ = std::make_shared<int>(0);
        active_catalog_names_.clear();
        for(const auto& tool : tools) {
            active_catalog_names_.insert(tool.name);
        }
        catalog_stale_ = false;
        return MCPToolCatalogAccess::create(config_.server_id, catalog_revision_, std::move(tools),
                                            active_catalog_token_);
    }

    void validateCatalogCallLocked(const MCPToolCatalog& catalog, const std::string& remote_name) const {
        ensureStateLocked(MCPClientState::Ready, "MCP tools/call 只能在 Ready 状态调用");
        ensureListenerSafeForCatalogLocked();
        if(!catalog.valid() || MCPToolCatalogAccess::serverId(catalog) != config_.server_id ||
           MCPToolCatalogAccess::issuerToken(catalog) != active_catalog_token_ || catalog_stale_ ||
           MCPToolCatalogAccess::revision(catalog) != catalog_revision_) {
            throw MCPException(MCPErrorCode::ToolCatalogStale, state_, "MCP 工具目录不是当前 Client 的有效快照");
        }
        if(active_catalog_names_.find(remote_name) == active_catalog_names_.end()) {
            throw std::invalid_argument("目标 MCP 工具不在签发目录中");
        }
    }

    // Transport 回调只做有界入队，避免网络线程直接执行协议分类或用户目录回调。
    // 请求、响应和不可合并通知共用 pending_message_count_，从而共享同一个资源上限。
    void onMessage(nlohmann::json message) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if(inbound_stop_ || state_ == MCPClientState::Closing || state_ == MCPClientState::Closed ||
               state_ == MCPClientState::Faulted) {
                return;
            }
            if(pending_message_count_ >= config_.limits.max_pending_messages) {
                markFaultLocked(MCPErrorCode::MessageQueueOverflow);
                return;
            }
            inbound_queue_.push_back(std::move(message));
            ++pending_message_count_;
            cv_.notify_all();
        } catch(...) {
            // 分配失败也不能静默丢弃已经收到的协议消息，统一收敛为传输边界故障。
            markFaultFromCallback(MCPErrorCode::TransportFailure);
        }
    }

    // processInboundMessage 只由固定协议 Worker 调用；此时不持有 Client 状态锁。
    // 目录回调仍会在 invalidateCatalogFromNotification 内部锁外运行。
    void processInboundMessage(nlohmann::json message) noexcept {
        try {
            const auto incoming = detail::classifyIncomingMessage(message, currentState());
            if(incoming.kind == detail::MCPIncomingMessageKind::Response) {
                acceptResponse(incoming);
                return;
            }
            if(incoming.kind == detail::MCPIncomingMessageKind::Notification) {
                if(incoming.method == "notifications/tools/list_changed") {
                    invalidateCatalogFromNotification();
                }
                // 合法未知通知不影响请求关联，也不占有界消息队列。
                return;
            }

            ControlTask task;
            task.message = incoming.method == "ping" ? detail::makeServerSuccessResponse(incoming.id)
                                                     : detail::makeServerMethodNotFoundResponse(incoming.id);
            task.kind = MCPTransportRequestKind::ServerResponse;
            task.fatal_on_failure = true;
            enqueueControlTask(std::move(task));
        } catch(const MCPException& exception) {
            markFaultFromCallback(exception.code());
        } catch(...) {
            markFaultFromCallback(MCPErrorCode::ProtocolViolation);
        }
    }

    void acceptResponse(const detail::MCPIncomingMessage& incoming) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::uint64_t response_id = 0U;
        if(incoming.id.is_number_unsigned()) {
            // unsigned 必须先于 signed 分支，才能安全处理大于 INT64_MAX 的 JSON ID。
            response_id = incoming.id.get<std::uint64_t>();
        } else if(incoming.id.is_number_integer()) {
            const auto signed_id = incoming.id.get<std::int64_t>();
            if(signed_id < 0) {
                markFaultLocked(MCPErrorCode::ProtocolViolation);
                return;
            }
            response_id = static_cast<std::uint64_t>(signed_id);
        } else {
            markFaultLocked(MCPErrorCode::ProtocolViolation);
            return;
        }

        if(retired_request_ids_.find(response_id) != retired_request_ids_.end()) {
            // 已超时或取消请求的迟到响应合法但不再影响后续公开操作。
            return;
        }
        if(!active_request_id_.has_value() || *active_request_id_ != response_id) {
            markFaultLocked(MCPErrorCode::ProtocolViolation);
            return;
        }
        if(active_completion_.has_value()) {
            if(active_completion_->response.has_value()) {
                // 首个终局响应保持胜出，但第二个同 ID 终局响应证明 Server 违反一请求一响应合同。
                markFaultLocked(MCPErrorCode::ProtocolViolation);
            }
            // 当前 ID 已由错误、超时或 close 完成时，同 ID 迟到响应不能改写首次结果。
            return;
        }
        active_completion_ = ActiveRequestCompletion{incoming.raw, std::nullopt, state_};
        cv_.notify_all();
    }

    void onTransportEvent(MCPTransportEvent event) noexcept {
        CatalogChangedCallback callback;
        std::uint64_t callback_revision = 0U;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(state_ == MCPClientState::Closing || state_ == MCPClientState::Closed ||
               state_ == MCPClientState::Faulted) {
                return;
            }
            const MCPListenerState previous_listener_state = listener_state_;
            switch(event.type) {
                case MCPTransportEventType::SendCompleted:
                    completeControlLocked(event.dispatch_id, std::nullopt);
                    break;
                case MCPTransportEventType::SendFailed:
                    if(active_dispatch_id_ == event.dispatch_id) {
                        completeActiveRequestWithErrorLocked(event.error_code);
                    }
                    completeControlLocked(event.dispatch_id, event.error_code);
                    break;
                case MCPTransportEventType::ResponseStreamEstablished:
                    // 只允许当前已提交的前台请求延长等待；迟到或控制流事件不得影响后续操作。
                    if(active_dispatch_id_ == event.dispatch_id && !active_completion_.has_value()) {
                        active_response_deadline_ = active_operation_deadline_;
                    }
                    break;
                case MCPTransportEventType::ListenerStarted:
                    listener_state_ = MCPListenerState::Listening;
                    last_listener_failure_code_.reset();
                    break;
                case MCPTransportEventType::ListenerUnsupported:
                    if(listener_state_ == MCPListenerState::Listening && tools_list_changed_capability_) {
                        invalidateCatalogLocked(callback, callback_revision);
                    }
                    listener_state_ = MCPListenerState::Unsupported;
                    last_listener_failure_code_.reset();
                    break;
                case MCPTransportEventType::ListenerRecovering:
                    if(listener_state_ == MCPListenerState::Listening && tools_list_changed_capability_) {
                        invalidateCatalogLocked(callback, callback_revision);
                    }
                    listener_state_ = MCPListenerState::Recovering;
                    last_listener_failure_code_ = event.error_code;
                    break;
                case MCPTransportEventType::ListenerUnavailable:
                    if((previous_listener_state == MCPListenerState::Listening ||
                        previous_listener_state == MCPListenerState::Recovering) &&
                       tools_list_changed_capability_) {
                        invalidateCatalogLocked(callback, callback_revision);
                    }
                    listener_state_ = MCPListenerState::Unavailable;
                    last_listener_failure_code_ = event.error_code;
                    if(state_ == MCPClientState::Initializing || event.error_code == MCPErrorCode::ProtocolViolation ||
                       event.error_code == MCPErrorCode::MessageLimitExceeded) {
                        markFaultLocked(event.error_code);
                    }
                    break;
                case MCPTransportEventType::ListenerStopped:
                    if((previous_listener_state == MCPListenerState::Listening ||
                        previous_listener_state == MCPListenerState::Recovering) &&
                       tools_list_changed_capability_) {
                        invalidateCatalogLocked(callback, callback_revision);
                    }
                    listener_state_ = MCPListenerState::Stopped;
                    last_listener_failure_code_ = event.error_code;
                    markFaultLocked(event.error_code);
                    break;
                case MCPTransportEventType::SessionExpired:
                    markFaultLocked(MCPErrorCode::SessionExpired);
                    break;
                case MCPTransportEventType::ServerExited:
                    markFaultLocked(MCPErrorCode::ServerExited);
                    break;
                case MCPTransportEventType::TransportFault:
                    markFaultLocked(event.error_code);
                    break;
            }
            cv_.notify_all();
        }
        invokeCatalogCallback(std::move(callback), callback_revision);
    }

    void invalidateCatalogFromNotification() {
        CatalogChangedCallback callback;
        std::uint64_t revision = 0U;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(state_ == MCPClientState::Closing || state_ == MCPClientState::Closed ||
               state_ == MCPClientState::Faulted) {
                return;
            }
            invalidateCatalogLocked(callback, revision);
        }
        invokeCatalogCallback(std::move(callback), revision);
    }

    void invalidateCatalogLocked(CatalogChangedCallback& callback, std::uint64_t& revision) {
        if(catalog_revision_ == std::numeric_limits<std::uint64_t>::max()) {
            markFaultLocked(MCPErrorCode::ProtocolViolation);
            return;
        }
        ++catalog_revision_;
        catalog_stale_ = true;
        active_catalog_token_.reset();
        active_catalog_names_.clear();
        callback = catalog_changed_callback_;
        revision = catalog_revision_;
        cv_.notify_all();
    }

    void invokeCatalogCallback(CatalogChangedCallback callback, std::uint64_t revision) noexcept {
        if(!callback || revision == 0U) {
            return;
        }
        try {
            // 用户回调始终运行在状态锁外；异常只被隔离，不改变协议状态。
            callback(config_.server_id, revision);
        } catch(...) {
        }
    }

    void enqueueCancellation(std::uint64_t request_id) noexcept {
        try {
            ControlTask task;
            task.message = detail::makeCancelledNotification(request_id);
            task.kind = MCPTransportRequestKind::CancellationNotification;
            task.fatal_on_failure = false;
            enqueueControlTask(std::move(task));
        } catch(...) {
            // 取消通知是尽力操作，构造或入队失败不能覆盖原公开操作已经选定的根因。
        }
    }

    void enqueueControlTask(ControlTask task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(control_stop_ || state_ == MCPClientState::Closing || state_ == MCPClientState::Closed ||
           state_ == MCPClientState::Faulted) {
            return;
        }
        if(pending_message_count_ >= config_.limits.max_pending_messages) {
            if(task.fatal_on_failure) {
                markFaultLocked(MCPErrorCode::MessageQueueOverflow);
            }
            return;
        }
        ++pending_message_count_;
        control_queue_.push_back(std::move(task));
        cv_.notify_all();
    }

    // startInboundWorkerLocked 在 Transport 安装回调之前创建唯一协议分发线程。
    // 所有入站 JSON 都先经该线程分类，网络 Worker 因此不会被用户回调或控制 POST 阻塞。
    void startInboundWorkerLocked() {
        if(inbound_worker_.joinable()) {
            return;
        }
        inbound_stop_ = false;
        inbound_worker_exited_ = false;
        inbound_worker_ = std::thread([this] { inboundLoop(); });
    }

    // discardInboundMessagesLocked 只在关闭或终命故障已取得线性化点后调用。
    // 已取消的消息不会再产生控制 POST，计数同步归还给与控制队列共用的预算。
    void discardInboundMessagesLocked() noexcept {
        const std::size_t count = inbound_queue_.size();
        inbound_queue_.clear();
        pending_message_count_ = pending_message_count_ >= count ? pending_message_count_ - count : 0U;
    }

    // stopInboundWorker 使用首个 close 生成的共同截止，随后 join 保证析构不会遗留访问 this 的线程。
    void stopInboundWorker(std::chrono::steady_clock::time_point close_deadline) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            inbound_stop_ = true;
            discardInboundMessagesLocked();
            cv_.notify_all();
        }
        if(inbound_worker_.joinable() && inbound_worker_.get_id() != std::this_thread::get_id()) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_until(lock, close_deadline, [this] { return inbound_worker_exited_; });
            lock.unlock();
            // 关闭已取消 Transport 回调，Worker 只会在本地队列空后退出；join 是对象所有权收口。
            inbound_worker_.join();
        }
    }

    // inboundLoop 将已出队消息在锁外分类，避免 JSON 解析、目录回调和控制请求占用状态锁。
    void inboundLoop() noexcept {
        while(true) {
            nlohmann::json message;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return inbound_stop_ || !inbound_queue_.empty(); });
                if(inbound_stop_) {
                    discardInboundMessagesLocked();
                    inbound_worker_exited_ = true;
                    cv_.notify_all();
                    return;
                }
                message = std::move(inbound_queue_.front());
                inbound_queue_.pop_front();
                --pending_message_count_;
            }
            processInboundMessage(std::move(message));
        }
    }

    void startControlWorkerLocked() {
        if(control_worker_.joinable()) {
            return;
        }
        control_stop_ = false;
        control_worker_exited_ = false;
        control_worker_ = std::thread([this] { controlLoop(); });
    }

    void stopControlWorker(std::chrono::steady_clock::time_point close_deadline) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            control_stop_ = true;
            cv_.notify_all();
        }
        if(control_worker_.joinable() && control_worker_.get_id() != std::this_thread::get_id()) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_until(lock, close_deadline, [this] { return control_worker_exited_; });
            lock.unlock();
            // Transport::close 已先行取消 I/O；最终 join 是保证 Impl 生命期安全的必要收口。
            control_worker_.join();
        }
    }

    void controlLoop() noexcept {
        while(true) {
            ControlTask task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return control_stop_ || !control_queue_.empty(); });
                if(control_stop_) {
                    // 关闭不再排空未提交控制队列，避免取消路径额外执行网络操作。
                    const std::size_t queued_count = control_queue_.size();
                    control_queue_.clear();
                    pending_message_count_ =
                        pending_message_count_ >= queued_count ? pending_message_count_ - queued_count : 0U;
                    control_worker_exited_ = true;
                    cv_.notify_all();
                    return;
                }
                task = std::move(control_queue_.front());
                control_queue_.pop_front();
                --pending_message_count_;
            }
            sendAsyncControl(std::move(task));
        }
    }

    void sendAsyncControl(ControlTask task) noexcept {
        try {
            const auto deadline = detail::saturatingSteadyDeadlineAfter(config_.limits.request_timeout);
            std::uint64_t dispatch_id = 0U;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if(control_stop_ || state_ == MCPClientState::Closing || state_ == MCPClientState::Closed ||
                   state_ == MCPClientState::Faulted) {
                    return;
                }
                dispatch_id = allocateDispatchIdLocked();
            }
            auto prepared = transport_->prepareMessage(task.message, {task.kind, dispatch_id, deadline, deadline});
            if(!prepared) {
                throw MCPException(MCPErrorCode::TransportFailure, currentState(), "MCP 控制消息准备失败");
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if(control_stop_ || state_ == MCPClientState::Closing || state_ == MCPClientState::Closed ||
                   state_ == MCPClientState::Faulted) {
                    return;
                }
                control_pending_.emplace(dispatch_id,
                                         ControlPending{false, std::nullopt, false, task.fatal_on_failure});
                try {
                    transport_->commitPrepared(std::move(prepared), deadline);
                } catch(...) {
                    control_pending_.erase(dispatch_id);
                    throw;
                }
            }
        } catch(const MCPException& exception) {
            if(task.fatal_on_failure) {
                markFaultFromCallback(exception.code());
            }
        } catch(...) {
            if(task.fatal_on_failure) {
                markFaultFromCallback(MCPErrorCode::TransportFailure);
            }
        }
    }

    void completeControlLocked(std::uint64_t dispatch_id, std::optional<MCPErrorCode> error) {
        const auto pending = control_pending_.find(dispatch_id);
        if(pending == control_pending_.end()) {
            return;
        }
        if(pending->second.completed) {
            return;
        }
        pending->second.completed = true;
        pending->second.error = error;
        const bool fatal = error.has_value() && pending->second.fatal_on_failure;
        const bool has_waiter = pending->second.has_waiter;
        if(!has_waiter) {
            control_pending_.erase(pending);
        }
        if(fatal) {
            markFaultLocked(*error);
        }
    }

    void markFaultFromCallback(MCPErrorCode code) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        markFaultLocked(code);
    }

    void markFaultLocked(MCPErrorCode code) noexcept {
        if(state_ == MCPClientState::Faulted || state_ == MCPClientState::Closing || state_ == MCPClientState::Closed) {
            return;
        }
        state_ = MCPClientState::Faulted;
        listener_state_ =
            listener_state_ == MCPListenerState::NotApplicable ? listener_state_ : MCPListenerState::Stopped;
        last_failure_code_ = code;
        completeActiveRequestWithErrorLocked(code);
        completePendingControlsWithErrorLocked(code);
        // 终命故障后不再解释已排队的 Server 消息，也不允许尚未提交的控制任务扩大副作用。
        inbound_stop_ = true;
        discardInboundMessagesLocked();
        control_stop_ = true;
        cv_.notify_all();
    }

    void completeActiveRequestWithErrorLocked(MCPErrorCode code) noexcept {
        if(active_request_id_.has_value() && !active_completion_.has_value()) {
            active_completion_ = ActiveRequestCompletion{std::nullopt, code, state_};
        }
    }

    void completePendingControlsWithErrorLocked(MCPErrorCode code) noexcept {
        // 控制发送也遵守首次完成；迟到 SendCompleted 不能覆盖 close/故障。
        for(auto& entry : control_pending_) {
            if(!entry.second.completed) {
                entry.second.completed = true;
                entry.second.error = code;
            }
        }
    }

    void finishTransportClose() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        transport_close_finished_ = true;
        if(state_ == MCPClientState::Closing) {
            state_ = MCPClientState::Closed;
        }
        cv_.notify_all();
    }

    void retireRequestLocked(std::uint64_t request_id) {
        // 有界退休集合只吸收失败、超时或取消后的迟到响应，不形成无限历史缓存。
        retired_request_ids_.insert(request_id);
        retired_request_order_.push_back(request_id);
        while(retired_request_order_.size() > config_.limits.max_pending_messages) {
            retired_request_ids_.erase(retired_request_order_.front());
            retired_request_order_.pop_front();
        }
    }

    static const char* requestFailureMessage(MCPErrorCode code) noexcept {
        switch(code) {
            case MCPErrorCode::RequestTimeout:
                return "MCP 请求超时";
            case MCPErrorCode::ServerExited:
                return "MCP Server 已退出";
            case MCPErrorCode::AuthenticationRequired:
                return "MCP Server 要求有效鉴权";
            case MCPErrorCode::CredentialUnavailable:
                return "MCP 请求凭据不可用";
            case MCPErrorCode::OperationCancelled:
                return "MCP 操作已取消";
            case MCPErrorCode::SessionExpired:
                return "MCP HTTP 会话已经失效";
            case MCPErrorCode::MessageQueueOverflow:
                return "MCP 消息队列超过配置上限";
            case MCPErrorCode::MessageLimitExceeded:
                return "MCP 消息超过配置上限";
            case MCPErrorCode::HttpStatusError:
                return "MCP Server 返回非成功 HTTP 状态";
            default:
                return "MCP 传输操作失败";
        }
    }

    MCPServerConfig config_;
    std::shared_ptr<IMCPTransport> transport_;

    // 所有可变协议状态由同一互斥量保护，锁内不调用凭据 Provider 或用户回调。
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    MCPClientState state_ = MCPClientState::Disconnected;
    MCPListenerState listener_state_ = MCPListenerState::NotApplicable;
    std::optional<MCPErrorCode> last_failure_code_;
    std::optional<MCPErrorCode> last_listener_failure_code_;
    bool foreground_busy_ = false;
    bool tools_list_changed_capability_ = false;

    // Client 请求保持单调 ID；同一时刻最多一个公开请求等待响应。
    std::uint64_t next_request_id_ = 1U;
    std::uint64_t next_dispatch_id_ = 1U;
    std::optional<std::uint64_t> active_request_id_;
    std::optional<std::uint64_t> active_dispatch_id_;
    std::optional<ActiveRequestCompletion> active_completion_;
    // 前者初始为请求段，SSE 建流事件后切换为后者；两者均由同一状态锁保护。
    std::chrono::steady_clock::time_point active_response_deadline_{};
    std::chrono::steady_clock::time_point active_operation_deadline_{};
    std::unordered_set<std::uint64_t> retired_request_ids_;
    std::deque<std::uint64_t> retired_request_order_;

    // Catalog 代次和每次签发令牌共同防止旧快照在重新列举后复活。
    std::uint64_t catalog_revision_ = 0U;
    bool catalog_stale_ = true;
    std::shared_ptr<const void> active_catalog_token_;
    std::unordered_set<std::string> active_catalog_names_;
    CatalogChangedCallback catalog_changed_callback_;

    // 入站协议 Worker 与控制 Worker 分离：前者只分类，后者才可能执行短生命周期控制 POST。
    std::thread inbound_worker_;
    bool inbound_stop_ = false;
    bool inbound_worker_exited_ = true;
    std::deque<nlohmann::json> inbound_queue_;

    // 单一控制 Worker 处理 Server 请求响应和取消通知，不按消息创建线程。
    std::thread control_worker_;
    bool control_stop_ = false;
    bool control_worker_exited_ = true;
    std::size_t pending_message_count_ = 0U;
    std::deque<ControlTask> control_queue_;
    std::unordered_map<std::uint64_t, ControlPending> control_pending_;

    // 传输关闭只有一个所有者，其他 close/连接失败路径共享同一截止时间。
    bool transport_close_started_ = false;
    bool transport_close_finished_ = false;
    std::chrono::steady_clock::time_point transport_close_deadline_{};
};

MCPClient::MCPClient(MCPServerConfig config, std::shared_ptr<IMCPTransport> transport)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(transport))) {}

MCPClient::~MCPClient() noexcept = default;

void MCPClient::connect() {
    try {
        impl_->connect();
    } catch(const MCPException& exception) {
        impl_->throwBoundedPublicError(exception);
    }
}

void MCPClient::ping() {
    try {
        impl_->ping();
    } catch(const MCPException& exception) {
        impl_->throwBoundedPublicError(exception);
    }
}

MCPToolCatalog MCPClient::listTools() {
    try {
        return impl_->listTools();
    } catch(const MCPException& exception) {
        impl_->throwBoundedPublicError(exception);
    }
}

MCPToolCallResult MCPClient::callTool(const MCPToolCatalog& catalog, const std::string& remote_name,
                                      const nlohmann::json& arguments) {
    try {
        return impl_->callTool(catalog, remote_name, arguments);
    } catch(const MCPException& exception) {
        impl_->throwBoundedPublicError(exception);
    }
}

void MCPClient::close() noexcept {
    impl_->close();
}

MCPClientState MCPClient::state() const noexcept {
    return impl_->state();
}

MCPListenerState MCPClient::listenerState() const noexcept {
    return impl_->listenerState();
}

std::optional<MCPErrorCode> MCPClient::lastFailureCode() const noexcept {
    return impl_->lastFailureCode();
}

std::optional<MCPErrorCode> MCPClient::lastListenerFailureCode() const noexcept {
    return impl_->lastListenerFailureCode();
}

std::uint64_t MCPClient::catalogRevision() const noexcept {
    return impl_->catalogRevision();
}

bool MCPClient::isToolCatalogStale() const noexcept {
    return impl_->isToolCatalogStale();
}

bool MCPClient::ownsCatalog(const MCPToolCatalog& catalog) const noexcept {
    return impl_->ownsCatalog(catalog);
}

const std::string& MCPClient::serverId() const noexcept {
    return impl_->serverId();
}

void MCPClient::setCatalogChangedCallback(CatalogChangedCallback callback) {
    impl_->setCatalogChangedCallback(std::move(callback));
}

}  // namespace aiSDK
