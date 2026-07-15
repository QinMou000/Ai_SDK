#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mcp/MCPClient.h"
#include "mcp/MCPToolAdapter.h"

namespace {

using namespace std::chrono_literals;

// MCPClient 确定性并发测试合同：以下注释描述测试桩、调度屏障和可观察结果。
// 这些用例不依赖 sleep、真实网络或子进程，所有竞争顺序都由条件变量建立。
// 每个屏障都在等待时释放 ScriptedTransport 自身锁，保证 Client 回调可以并发进入。
// commitPrepared 只入队，绝不同步调用 Client，保持与生产 Transport 相同的关键合同。
// Worker 的自动响应可被暂停，测试线程才能在同一 request ID 上安排先后事件。
// close 屏障位于 Client 已写取消、Transport 尚未清空回调之间，用于投递确定的迟到响应。
// 所有异常跨线程传递都使用 exception_ptr，并在 join 后读取。
// 所有线程在断言前完成清理，测试失败也不能遗留 joinable 线程。
// 下列矩阵既是测试说明，也是修改 ScriptedTransport 时必须保持的夹具合同。
// 测试夹具 PreparedMessage 的职责：完整保存 JSON 和 request context。
// 测试夹具 PreparedMessage 的同步方式：prepare 与 commit 可被测试分别观察。
// 测试夹具 PreparedMessage 的边界：只由 ScriptedTransport 解释。
// 测试夹具 PreparedMessage 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 PreparedMessage 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 open 回调安装 的职责：在一个锁内保存两个 Client 回调。
// 测试夹具 open 回调安装 的同步方式：Worker 启动后才处理提交队列。
// 测试夹具 open 回调安装 的边界：禁止重复 open。
// 测试夹具 open 回调安装 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 open 回调安装 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 prepare 屏障 的职责：只对显式 ping 启用。
// 测试夹具 prepare 屏障 的同步方式：等待会释放假传输锁。
// 测试夹具 prepare 屏障 的边界：允许故障回调在 Provider 期间进入 Client。
// 测试夹具 prepare 屏障 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 prepare 屏障 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 commit 队列 的职责：移动 prepared 内容并递增写入计数。
// 测试夹具 commit 队列 的同步方式：commit 本身不调用 Client。
// 测试夹具 commit 队列 的边界：stopping 后立即返回取消。
// 测试夹具 commit 队列 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 commit 队列 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 单 Worker 的职责：串行处理所有脚本消息。
// 测试夹具 单 Worker 的同步方式：测试结果不依赖真实网络或进程。
// 测试夹具 单 Worker 的边界：close 通过 stopping 唤醒并 join。
// 测试夹具 单 Worker 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 单 Worker 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 初始化脚本 的职责：返回固定 2025-11-25 版本。
// 测试夹具 初始化脚本 的同步方式：声明 tools.listChanged=true。
// 测试夹具 初始化脚本 的边界：提供稳定中文 serverInfo。
// 测试夹具 初始化脚本 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 初始化脚本 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 Listener 脚本 的职责：connect 默认报告 405 Unsupported。
// 测试夹具 Listener 脚本 的同步方式：额外事件可切换到 Listening。
// 测试夹具 Listener 脚本 的边界：监听不占公开操作槽。
// 测试夹具 Listener 脚本 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 Listener 脚本 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 ping 响应屏障 的职责：记录 request ID 与 dispatch ID。
// 测试夹具 ping 响应屏障 的同步方式：可阻止自动响应。
// 测试夹具 ping 响应屏障 的边界：支持手动注入发送失败和迟到响应。
// 测试夹具 ping 响应屏障 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 ping 响应屏障 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 工具调用屏障 的职责：记录工具 request ID。
// 测试夹具 工具调用屏障 的同步方式：可阻止自动结果。
// 测试夹具 工具调用屏障 的边界：支持 close 后注入迟到工具响应。
// 测试夹具 工具调用屏障 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 工具调用屏障 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 close 屏障 的职责：先记录 Transport close 已进入。
// 测试夹具 close 屏障 的同步方式：再暂停回调清理与 Worker join。
// 测试夹具 close 屏障 的边界：用于证明 Client 取消先于资源回收。
// 测试夹具 close 屏障 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 close 屏障 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 close 计数 的职责：统计每次 Transport::close 调用。
// 测试夹具 close 计数 的同步方式：并发 Client close 应只增加一次。
// 测试夹具 close 计数 的边界：断言发生在假传输析构前。
// 测试夹具 close 计数 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 close 计数 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 故障注入 的职责：直接投递闭合 TransportFault。
// 测试夹具 故障注入 的同步方式：不经过队列与 Worker 调度。
// 测试夹具 故障注入 的边界：用于精确控制首因顺序。
// 测试夹具 故障注入 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 故障注入 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 Listener 注入 的职责：直接投递 started/recovering/unavailable/stopped。
// 测试夹具 Listener 注入 的同步方式：同步返回后目录回调已执行。
// 测试夹具 Listener 注入 的边界：用于验证代次和主状态。
// 测试夹具 Listener 注入 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 Listener 注入 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 Server ping 注入 的职责：直接投递 Server request。
// 测试夹具 Server ping 注入 的同步方式：控制 Worker 生成 JSON-RPC 响应。
// 测试夹具 Server ping 注入 的边界：可记录返回 ID 原始 JSON 类型。
// 测试夹具 Server ping 注入 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 Server ping 注入 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 分页脚本 的职责：第一页可返回 nextCursor。
// 测试夹具 分页脚本 的同步方式：第二页返回不同工具。
// 测试夹具 分页脚本 的边界：验证顺序、游标和完整结果。
// 测试夹具 分页脚本 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 分页脚本 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 工具失败脚本 的职责：下一个 tools/call 可发送 SendFailed。
// 测试夹具 工具失败脚本 的同步方式：只消费一次失败标志。
// 测试夹具 工具失败脚本 的边界：验证 Submitted 后副作用未知。
// 测试夹具 工具失败脚本 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 工具失败脚本 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 凭据隔离 的职责：假传输不访问环境变量或远端认证。
// 测试夹具 凭据隔离 的同步方式：Client 单元测试只关注协议状态机。
// 测试夹具 凭据隔离 的边界：真实凭据行为留给 HTTP 集成测试。
// 测试夹具 凭据隔离 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 凭据隔离 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 计时配置 的职责：request_timeout 固定一秒。
// 测试夹具 计时配置 的同步方式：absolute timeout 固定三秒。
// 测试夹具 计时配置 的边界：屏障测试均在一秒内显式释放。
// 测试夹具 计时配置 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 计时配置 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 异常捕获 的职责：工作线程只写 exception_ptr。
// 测试夹具 异常捕获 的同步方式：主线程 join 后再读取。
// 测试夹具 异常捕获 的边界：避免未同步读取异常对象。
// 测试夹具 异常捕获 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 异常捕获 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 测试夹具 资源收口 的职责：每个测试显式 client.close。
// 测试夹具 资源收口 的同步方式：ScriptedTransport 析构再次执行幂等 close。
// 测试夹具 资源收口 的边界：防止后台 Worker 泄漏到后续用例。
// 测试夹具 资源收口 的锁规则：复制回调或状态后在假传输锁外进入 Client。
// 测试夹具 资源收口 的失败规则：只使用闭合 MCP 错误码，不依赖动态秘密文本。
// 用例“连接后支持分页列举和完整工具调用结果”的类别：正常流程。
// 用例“连接后支持分页列举和完整工具调用结果”验证的合同：Ready 生命周期、两页工具目录和完整 call result。
// 用例“连接后支持分页列举和完整工具调用结果”的前置场景：开启多页脚本。
// 用例“连接后支持分页列举和完整工具调用结果”的触发动作：连接后依次 listTools 与 callTool。
// 用例“连接后支持分页列举和完整工具调用结果”的状态断言：目录含 echo、sum 且扩展字段保留。
// 用例“连接后支持分页列举和完整工具调用结果”的结果断言：工具 structuredContent 与 raw_result 完整。
// 用例“连接后支持分页列举和完整工具调用结果”的资源收口：显式 close。
// 用例“连接后支持分页列举和完整工具调用结果”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“连接后支持分页列举和完整工具调用结果”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“目录变化使旧快照失效且调用保持零写入”的类别：状态边界。
// 用例“目录变化使旧快照失效且调用保持零写入”验证的合同：list_changed 的即时失效语义。
// 用例“目录变化使旧快照失效且调用保持零写入”的前置场景：先签发 Catalog 并记录 commit 数。
// 用例“目录变化使旧快照失效且调用保持零写入”的触发动作：同步注入 list_changed。
// 用例“目录变化使旧快照失效且调用保持零写入”的状态断言：revision 增一且 stale=true。
// 用例“目录变化使旧快照失效且调用保持零写入”的结果断言：旧 Catalog 调用返回 ToolCatalogStale。
// 用例“目录变化使旧快照失效且调用保持零写入”的资源收口：提交数不变。
// 用例“目录变化使旧快照失效且调用保持零写入”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“目录变化使旧快照失效且调用保持零写入”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“公开异常文本遵守UTF8字节上限且保留结构化字段”的类别：错误恢复。
// 用例“公开异常文本遵守UTF8字节上限且保留结构化字段”验证的合同：公开错误文本 UTF-8 截断边界。
// 用例“公开异常文本遵守UTF8字节上限且保留结构化字段”的前置场景：把 max_error_text_bytes 设为六。
// 用例“公开异常文本遵守UTF8字节上限且保留结构化字段”的触发动作：让旧 Catalog 触发固定中文错误。
// 用例“公开异常文本遵守UTF8字节上限且保留结构化字段”的状态断言：异常文本不超过六字节。
// 用例“公开异常文本遵守UTF8字节上限且保留结构化字段”的结果断言：错误码与副作用字段不丢失。
// 用例“公开异常文本遵守UTF8字节上限且保留结构化字段”的资源收口：显式 close。
// 用例“公开异常文本遵守UTF8字节上限且保留结构化字段”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“公开异常文本遵守UTF8字节上限且保留结构化字段”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“已提交工具在传输失败后返回结果未知”的类别：错误恢复。
// 用例“已提交工具在传输失败后返回结果未知”验证的合同：Submitted 工具的副作用不确定性。
// 用例“已提交工具在传输失败后返回结果未知”的前置场景：签发 Catalog 后设置下一调用失败。
// 用例“已提交工具在传输失败后返回结果未知”的触发动作：tools/call 已进入 commit 队列。
// 用例“已提交工具在传输失败后返回结果未知”的状态断言：顶层错误为 OutcomeUnknown。
// 用例“已提交工具在传输失败后返回结果未知”的结果断言：cause 为 TransportFailure 且 mayHaveExecuted=true。
// 用例“已提交工具在传输失败后返回结果未知”的资源收口：状态快照仍为 Ready。
// 用例“已提交工具在传输失败后返回结果未知”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“已提交工具在传输失败后返回结果未知”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“后台Listener不占槽但另一个公开操作会返回忙”的类别：并发边界。
// 用例“后台Listener不占槽但另一个公开操作会返回忙”验证的合同：前台槽与 Listener 后台任务隔离。
// 用例“后台Listener不占槽但另一个公开操作会返回忙”的前置场景：阻塞已提交 ping。
// 用例“后台Listener不占槽但另一个公开操作会返回忙”的触发动作：ping 等待时调用 listTools。
// 用例“后台Listener不占槽但另一个公开操作会返回忙”的状态断言：第二个公开操作立即 ClientBusy。
// 用例“后台Listener不占槽但另一个公开操作会返回忙”的结果断言：释放 ping 后原操作成功。
// 用例“后台Listener不占槽但另一个公开操作会返回忙”的资源收口：显式 close。
// 用例“后台Listener不占槽但另一个公开操作会返回忙”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“后台Listener不占槽但另一个公开操作会返回忙”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“ServerPing由后台控制流独立响应”的类别：正常流程。
// 用例“ServerPing由后台控制流独立响应”验证的合同：Server request 的控制 Worker 路径。
// 用例“ServerPing由后台控制流独立响应”的前置场景：Client 已 Ready。
// 用例“ServerPing由后台控制流独立响应”的触发动作：注入字符串 ID 的 Server ping。
// 用例“ServerPing由后台控制流独立响应”的状态断言：控制队列生成成功响应。
// 用例“ServerPing由后台控制流独立响应”的结果断言：主状态保持 Ready。
// 用例“ServerPing由后台控制流独立响应”的资源收口：显式 close。
// 用例“ServerPing由后台控制流独立响应”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“ServerPing由后台控制流独立响应”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“请求错误先到时迟到响应不能覆盖”的类别：竞态恢复。
// 用例“请求错误先到时迟到响应不能覆盖”验证的合同：SendFailed 与同 ID 响应的 first-wins。
// 用例“请求错误先到时迟到响应不能覆盖”的前置场景：阻塞 ping 自动响应并捕获关联 ID。
// 用例“请求错误先到时迟到响应不能覆盖”的触发动作：先注入 SendFailed 再注入响应。
// 用例“请求错误先到时迟到响应不能覆盖”的状态断言：ping 返回 TransportFailure。
// 用例“请求错误先到时迟到响应不能覆盖”的结果断言：完成状态快照为 Ready。
// 用例“请求错误先到时迟到响应不能覆盖”的资源收口：释放 Worker 并 close。
// 用例“请求错误先到时迟到响应不能覆盖”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“请求错误先到时迟到响应不能覆盖”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“故障先到时迟到响应不能改写根因”的类别：竞态恢复。
// 用例“故障先到时迟到响应不能改写根因”验证的合同：主故障与同 ID 响应的 first-wins。
// 用例“故障先到时迟到响应不能改写根因”的前置场景：阻塞 ping 自动响应。
// 用例“故障先到时迟到响应不能改写根因”的触发动作：先注入 ServerExited 再注入响应。
// 用例“故障先到时迟到响应不能改写根因”的状态断言：ping 返回 ServerExited。
// 用例“故障先到时迟到响应不能改写根因”的结果断言：Client Faulted 且 lastFailure 不变。
// 用例“故障先到时迟到响应不能改写根因”的资源收口：释放 Worker 并 close。
// 用例“故障先到时迟到响应不能改写根因”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“故障先到时迟到响应不能改写根因”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“成功响应后的重复同ID响应使Client故障”的类别：协议边界。
// 用例“成功响应后的重复同ID响应使Client故障”验证的合同：首个终局结果胜出，重复终局响应触发 ProtocolViolation。
// 用例“成功响应后的重复同ID响应使Client故障”的前置场景：阻塞 ping 自动响应并捕获关联 ID。
// 用例“成功响应后的重复同ID响应使Client故障”的触发动作：连续投递两个相同 ID 的成功响应。
// 用例“成功响应后的重复同ID响应使Client故障”的状态断言：ping 结果仍已知，Client 进入 Faulted。
// 用例“成功响应后的重复同ID响应使Client故障”的结果断言：lastFailure 固定为 ProtocolViolation。
// 用例“成功响应后的重复同ID响应使Client故障”的资源收口：释放 Worker 并显式 close。
// 用例“成功响应后的重复同ID响应使Client故障”的确定性要求：两次回调由测试线程顺序投递。
// 用例“成功响应后的重复同ID响应使Client故障”的回归价值：防止退休集合静默吞掉 Server 重复终局响应。
// 用例“close先完成已提交工具且迟到响应无效”的类别：竞态恢复。
// 用例“close先完成已提交工具且迟到响应无效”验证的合同：Submitted 工具与 close 的 first-wins。
// 用例“close先完成已提交工具且迟到响应无效”的前置场景：阻塞工具结果并暂停 Transport close。
// 用例“close先完成已提交工具且迟到响应无效”的触发动作：close 进入传输后注入迟到工具响应。
// 用例“close先完成已提交工具且迟到响应无效”的状态断言：调用返回 OutcomeUnknown。
// 用例“close先完成已提交工具且迟到响应无效”的结果断言：cause 为 OperationCancelled 且状态为 Closing。
// 用例“close先完成已提交工具且迟到响应无效”的资源收口：释放 close 并验证单次传输关闭。
// 用例“close先完成已提交工具且迟到响应无效”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“close先完成已提交工具且迟到响应无效”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“并发close只有一个Transport关闭所有者”的类别：并发边界。
// 用例“并发close只有一个Transport关闭所有者”验证的合同：公开 close 的 single-owner 协议。
// 用例“并发close只有一个Transport关闭所有者”的前置场景：暂停首个 Transport close。
// 用例“并发close只有一个Transport关闭所有者”的触发动作：第二个线程同时调用 client.close。
// 用例“并发close只有一个Transport关闭所有者”的状态断言：两个调用都收敛到 Closed。
// 用例“并发close只有一个Transport关闭所有者”的结果断言：Transport close 调用计数等于一。
// 用例“并发close只有一个Transport关闭所有者”的资源收口：释放屏障并 join 两线程。
// 用例“并发close只有一个Transport关闭所有者”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“并发close只有一个Transport关闭所有者”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“close取消未提交准备且保持零写入”的类别：竞态恢复。
// 用例“close取消未提交准备且保持零写入”验证的合同：Provider 阶段取消的零副作用语义。
// 用例“close取消未提交准备且保持零写入”的前置场景：阻塞 ping prepare 并暂停 Transport close。
// 用例“close取消未提交准备且保持零写入”的触发动作：准备期间并发 client.close。
// 用例“close取消未提交准备且保持零写入”的状态断言：ping 返回 OperationCancelled。
// 用例“close取消未提交准备且保持零写入”的结果断言：commit 计数保持不变。
// 用例“close取消未提交准备且保持零写入”的资源收口：释放 close 并 join。
// 用例“close取消未提交准备且保持零写入”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“close取消未提交准备且保持零写入”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“Provider准备期间故障抛出首个真实根因”的类别：竞态恢复。
// 用例“Provider准备期间故障抛出首个真实根因”验证的合同：Provider 阶段 Faulted 的真实错误传播。
// 用例“Provider准备期间故障抛出首个真实根因”的前置场景：阻塞 ping prepare。
// 用例“Provider准备期间故障抛出首个真实根因”的触发动作：准备期间注入 ServerExited 后释放。
// 用例“Provider准备期间故障抛出首个真实根因”的状态断言：抛出 MCPException 而非 logic_error。
// 用例“Provider准备期间故障抛出首个真实根因”的结果断言：错误码和状态快照为 ServerExited/Faulted。
// 用例“Provider准备期间故障抛出首个真实根因”的资源收口：显式 close。
// 用例“Provider准备期间故障抛出首个真实根因”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“Provider准备期间故障抛出首个真实根因”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“Faulted只保留第一个故障根因”的类别：错误恢复。
// 用例“Faulted只保留第一个故障根因”验证的合同：主故障 first-wins。
// 用例“Faulted只保留第一个故障根因”的前置场景：Client 已 Ready。
// 用例“Faulted只保留第一个故障根因”的触发动作：依次注入 SessionExpired 与 ProtocolViolation。
// 用例“Faulted只保留第一个故障根因”的状态断言：状态为 Faulted。
// 用例“Faulted只保留第一个故障根因”的结果断言：lastFailure 保持 SessionExpired。
// 用例“Faulted只保留第一个故障根因”的资源收口：显式 close。
// 用例“Faulted只保留第一个故障根因”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“Faulted只保留第一个故障根因”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“Listener监听缺口递增目录代次并回调”的类别：监听恢复。
// 用例“Listener监听缺口递增目录代次并回调”验证的合同：Listening 与 Recovering 到 Unavailable 的目录完整性。
// 用例“Listener监听缺口递增目录代次并回调”的前置场景：切换 Listening 后签发 Catalog。
// 用例“Listener监听缺口递增目录代次并回调”的触发动作：依次注入 Recovering 与 Unavailable。
// 用例“Listener监听缺口递增目录代次并回调”的状态断言：目录 stale 且 revision 到二。
// 用例“Listener监听缺口递增目录代次并回调”的结果断言：回调顺序严格为一、二。
// 用例“Listener监听缺口递增目录代次并回调”的资源收口：显式 close。
// 用例“Listener监听缺口递增目录代次并回调”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“Listener监听缺口递增目录代次并回调”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“Listener意外停止先使目录失效再进入故障”的类别：监听终止。
// 用例“Listener意外停止先使目录失效再进入故障”验证的合同：ListenerStopped 的目录与主状态顺序。
// 用例“Listener意外停止先使目录失效再进入故障”的前置场景：Listening 状态签发 Catalog。
// 用例“Listener意外停止先使目录失效再进入故障”的触发动作：注入带 ServerExited 的 ListenerStopped。
// 用例“Listener意外停止先使目录失效再进入故障”的状态断言：目录 revision 增一并回调。
// 用例“Listener意外停止先使目录失效再进入故障”的结果断言：Client Faulted 且首因为 ServerExited。
// 用例“Listener意外停止先使目录失效再进入故障”的资源收口：显式 close。
// 用例“Listener意外停止先使目录失效再进入故障”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“Listener意外停止先使目录失效再进入故障”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“Server超大无符号ID保持原值响应”的类别：数值边界。
// 用例“Server超大无符号ID保持原值响应”验证的合同：大于 INT64_MAX 的 JSON unsigned ID。
// 用例“Server超大无符号ID保持原值响应”的前置场景：Client 已 Ready。
// 用例“Server超大无符号ID保持原值响应”的触发动作：注入 UINT64_MAX 的 Server ping。
// 用例“Server超大无符号ID保持原值响应”的状态断言：控制响应 ID 类型仍为 unsigned。
// 用例“Server超大无符号ID保持原值响应”的结果断言：数值与请求完全一致。
// 用例“Server超大无符号ID保持原值响应”的资源收口：显式 close。
// 用例“Server超大无符号ID保持原值响应”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“Server超大无符号ID保持原值响应”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“显式绑定注册执行和注销保持可审计”的类别：正常流程。
// 用例“显式绑定注册执行和注销保持可审计”验证的合同：Adapter 显式绑定生命周期。
// 用例“显式绑定注册执行和注销保持可审计”的前置场景：签发 Catalog 并创建 echo binding。
// 用例“显式绑定注册执行和注销保持可审计”的触发动作：注册、执行、注销。
// 用例“显式绑定注册执行和注销保持可审计”的状态断言：本地名称含 server 前缀。
// 用例“显式绑定注册执行和注销保持可审计”的结果断言：结果数据映射完整且最终不存在工具。
// 用例“显式绑定注册执行和注销保持可审计”的资源收口：关闭共享 Client。
// 用例“显式绑定注册执行和注销保持可审计”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“显式绑定注册执行和注销保持可审计”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“目录过期的旧Binding返回稳定失败且不调用远端”的类别：状态边界。
// 用例“目录过期的旧Binding返回稳定失败且不调用远端”验证的合同：Adapter 对 stale Catalog 的零写入防线。
// 用例“目录过期的旧Binding返回稳定失败且不调用远端”的前置场景：创建 binding 后记录提交数。
// 用例“目录过期的旧Binding返回稳定失败且不调用远端”的触发动作：注入 list_changed 再调用 handler。
// 用例“目录过期的旧Binding返回稳定失败且不调用远端”的状态断言：返回稳定中文失败。
// 用例“目录过期的旧Binding返回稳定失败且不调用远端”的结果断言：提交数保持不变。
// 用例“目录过期的旧Binding返回稳定失败且不调用远端”的资源收口：关闭共享 Client。
// 用例“目录过期的旧Binding返回稳定失败且不调用远端”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“目录过期的旧Binding返回稳定失败且不调用远端”的回归价值：防止未来重构改变首次完成或零写入语义。
// 用例“所有固定失败文本也遵守适配器字节上限”的类别：数值边界。
// 用例“所有固定失败文本也遵守适配器字节上限”验证的合同：Adapter 固定文本 UTF-8 边界。
// 用例“所有固定失败文本也遵守适配器字节上限”的前置场景：把 Adapter 上限设为六字节。
// 用例“所有固定失败文本也遵守适配器字节上限”的触发动作：使绑定目录 stale 后调用。
// 用例“所有固定失败文本也遵守适配器字节上限”的状态断言：失败文本不超过上限。
// 用例“所有固定失败文本也遵守适配器字节上限”的结果断言：不依赖文本解析错误类型。
// 用例“所有固定失败文本也遵守适配器字节上限”的资源收口：关闭共享 Client。
// 用例“所有固定失败文本也遵守适配器字节上限”的确定性要求：所有竞争点必须由夹具条件变量确认。
// 用例“所有固定失败文本也遵守适配器字节上限”的回归价值：防止未来重构改变首次完成或零写入语义。
// 竞态日程“发送错误对迟到响应”开始：以下步骤按条件变量和同步回调形成 happens-before。
// 竞态日程“发送错误对迟到响应”步骤 1：主线程等待 ping_committed_。
// 竞态日程“发送错误对迟到响应”步骤 2：测试线程持有公开 ping 槽。
// 竞态日程“发送错误对迟到响应”步骤 3：Worker 停在自动响应屏障。
// 竞态日程“发送错误对迟到响应”步骤 4：主线程投递匹配 dispatch 的 SendFailed。
// 竞态日程“发送错误对迟到响应”步骤 5：Client 锁内写入错误完成记录。
// 竞态日程“发送错误对迟到响应”步骤 6：主线程立即投递匹配 request ID 的响应。
// 竞态日程“发送错误对迟到响应”步骤 7：acceptResponse 看到完成槽非空并忽略。
// 竞态日程“发送错误对迟到响应”步骤 8：主线程释放 Worker。
// 竞态日程“发送错误对迟到响应”步骤 9：等待线程读取首次错误并退休 ID。
// 竞态日程“发送错误对迟到响应”结束：若不满足此顺序，用例必须先修复夹具而不是放宽断言。
// 竞态日程“故障对迟到响应”开始：以下步骤按条件变量和同步回调形成 happens-before。
// 竞态日程“故障对迟到响应”步骤 1：主线程等待 ping_committed_。
// 竞态日程“故障对迟到响应”步骤 2：Worker 停在自动响应屏障。
// 竞态日程“故障对迟到响应”步骤 3：主线程投递 ServerExited。
// 竞态日程“故障对迟到响应”步骤 4：markFaultLocked 固定首因。
// 竞态日程“故障对迟到响应”步骤 5：活动请求以 ServerExited 完成。
// 竞态日程“故障对迟到响应”步骤 6：主线程投递同 ID 响应。
// 竞态日程“故障对迟到响应”步骤 7：响应不能覆盖故障完成。
// 竞态日程“故障对迟到响应”步骤 8：释放 Worker 后额外自动响应也被退休集合吸收。
// 竞态日程“故障对迟到响应”步骤 9：等待线程观察 Faulted 状态快照。
// 竞态日程“故障对迟到响应”结束：若不满足此顺序，用例必须先修复夹具而不是放宽断言。
// 竞态日程“close 对迟到工具结果”开始：以下步骤按条件变量和同步回调形成 happens-before。
// 竞态日程“close 对迟到工具结果”步骤 1：工具线程完成 commit。
// 竞态日程“close 对迟到工具结果”步骤 2：Worker 停在工具结果屏障。
// 竞态日程“close 对迟到工具结果”步骤 3：close 线程先写 Closing 与取消完成。
// 竞态日程“close 对迟到工具结果”步骤 4：Transport close 在假传输屏障暂停。
// 竞态日程“close 对迟到工具结果”步骤 5：主线程仍可通过已复制回调投递结果。
// 竞态日程“close 对迟到工具结果”步骤 6：结果回调看到取消完成并忽略。
// 竞态日程“close 对迟到工具结果”步骤 7：释放 Transport close。
// 竞态日程“close 对迟到工具结果”步骤 8：Worker 因 stopping 退出。
// 竞态日程“close 对迟到工具结果”步骤 9：工具线程得到 OutcomeUnknown/OperationCancelled。
// 竞态日程“close 对迟到工具结果”结束：若不满足此顺序，用例必须先修复夹具而不是放宽断言。
// 竞态日程“两个并发 close”开始：以下步骤按条件变量和同步回调形成 happens-before。
// 竞态日程“两个并发 close”步骤 1：首线程进入 client.close。
// 竞态日程“两个并发 close”步骤 2：Client 锁内取得 transport_close_started_。
// 竞态日程“两个并发 close”步骤 3：假 Transport 在 close 屏障暂停。
// 竞态日程“两个并发 close”步骤 4：第二线程进入 client.close。
// 竞态日程“两个并发 close”步骤 5：第二线程读取同一 close_deadline。
// 竞态日程“两个并发 close”步骤 6：第二线程不调用 Transport close。
// 竞态日程“两个并发 close”步骤 7：主线程释放屏障。
// 竞态日程“两个并发 close”步骤 8：所有者发布 transport_close_finished_。
// 竞态日程“两个并发 close”步骤 9：两线程均观察 Closed。
// 竞态日程“两个并发 close”结束：若不满足此顺序，用例必须先修复夹具而不是放宽断言。
// 竞态日程“close 对 Provider”开始：以下步骤按条件变量和同步回调形成 happens-before。
// 竞态日程“close 对 Provider”步骤 1：ping 线程进入 prepare 屏障。
// 竞态日程“close 对 Provider”步骤 2：此时尚无活动请求与 Submitted。
// 竞态日程“close 对 Provider”步骤 3：close 线程写 Closing。
// 竞态日程“close 对 Provider”步骤 4：Transport close 设置 stopping 并释放 prepare。
// 竞态日程“close 对 Provider”步骤 5：ping 线程返回 Client 二次校验。
// 竞态日程“close 对 Provider”步骤 6：ensureStateLocked 抛 OperationCancelled。
// 竞态日程“close 对 Provider”步骤 7：commitPrepared 从未调用。
// 竞态日程“close 对 Provider”步骤 8：主线程释放 close 屏障。
// 竞态日程“close 对 Provider”步骤 9：两线程有界收敛。
// 竞态日程“close 对 Provider”结束：若不满足此顺序，用例必须先修复夹具而不是放宽断言。
// 竞态日程“故障对 Provider”开始：以下步骤按条件变量和同步回调形成 happens-before。
// 竞态日程“故障对 Provider”步骤 1：ping 线程进入 prepare 屏障。
// 竞态日程“故障对 Provider”步骤 2：主线程注入 ServerExited。
// 竞态日程“故障对 Provider”步骤 3：Client 先进入 Faulted。
// 竞态日程“故障对 Provider”步骤 4：主线程释放 prepare。
// 竞态日程“故障对 Provider”步骤 5：ping 线程回到二次校验。
// 竞态日程“故障对 Provider”步骤 6：ensureStateLocked 读取 last_failure_code_。
// 竞态日程“故障对 Provider”步骤 7：公开边界收到 MCPException。
// 竞态日程“故障对 Provider”步骤 8：不得落入 logic_error catch。
// 竞态日程“故障对 Provider”步骤 9：后续 close 回收 Transport。
// 竞态日程“故障对 Provider”结束：若不满足此顺序，用例必须先修复夹具而不是放宽断言。
// 竞态日程“Listener 恢复缺口”开始：以下步骤按条件变量和同步回调形成 happens-before。
// 竞态日程“Listener 恢复缺口”步骤 1：Client 从 Unsupported 人工切换 Listening。
// 竞态日程“Listener 恢复缺口”步骤 2：完整 listTools 签发 revision 零目录。
// 竞态日程“Listener 恢复缺口”步骤 3：Recovering 事件保存 previous=Listening。
// 竞态日程“Listener 恢复缺口”步骤 4：目录第一次 stale 并回调 revision 一。
// 竞态日程“Listener 恢复缺口”步骤 5：Unavailable 保存 previous=Recovering。
// 竞态日程“Listener 恢复缺口”步骤 6：目录第二次递增并回调 revision 二。
// 竞态日程“Listener 恢复缺口”步骤 7：TransportFailure 不使 Ready 主状态故障。
// 竞态日程“Listener 恢复缺口”步骤 8：listTools 在 Unavailable 时将被拒绝。
// 竞态日程“Listener 恢复缺口”步骤 9：close 正常收口。
// 竞态日程“Listener 恢复缺口”结束：若不满足此顺序，用例必须先修复夹具而不是放宽断言。
// 竞态日程“Listener 意外停止”开始：以下步骤按条件变量和同步回调形成 happens-before。
// 竞态日程“Listener 意外停止”步骤 1：Client 切换 Listening 并签发目录。
// 竞态日程“Listener 意外停止”步骤 2：注入 ListenerStopped。
// 竞态日程“Listener 意外停止”步骤 3：锁内先依据 previous=Listening 失效目录。
// 竞态日程“Listener 意外停止”步骤 4：随后保存 Listener 错误。
// 竞态日程“Listener 意外停止”步骤 5：markFaultLocked 固定 ServerExited。
// 竞态日程“Listener 意外停止”步骤 6：锁外执行目录回调。
// 竞态日程“Listener 意外停止”步骤 7：调用方同时看到 stale 与 Faulted。
// 竞态日程“Listener 意外停止”步骤 8：后续 Listener 事件被终态入口忽略。
// 竞态日程“Listener 意外停止”步骤 9：close 收口。
// 竞态日程“Listener 意外停止”结束：若不满足此顺序，用例必须先修复夹具而不是放宽断言。
// 竞态日程“主故障首因”开始：以下步骤按条件变量和同步回调形成 happens-before。
// 竞态日程“主故障首因”步骤 1：Client 已处于 Ready。
// 竞态日程“主故障首因”步骤 2：第一个 SessionExpired 获取 Client 锁。
// 竞态日程“主故障首因”步骤 3：markFaultLocked 写 Faulted 与首因。
// 竞态日程“主故障首因”步骤 4：第二个 ProtocolViolation 随后进入。
// 竞态日程“主故障首因”步骤 5：Faulted 快速路径直接返回。
// 竞态日程“主故障首因”步骤 6：last_failure_code_ 不被覆盖。
// 竞态日程“主故障首因”步骤 7：Listener 状态不被后续事件改写。
// 竞态日程“主故障首因”步骤 8：公开状态读取与锁同步。
// 竞态日程“主故障首因”步骤 9：close 不改变已记录首因。
// 竞态日程“主故障首因”结束：若不满足此顺序，用例必须先修复夹具而不是放宽断言。
// 断言规范：竞态测试先等待明确屏障，再注入竞争事件，禁止使用 sleep 猜测顺序。
// 断言规范：任何 ASSERT 之前都要释放屏障并 join 已创建线程，避免失败路径触发 terminate。
// 断言规范：工作线程只捕获 exception_ptr，主线程 join 后才检查。
// 断言规范：first-wins 用例同时断言错误码和 clientStateAtFailure。
// 断言规范：OutcomeUnknown 用例必须断言 causeCode 与 mayHaveExecuted。
// 断言规范：零写入用例比较竞争前后的 commitCount。
// 断言规范：single-owner 用例直接检查 Transport::close 调用次数。
// 断言规范：Listener 用例同时检查主状态、监听状态、stale、revision 和回调。
// 断言规范：首因用例同时检查 state 与 lastFailureCode。
// 断言规范：大 ID 用例同时检查 JSON 类型和 uint64 数值。
// 断言规范：正常调用用例验证 raw 扩展字段不被裁剪。
// 断言规范：分页用例验证工具顺序和数量。
// 断言规范：Adapter 用例验证显式注销后 registry 不再拥有工具。
// 断言规范：错误文本上限用例按 UTF-8 字节数断言。
// 断言规范：测试只使用 loopback 配置占位，不发起真实 HTTP。
// 断言规范：ScriptedTransport 的 commit 不同步回调，符合生产 Transport 合同。
// 断言规范：假 Listener 默认 405 使 connect 路径稳定且快速。
// 断言规范：所有测试显式 close，避免测试框架退出阶段隐藏资源问题。
// 断言规范：一秒屏障预算远大于本地内存事件耗时，但小于全局测试超时。
// 断言规范：失败输出使用稳定中文，便于本地审计。
// 断言规范：每个竞态都至少覆盖正常唤醒和迟到事件吸收。
// 断言规范：Faulted 测试不尝试复用 Client，符合终态合同。
// 断言规范：Closed 测试不再次执行协议公开操作。
// 断言规范：Catalog 测试不伪造私有签发令牌。
// 断言规范：Server request 测试经真实控制队列发送响应。
// 断言规范：消息注入在 ScriptedTransport 锁外调用 Client 回调。
// 断言规范：close 屏障保留回调只用于精确验证迟到事件。
// 断言规范：测试释放 close 屏障后才销毁 Client。
// 断言规范：测试配置关闭预算固定，便于发现无界等待。
// 断言规范：重复运行应得到相同调度结果和断言。
// 断言规范：MSVC /W4 构建不得出现新增警告。
// 断言规范：测试文件只依赖既有 GTest 与 MCP 公共接口。
// 断言规范：异常检查不依赖 what() 的完整文本。
// 断言规范：目录回调测试记录 revision 序列而非只记录次数。
// 断言规范：TransportFailure 的非致命 SendFailed 用例确认 Client 仍保持 Ready。
// 断言规范：ListenerUnavailable 的非协议错误用例确认主状态仍保持 Ready。
// 断言规范：ListenerStopped 用例确认目录回调在故障后仍被锁外送达。
// 断言规范：Provider 故障用例专门防止真实根因被 connect/ping 的通用 catch 改写。
// 断言规范：close 工具用例专门防止迟到成功掩盖副作用未知。
// 断言规范：退休响应由 Worker 释放后的第二次自动响应隐式覆盖。
// 测试合同说明结束；新增竞态必须先定义屏障、线性化顺序、结果快照和资源收口。

// PreparedMessage 保存一次假传输提交所需的完整值。
// 测试故意把准备和提交分开，才能验证 Client 的二次状态校验不会提前产生写入。
class PreparedMessage final : public aiSDK::IMCPPreparedMessage {
   public:
    PreparedMessage(nlohmann::json value, aiSDK::MCPTransportRequestContext request_context)
        : message(std::move(value)), context(request_context) {}

    nlohmann::json message;
    aiSDK::MCPTransportRequestContext context;
};

// ScriptedTransport 用单个 Worker 模拟异步 Transport，commitPrepared 本身绝不回调 Client。
// 所有脚本响应都是本地确定数据，因此状态机测试不依赖真实进程、端口或系统计时抖动。
class ScriptedTransport final : public aiSDK::IMCPTransport {
   public:
    ~ScriptedTransport() override {
        close(std::chrono::steady_clock::now());
    }

    void open(aiSDK::MCPTransportCallbacks callbacks, std::chrono::steady_clock::time_point) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if(opened_) {
            throw std::logic_error("假 Transport 不能重复打开");
        }
        callbacks_ = std::move(callbacks);
        opened_ = true;
        worker_ = std::thread([this] { workerLoop(); });
    }

    std::unique_ptr<aiSDK::IMCPPreparedMessage> prepareMessage(
        const nlohmann::json& message, const aiSDK::MCPTransportRequestContext& context) override {
        // 准备阶段只复制数据，不修改提交计数；可选屏障用于精确模拟 Provider 期间故障。
        std::unique_lock<std::mutex> lock(mutex_);
        if(message.value("method", std::string{}) == "ping" && block_ping_prepare_) {
            // 记录准备上下文，让计时测试可以观察请求段尚未启动时的绝对上限。
            last_ping_prepare_context_ = context;
            ping_prepare_entered_ = true;
            cv_.notify_all();
            cv_.wait(lock, [this] { return release_ping_prepare_ || stopping_; });
        }
        return std::make_unique<PreparedMessage>(message, context);
    }

    void commitPrepared(std::unique_ptr<aiSDK::IMCPPreparedMessage> prepared,
                        std::chrono::steady_clock::time_point request_deadline) override {
        auto* scripted = dynamic_cast<PreparedMessage*>(prepared.get());
        if(scripted == nullptr) {
            throw std::invalid_argument("假 Transport 收到未知准备消息");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_) {
            throw aiSDK::MCPException(aiSDK::MCPErrorCode::OperationCancelled, aiSDK::MCPClientState::Closing,
                                      "假 Transport 已关闭");
        }
        // 整条消息在一个锁内移入队列，测试写入计数就是 Submitted 的可观测替身。
        scripted->context.deadline = request_deadline;
        if(scripted->message.value("method", std::string{}) == "ping") {
            // 提交上下文中的 deadline 已是请求段起点产生的短截止时间。
            last_ping_commit_context_ = scripted->context;
        }
        queue_.push_back({std::move(scripted->message), scripted->context});
        ++commit_count_;
        cv_.notify_all();
    }

    void completeInitialization(const std::string& protocol_version) override {
        std::lock_guard<std::mutex> lock(mutex_);
        negotiated_version_ = protocol_version;
    }

    void startListener(std::chrono::steady_clock::time_point) override {
        aiSDK::MCPTransportCallbacks callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks = callbacks_;
        }
        // Fake 选择 405 降级路径，验证 Listener 不占前台公开操作槽。
        callbacks.on_event({aiSDK::MCPTransportEventType::ListenerUnsupported, 0U, aiSDK::MCPErrorCode::HttpStatusError,
                            405, aiSDK::MCPListenerState::Unsupported});
    }

    void close(std::chrono::steady_clock::time_point) noexcept override {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ++close_call_count_;
            if(stopping_) {
                return;
            }
            stopping_ = true;
            release_blocked_request_ = true;
            release_ping_prepare_ = true;
            close_entered_ = true;
            cv_.notify_all();
            if(block_close_) {
                cv_.wait(lock, [this] { return release_close_; });
            }
        }
        if(worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
            worker_.join();
        }
        // close 返回后清空回调，防止测试假实现继续访问已析构 Client。
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_ = {};
    }

    void enableMultiPageList() {
        std::lock_guard<std::mutex> lock(mutex_);
        multi_page_list_ = true;
    }

    void failNextToolCall() {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_next_tool_call_ = true;
    }

    void blockPing() {
        std::lock_guard<std::mutex> lock(mutex_);
        block_ping_ = true;
        release_blocked_request_ = false;
    }

    void blockPingPrepare() {
        std::lock_guard<std::mutex> lock(mutex_);
        block_ping_prepare_ = true;
        release_ping_prepare_ = false;
    }

    bool waitForPingPrepare(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return ping_prepare_entered_; });
    }

    void releasePingPrepare() {
        std::lock_guard<std::mutex> lock(mutex_);
        release_ping_prepare_ = true;
        cv_.notify_all();
    }

    std::optional<aiSDK::MCPTransportRequestContext> lastPingPrepareContext() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_ping_prepare_context_;
    }

    std::optional<aiSDK::MCPTransportRequestContext> lastPingCommitContext() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_ping_commit_context_;
    }

    void blockToolCall() {
        std::lock_guard<std::mutex> lock(mutex_);
        block_tool_call_ = true;
        release_blocked_tool_call_ = false;
    }

    bool waitForToolCallCommit(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return tool_call_committed_; });
    }

    void blockClose() {
        std::lock_guard<std::mutex> lock(mutex_);
        block_close_ = true;
        release_close_ = false;
    }

    bool waitForCloseEntered(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return close_entered_; });
    }

    void releaseClose() {
        std::lock_guard<std::mutex> lock(mutex_);
        release_close_ = true;
        cv_.notify_all();
    }

    void releasePing() {
        std::lock_guard<std::mutex> lock(mutex_);
        release_blocked_request_ = true;
        cv_.notify_all();
    }

    bool waitForPingCommit(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return ping_committed_; });
    }

    std::size_t commitCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commit_count_;
    }

    std::size_t closeCallCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return close_call_count_;
    }

    void emitListChanged() {
        aiSDK::MCPTransportCallbacks callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks = callbacks_;
        }
        callbacks.on_message({
            {"jsonrpc", "2.0"                             },
            {"method",  "notifications/tools/list_changed"},
            {"params",  nlohmann::json::object()          }
        });
    }

    void emitTransportFault(aiSDK::MCPErrorCode error) {
        emitEvent(0U, aiSDK::MCPTransportEventType::TransportFault, error);
    }

    void emitListenerEvent(aiSDK::MCPTransportEventType type,
                           aiSDK::MCPErrorCode error = aiSDK::MCPErrorCode::TransportFailure) {
        emitEvent(0U, type, error);
    }

    void emitCapturedPingResponse() {
        std::optional<nlohmann::json> request_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            request_id = captured_ping_request_id_;
        }
        if(request_id.has_value()) {
            emitMessage({
                {"jsonrpc", "2.0"                   },
                {"id",      std::move(*request_id)  },
                {"result",  nlohmann::json::object()}
            });
        }
    }

    void emitCapturedPingFailure(aiSDK::MCPErrorCode error) {
        std::optional<std::uint64_t> dispatch_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dispatch_id = captured_ping_dispatch_id_;
        }
        if(dispatch_id.has_value()) {
            emitEvent(*dispatch_id, aiSDK::MCPTransportEventType::SendFailed, error);
        }
    }

    void emitCapturedToolResponse() {
        std::optional<nlohmann::json> request_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            request_id = captured_tool_request_id_;
        }
        if(request_id.has_value()) {
            emitMessage({
                {"jsonrpc", "2.0"                 },
                {"id",      std::move(*request_id)},
                {"result",
                 {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "迟到结果"}}})},
                  {"isError", false}}             }
            });
        }
    }

    void emitServerPing(nlohmann::json request_id) {
        aiSDK::MCPTransportCallbacks callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks = callbacks_;
        }
        callbacks.on_message({
            {"jsonrpc", "2.0"                   },
            {"id",      std::move(request_id)   },
            {"method",  "ping"                  },
            {"params",  nlohmann::json::object()}
        });
    }

    bool waitForServerResponse(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return server_response_seen_; });
    }

    std::optional<nlohmann::json> serverResponseId() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return server_response_id_;
    }

   private:
    struct QueuedMessage {
        nlohmann::json message;
        aiSDK::MCPTransportRequestContext context;
    };

    void workerLoop() noexcept {
        while(true) {
            QueuedMessage queued;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if(stopping_) {
                    return;
                }
                queued = std::move(queue_.front());
                queue_.pop_front();
            }
            processMessage(std::move(queued));
        }
    }

    void processMessage(QueuedMessage queued) noexcept {
        try {
            if(!queued.message.contains("method")) {
                // 没有 method 的客户端消息是对 Server 请求的响应。
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    server_response_seen_ = true;
                    server_response_id_ = queued.message.at("id");
                    cv_.notify_all();
                }
                emitEvent(queued.context.dispatch_id, aiSDK::MCPTransportEventType::SendCompleted);
                return;
            }

            const std::string method = queued.message.at("method").get<std::string>();
            if(method == "initialize") {
                emitInitializeResponse(queued);
            } else if(method == "notifications/initialized" || method == "notifications/cancelled") {
                emitEvent(queued.context.dispatch_id, aiSDK::MCPTransportEventType::SendCompleted);
            } else if(method == "ping") {
                emitPingResponse(queued);
            } else if(method == "tools/list") {
                emitToolsPage(queued);
            } else if(method == "tools/call") {
                emitToolCallResult(queued);
            }
        } catch(...) {
            emitEvent(queued.context.dispatch_id, aiSDK::MCPTransportEventType::SendFailed,
                      aiSDK::MCPErrorCode::ProtocolViolation);
        }
    }

    void emitInitializeResponse(const QueuedMessage& queued) {
        // listChanged=true 让后续测试能证明目录通知立即使签发快照失效。
        emitMessage({
            {"jsonrpc", "2.0"                                                    },
            {"id",      queued.message.at("id")                                  },
            {"result",
             {{"protocolVersion", "2025-11-25"},
              {"capabilities", {{"tools", {{"listChanged", true}}}}},
              {"serverInfo", {{"name", "本地假服务器"}, {"version", "1"}}}}}
        });
    }

    void emitPingResponse(const QueuedMessage& queued) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ping_committed_ = true;
            captured_ping_request_id_ = queued.message.at("id");
            captured_ping_dispatch_id_ = queued.context.dispatch_id;
            cv_.notify_all();
            if(block_ping_) {
                cv_.wait(lock, [this] { return release_blocked_request_ || stopping_; });
            }
            if(stopping_) {
                return;
            }
        }
        emitMessage({
            {"jsonrpc", "2.0"                   },
            {"id",      queued.message.at("id") },
            {"result",  nlohmann::json::object()}
        });
    }

    void emitToolsPage(const QueuedMessage& queued) {
        const auto& params = queued.message.at("params");
        const bool second_page = params.contains("cursor");
        if(second_page) {
            emitMessage({
                {"jsonrpc", "2.0"                                                },
                {"id",      queued.message.at("id")                              },
                {"result",  {{"tools", nlohmann::json::array({makeTool("sum")})}}}
            });
            return;
        }

        bool multi_page = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            multi_page = multi_page_list_;
        }
        nlohmann::json result = {
            {"tools", nlohmann::json::array({makeTool("echo")})}
        };
        if(multi_page) {
            result["nextCursor"] = "第二页";
        }
        emitMessage({
            {"jsonrpc", "2.0"                  },
            {"id",      queued.message.at("id")},
            {"result",  std::move(result)      }
        });
    }

    void emitToolCallResult(const QueuedMessage& queued) {
        bool should_fail = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            should_fail = fail_next_tool_call_;
            fail_next_tool_call_ = false;
            tool_call_committed_ = true;
            captured_tool_request_id_ = queued.message.at("id");
            cv_.notify_all();
            if(block_tool_call_) {
                cv_.wait(lock, [this] { return release_blocked_tool_call_ || stopping_; });
            }
            if(stopping_) {
                return;
            }
        }
        if(should_fail) {
            emitEvent(queued.context.dispatch_id, aiSDK::MCPTransportEventType::SendFailed,
                      aiSDK::MCPErrorCode::TransportFailure);
            return;
        }
        const auto& arguments = queued.message.at("params").at("arguments");
        emitMessage({
            {"jsonrpc", "2.0"                  },
            {"id",      queued.message.at("id")},
            {"result",
             {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "调用成功"}}})},
              {"structuredContent", {{"arguments", arguments}}},
              {"isError", false},
              {"extension", "保留"}}         }
        });
    }

    static nlohmann::json makeTool(const std::string& name) {
        // 测试工具带未知扩展，确保 Client 同时填充常用字段并保留 raw。
        return {
            {"name",             name                                     },
            {"title",            "测试工具"                           },
            {"description",      "用于验证 MCP Client 的远端工具"},
            {"inputSchema",      {{"type", "object"}}                     },
            {"x-test-extension", true                                     }
        };
    }

    void emitMessage(nlohmann::json message) {
        aiSDK::MCPTransportCallbacks callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks = callbacks_;
        }
        if(callbacks.on_message) {
            callbacks.on_message(std::move(message));
        }
    }

    void emitEvent(std::uint64_t dispatch_id, aiSDK::MCPTransportEventType type,
                   aiSDK::MCPErrorCode error = aiSDK::MCPErrorCode::TransportFailure) {
        aiSDK::MCPTransportCallbacks callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks = callbacks_;
        }
        if(callbacks.on_event) {
            callbacks.on_event({type, dispatch_id, error, 0, aiSDK::MCPListenerState::NotApplicable});
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    aiSDK::MCPTransportCallbacks callbacks_;
    std::deque<QueuedMessage> queue_;
    std::thread worker_;
    bool opened_ = false;
    bool stopping_ = false;
    bool multi_page_list_ = false;
    bool fail_next_tool_call_ = false;
    bool block_ping_ = false;
    bool release_blocked_request_ = true;
    bool ping_committed_ = false;
    bool block_ping_prepare_ = false;
    bool release_ping_prepare_ = true;
    bool ping_prepare_entered_ = false;
    bool block_tool_call_ = false;
    bool release_blocked_tool_call_ = true;
    bool tool_call_committed_ = false;
    bool block_close_ = false;
    bool release_close_ = true;
    bool close_entered_ = false;
    bool server_response_seen_ = false;
    std::size_t commit_count_ = 0U;
    std::size_t close_call_count_ = 0U;
    std::string negotiated_version_;
    std::optional<nlohmann::json> captured_ping_request_id_;
    std::optional<std::uint64_t> captured_ping_dispatch_id_;
    std::optional<nlohmann::json> captured_tool_request_id_;
    std::optional<nlohmann::json> server_response_id_;
    std::optional<aiSDK::MCPTransportRequestContext> last_ping_prepare_context_;
    std::optional<aiSDK::MCPTransportRequestContext> last_ping_commit_context_;
};

aiSDK::MCPServerConfig makeConfig() {
    aiSDK::MCPServerConfig config;
    config.server_id = "fake";
    config.limits.request_timeout = 1s;
    config.limits.absolute_request_timeout = 3s;
    config.limits.close_timeout = 1s;
    aiSDK::MCPStreamableHttpConfig http;
    http.endpoint = "http://127.0.0.1:65530/mcp";
    http.allow_loopback_http = true;
    http.credential_timeout = 100ms;
    config.transport = std::move(http);
    return config;
}

bool waitForClientState(const aiSDK::MCPClient& client, aiSDK::MCPClientState expected,
                        std::chrono::milliseconds timeout) {
    // Transport 回调先进入 Client 的入站队列；测试必须等待 Worker 消费后再观察最终状态。
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while(std::chrono::steady_clock::now() < deadline) {
        if(client.state() == expected) {
            return true;
        }
        std::this_thread::yield();
    }
    return client.state() == expected;
}

TEST(MCPClientTest, 连接后支持分页列举和完整工具调用结果) {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->enableMultiPageList();
    aiSDK::MCPClient client(makeConfig(), transport);

    client.connect();
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Ready);
    EXPECT_EQ(client.listenerState(), aiSDK::MCPListenerState::Unsupported);

    const auto catalog = client.listTools();
    ASSERT_TRUE(catalog.valid());
    ASSERT_EQ(catalog.tools().size(), 2U);
    EXPECT_EQ(catalog.tools()[0].name, "echo");
    EXPECT_TRUE(catalog.tools()[0].raw.at("x-test-extension"));

    const auto result = client.callTool(catalog, "echo",
                                        {
                                            {"value", "中文"}
    });
    EXPECT_FALSE(result.is_error);
    EXPECT_EQ(result.content.at(0).at("text"), "调用成功");
    EXPECT_EQ(result.structured_content.at("arguments").at("value"), "中文");
    EXPECT_EQ(result.raw_result.at("extension"), "保留");
    client.close();
}

TEST(MCPClientTest, 毫秒上界截止时间饱和而不会立即超时) {
    auto transport = std::make_shared<ScriptedTransport>();
    auto config = makeConfig();
    // 最大 milliseconds 超过 steady_clock 常见可加范围，直接 now + timeout 可能回绕。
    config.limits.request_timeout = std::chrono::milliseconds::max();
    config.limits.absolute_request_timeout = std::chrono::milliseconds::max();
    config.limits.close_timeout = std::chrono::milliseconds::max();
    aiSDK::MCPClient client(std::move(config), transport);

    EXPECT_NO_THROW(client.connect());
    EXPECT_NO_THROW(client.ping());
    client.close();
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Closed);
}

TEST(MCPClientTest, 请求段只在原子提交后启动且准备保留完整绝对预算) {
    // ScriptedTransport 在 prepare 阶段停住，不会发送任何字节，也不会依赖任意休眠推进时序。
    auto transport = std::make_shared<ScriptedTransport>();
    auto config = makeConfig();
    config.limits.request_timeout = 100ms;
    config.limits.absolute_request_timeout = 2s;
    aiSDK::MCPClient client(std::move(config), transport);
    client.connect();
    transport->blockPingPrepare();

    std::exception_ptr ping_failure;
    std::thread ping_thread([&] {
        try {
            client.ping();
        } catch(...) {
            ping_failure = std::current_exception();
        }
    });
    const bool preparing = transport->waitForPingPrepare(1s);
    const auto prepare_context = transport->lastPingPrepareContext();

    ASSERT_TRUE(preparing);
    ASSERT_TRUE(prepare_context.has_value());
    // 此时 request_timeout 尚未开始，准备上下文必须保有远大于 100ms 的操作绝对上限。
    EXPECT_GT(prepare_context->deadline - std::chrono::steady_clock::now(), 1s);
    EXPECT_EQ(prepare_context->deadline, prepare_context->operation_deadline);

    transport->releasePingPrepare();
    ping_thread.join();
    ASSERT_EQ(ping_failure, nullptr);
    const auto commit_context = transport->lastPingCommitContext();
    ASSERT_TRUE(commit_context.has_value());
    // 提交后上下文改为短请求段，而绝对上限字段仍保留供 HTTP SSE 与恢复路径使用。
    EXPECT_LE(commit_context->deadline - std::chrono::steady_clock::now(), 100ms);
    EXPECT_EQ(commit_context->operation_deadline, prepare_context->operation_deadline);
    client.close();
}

TEST(MCPClientTest, 目录变化使旧快照失效且调用保持零写入) {
    auto transport = std::make_shared<ScriptedTransport>();
    aiSDK::MCPClient client(makeConfig(), transport);
    client.connect();
    const auto catalog = client.listTools();
    const std::size_t writes_before = transport->commitCount();

    // 入站消息现在经过固定协议 Worker；回调是无 sleep 的处理完成屏障。
    std::mutex callback_mutex;
    std::condition_variable callback_cv;
    bool catalog_invalidated = false;
    client.setCatalogChangedCallback([&](const std::string&, std::uint64_t) {
        std::lock_guard<std::mutex> lock(callback_mutex);
        catalog_invalidated = true;
        callback_cv.notify_all();
    });
    transport->emitListChanged();
    {
        std::unique_lock<std::mutex> lock(callback_mutex);
        ASSERT_TRUE(callback_cv.wait_for(lock, 1s, [&] { return catalog_invalidated; }));
    }
    EXPECT_TRUE(client.isToolCatalogStale());
    EXPECT_EQ(client.catalogRevision(), 1U);
    try {
        static_cast<void>(client.callTool(catalog, "echo", nlohmann::json::object()));
        FAIL() << "旧 Catalog 必须被拒绝";
    } catch(const aiSDK::MCPException& exception) {
        EXPECT_EQ(exception.code(), aiSDK::MCPErrorCode::ToolCatalogStale);
    }
    EXPECT_EQ(transport->commitCount(), writes_before);
    client.close();
}

TEST(MCPClientTest, 入站队列溢出会终止已提交工具且不丢弃故障) {
    auto transport = std::make_shared<ScriptedTransport>();
    auto config = makeConfig();
    // 两条容量让第三条不可合并 Server 请求稳定跨过共享队列上限。
    config.limits.max_pending_messages = 2U;
    aiSDK::MCPClient client(std::move(config), transport);
    client.connect();
    const auto catalog = client.listTools();

    std::mutex callback_mutex;
    std::condition_variable callback_cv;
    bool callback_entered = false;
    bool release_callback = false;
    client.setCatalogChangedCallback([&](const std::string&, std::uint64_t) {
        std::unique_lock<std::mutex> lock(callback_mutex);
        callback_entered = true;
        callback_cv.notify_all();
        callback_cv.wait(lock, [&] { return release_callback; });
    });

    // 先让 tools/call 进入 Submitted，随后才用目录通知暂停协议 Worker。
    transport->blockToolCall();
    std::exception_ptr call_failure;
    std::mutex call_mutex;
    std::condition_variable call_cv;
    bool call_finished = false;
    std::thread call_thread([&] {
        try {
            static_cast<void>(client.callTool(catalog, "echo", nlohmann::json::object()));
        } catch(...) {
            call_failure = std::current_exception();
        }
        std::lock_guard<std::mutex> lock(call_mutex);
        call_finished = true;
        call_cv.notify_all();
    });
    const bool tool_committed = transport->waitForToolCallCommit(1s);
    if(!tool_committed) {
        client.close();
        call_thread.join();
        FAIL() << "tools/call 未进入可观察的 Submitted 阶段";
    }

    transport->emitListChanged();
    bool entered_callback = false;
    {
        std::unique_lock<std::mutex> lock(callback_mutex);
        entered_callback = callback_cv.wait_for(lock, 1s, [&] { return callback_entered; });
    }
    if(!entered_callback) {
        client.close();
        call_thread.join();
        FAIL() << "目录回调未进入协议 Worker 屏障";
    }

    // 协议 Worker 被上面的用户回调暂停，因此这三条不可丢 Server 请求只能进入入站队列。
    transport->emitServerPing("overflow-1");
    transport->emitServerPing("overflow-2");
    transport->emitServerPing("overflow-3");

    bool tool_finished = false;
    {
        std::unique_lock<std::mutex> lock(call_mutex);
        tool_finished = call_cv.wait_for(lock, 1s, [&] { return call_finished; });
    }
    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        release_callback = true;
        callback_cv.notify_all();
    }
    if(!tool_finished) {
        client.close();
        call_thread.join();
        FAIL() << "队列溢出后已提交工具未在截止内完成";
    }
    call_thread.join();

    ASSERT_NE(call_failure, nullptr);
    try {
        std::rethrow_exception(call_failure);
    } catch(const aiSDK::MCPException& exception) {
        EXPECT_EQ(exception.code(), aiSDK::MCPErrorCode::OutcomeUnknown);
        ASSERT_TRUE(exception.causeCode().has_value());
        EXPECT_EQ(*exception.causeCode(), aiSDK::MCPErrorCode::MessageQueueOverflow);
        EXPECT_TRUE(exception.mayHaveExecuted());
    }
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Faulted);
    ASSERT_TRUE(client.lastFailureCode().has_value());
    EXPECT_EQ(*client.lastFailureCode(), aiSDK::MCPErrorCode::MessageQueueOverflow);
    client.close();
}

TEST(MCPClientTest, 公开异常文本遵守UTF8字节上限且保留结构化字段) {
    auto transport = std::make_shared<ScriptedTransport>();
    auto config = makeConfig();
    // 六字节只容纳两个完整的省略号码点，能同时覆盖极小上限和 UTF-8 边界。
    config.limits.max_error_text_bytes = 6U;
    aiSDK::MCPClient client(std::move(config), transport);
    client.connect();
    const auto catalog = client.listTools();
    transport->emitListChanged();

    try {
        static_cast<void>(client.callTool(catalog, "echo", nlohmann::json::object()));
        FAIL() << "旧 Catalog 必须触发受限公开异常";
    } catch(const aiSDK::MCPException& exception) {
        EXPECT_EQ(exception.code(), aiSDK::MCPErrorCode::ToolCatalogStale);
        EXPECT_LE(std::string(exception.what()).size(), 6U);
        EXPECT_FALSE(exception.causeCode().has_value());
        EXPECT_FALSE(exception.mayHaveExecuted());
    }
    client.close();
}

TEST(MCPClientTest, 已提交工具在传输失败后返回结果未知) {
    auto transport = std::make_shared<ScriptedTransport>();
    aiSDK::MCPClient client(makeConfig(), transport);
    client.connect();
    const auto catalog = client.listTools();
    transport->failNextToolCall();

    try {
        static_cast<void>(client.callTool(catalog, "echo", nlohmann::json::object()));
        FAIL() << "已提交工具失败必须提升为 OutcomeUnknown";
    } catch(const aiSDK::MCPException& exception) {
        EXPECT_EQ(exception.code(), aiSDK::MCPErrorCode::OutcomeUnknown);
        ASSERT_TRUE(exception.causeCode().has_value());
        EXPECT_EQ(*exception.causeCode(), aiSDK::MCPErrorCode::TransportFailure);
        EXPECT_TRUE(exception.mayHaveExecuted());
        EXPECT_EQ(exception.clientStateAtFailure(), aiSDK::MCPClientState::Ready);
    }
    client.close();
}

TEST(MCPClientTest, 后台Listener不占槽但另一个公开操作会返回忙) {
    auto transport = std::make_shared<ScriptedTransport>();
    aiSDK::MCPClient client(makeConfig(), transport);
    client.connect();
    transport->blockPing();

    std::exception_ptr ping_failure;
    std::thread ping_thread([&] {
        try {
            client.ping();
        } catch(...) {
            ping_failure = std::current_exception();
        }
    });
    ASSERT_TRUE(transport->waitForPingCommit(1s));
    try {
        static_cast<void>(client.listTools());
        FAIL() << "并发公开操作必须立即返回 ClientBusy";
    } catch(const aiSDK::MCPException& exception) {
        EXPECT_EQ(exception.code(), aiSDK::MCPErrorCode::ClientBusy);
    }

    transport->releasePing();
    ping_thread.join();
    EXPECT_EQ(ping_failure, nullptr);
    client.close();
}

TEST(MCPClientTest, ServerPing由后台控制流独立响应) {
    auto transport = std::make_shared<ScriptedTransport>();
    aiSDK::MCPClient client(makeConfig(), transport);
    client.connect();

    transport->emitServerPing("server-request-1");
    EXPECT_TRUE(transport->waitForServerResponse(1s));
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Ready);
    client.close();
}

TEST(MCPClientTest, 请求错误先到时迟到响应不能覆盖) {
    auto transport = std::make_shared<ScriptedTransport>();
    aiSDK::MCPClient client(makeConfig(), transport);
    client.connect();
    transport->blockPing();

    std::exception_ptr ping_failure;
    std::thread ping_thread([&] {
        try {
            client.ping();
        } catch(...) {
            ping_failure = std::current_exception();
        }
    });
    const bool committed = transport->waitForPingCommit(1s);
    if(committed) {
        // SendFailed 先完成请求，同 ID 响应紧随其后投递。
        transport->emitCapturedPingFailure(aiSDK::MCPErrorCode::TransportFailure);
        transport->emitCapturedPingResponse();
    }
    transport->releasePing();
    ping_thread.join();

    ASSERT_TRUE(committed);
    ASSERT_NE(ping_failure, nullptr);
    try {
        std::rethrow_exception(ping_failure);
    } catch(const aiSDK::MCPException& exception) {
        EXPECT_EQ(exception.code(), aiSDK::MCPErrorCode::TransportFailure);
        EXPECT_EQ(exception.clientStateAtFailure(), aiSDK::MCPClientState::Ready);
    }
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Ready);
    client.close();
}

TEST(MCPClientTest, 故障先到时迟到响应不能改写根因) {
    auto transport = std::make_shared<ScriptedTransport>();
    aiSDK::MCPClient client(makeConfig(), transport);
    client.connect();
    transport->blockPing();

    std::exception_ptr ping_failure;
    std::thread ping_thread([&] {
        try {
            client.ping();
        } catch(...) {
            ping_failure = std::current_exception();
        }
    });
    const bool committed = transport->waitForPingCommit(1s);
    if(committed) {
        transport->emitTransportFault(aiSDK::MCPErrorCode::ServerExited);
        transport->emitCapturedPingResponse();
    }
    transport->releasePing();
    ping_thread.join();

    ASSERT_TRUE(committed);
    ASSERT_NE(ping_failure, nullptr);
    try {
        std::rethrow_exception(ping_failure);
    } catch(const aiSDK::MCPException& exception) {
        EXPECT_EQ(exception.code(), aiSDK::MCPErrorCode::ServerExited);
        EXPECT_EQ(exception.clientStateAtFailure(), aiSDK::MCPClientState::Faulted);
    }
    ASSERT_TRUE(client.lastFailureCode().has_value());
    EXPECT_EQ(*client.lastFailureCode(), aiSDK::MCPErrorCode::ServerExited);
    client.close();
}

TEST(MCPClientTest, 成功响应后的重复同ID响应使Client故障) {
    auto transport = std::make_shared<ScriptedTransport>();
    aiSDK::MCPClient client(makeConfig(), transport);
    client.connect();
    transport->blockPing();

    std::exception_ptr ping_failure;
    std::thread ping_thread([&] {
        try {
            client.ping();
        } catch(...) {
            ping_failure = std::current_exception();
        }
    });
    const bool committed = transport->waitForPingCommit(1s);
    if(committed) {
        // 两次回调在测试线程内严格排序，首个结果已知，第二个结果只能触发协议故障。
        transport->emitCapturedPingResponse();
        transport->emitCapturedPingResponse();
    }
    transport->releasePing();
    ping_thread.join();

    ASSERT_TRUE(committed);
    EXPECT_EQ(ping_failure, nullptr);
    ASSERT_TRUE(waitForClientState(client, aiSDK::MCPClientState::Faulted, 1s));
    ASSERT_TRUE(client.lastFailureCode().has_value());
    EXPECT_EQ(*client.lastFailureCode(), aiSDK::MCPErrorCode::ProtocolViolation);
    client.close();
}

TEST(MCPClientTest, close先完成已提交工具且迟到响应无效) {
    auto transport = std::make_shared<ScriptedTransport>();
    aiSDK::MCPClient client(makeConfig(), transport);
    client.connect();
    const auto catalog = client.listTools();
    transport->blockToolCall();
    transport->blockClose();

    std::exception_ptr call_failure;
    std::thread call_thread([&] {
        try {
            static_cast<void>(client.callTool(catalog, "echo", nlohmann::json::object()));
        } catch(...) {
            call_failure = std::current_exception();
        }
    });
    const bool committed = transport->waitForToolCallCommit(1s);
    std::thread close_thread;
    if(committed) {
        close_thread = std::thread([&] { client.close(); });
    }
    const bool close_entered = committed && transport->waitForCloseEntered(1s);
    if(close_entered) {
        // Transport close 被测试屏障暂停，但 Client 的取消线性化点已经建立。
        transport->emitCapturedToolResponse();
        transport->releaseClose();
    } else if(committed) {
        transport->releaseClose();
    }
    call_thread.join();
    if(close_thread.joinable()) {
        close_thread.join();
    }

    ASSERT_TRUE(committed);
    ASSERT_TRUE(close_entered);
    ASSERT_NE(call_failure, nullptr);
    try {
        std::rethrow_exception(call_failure);
    } catch(const aiSDK::MCPException& exception) {
        EXPECT_EQ(exception.code(), aiSDK::MCPErrorCode::OutcomeUnknown);
        ASSERT_TRUE(exception.causeCode().has_value());
        EXPECT_EQ(*exception.causeCode(), aiSDK::MCPErrorCode::OperationCancelled);
        EXPECT_TRUE(exception.mayHaveExecuted());
        EXPECT_EQ(exception.clientStateAtFailure(), aiSDK::MCPClientState::Closing);
    }
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Closed);
    EXPECT_EQ(transport->closeCallCount(), 1U);
}

TEST(MCPClientTest, 并发close只有一个Transport关闭所有者) {
    auto transport = std::make_shared<ScriptedTransport>();
    aiSDK::MCPClient client(makeConfig(), transport);
    client.connect();
    transport->blockClose();

    std::thread first_close([&] { client.close(); });
    const bool close_entered = transport->waitForCloseEntered(1s);
    std::thread second_close;
    if(close_entered) {
        second_close = std::thread([&] { client.close(); });
        transport->releaseClose();
    }
    first_close.join();
    if(second_close.joinable()) {
        second_close.join();
    }

    ASSERT_TRUE(close_entered);
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Closed);
    EXPECT_EQ(transport->closeCallCount(), 1U);
}

TEST(MCPClientTest, close取消未提交准备且保持零写入) {
    auto transport = std::make_shared<ScriptedTransport>();
    aiSDK::MCPClient client(makeConfig(), transport);
    client.connect();
    const std::size_t writes_before = transport->commitCount();
    transport->blockPingPrepare();
    transport->blockClose();

    std::exception_ptr ping_failure;
    std::thread ping_thread([&] {
        try {
            client.ping();
        } catch(...) {
            ping_failure = std::current_exception();
        }
    });
    const bool preparing = transport->waitForPingPrepare(1s);
    std::thread close_thread;
    if(preparing) {
        close_thread = std::thread([&] { client.close(); });
    }
    const bool close_entered = preparing && transport->waitForCloseEntered(1s);
    ping_thread.join();
    transport->releaseClose();
    if(close_thread.joinable()) {
        close_thread.join();
    }

    ASSERT_TRUE(preparing);
    ASSERT_TRUE(close_entered);
    ASSERT_NE(ping_failure, nullptr);
    try {
        std::rethrow_exception(ping_failure);
    } catch(const aiSDK::MCPException& exception) {
        EXPECT_EQ(exception.code(), aiSDK::MCPErrorCode::OperationCancelled);
        EXPECT_FALSE(exception.mayHaveExecuted());
    }
    EXPECT_EQ(transport->commitCount(), writes_before);
}

TEST(MCPClientTest, Provider准备期间故障抛出首个真实根因) {
    auto transport = std::make_shared<ScriptedTransport>();
    aiSDK::MCPClient client(makeConfig(), transport);
    client.connect();
    transport->blockPingPrepare();

    std::exception_ptr ping_failure;
    std::thread ping_thread([&] {
        try {
            client.ping();
        } catch(...) {
            ping_failure = std::current_exception();
        }
    });
    const bool preparing = transport->waitForPingPrepare(1s);
    if(preparing) {
        transport->emitTransportFault(aiSDK::MCPErrorCode::ServerExited);
        transport->releasePingPrepare();
    }
    ping_thread.join();

    ASSERT_TRUE(preparing);
    ASSERT_NE(ping_failure, nullptr);
    try {
        std::rethrow_exception(ping_failure);
    } catch(const aiSDK::MCPException& exception) {
        EXPECT_EQ(exception.code(), aiSDK::MCPErrorCode::ServerExited);
        EXPECT_EQ(exception.clientStateAtFailure(), aiSDK::MCPClientState::Faulted);
    } catch(const std::logic_error&) {
        FAIL() << "Provider 竞态不得泄漏 logic_error";
    }
    ASSERT_TRUE(client.lastFailureCode().has_value());
    EXPECT_EQ(*client.lastFailureCode(), aiSDK::MCPErrorCode::ServerExited);
    client.close();
}

TEST(MCPClientTest, Faulted只保留第一个故障根因) {
    auto transport = std::make_shared<ScriptedTransport>();
    aiSDK::MCPClient client(makeConfig(), transport);
    client.connect();

    transport->emitTransportFault(aiSDK::MCPErrorCode::SessionExpired);
    transport->emitTransportFault(aiSDK::MCPErrorCode::ProtocolViolation);

    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Faulted);
    ASSERT_TRUE(client.lastFailureCode().has_value());
    EXPECT_EQ(*client.lastFailureCode(), aiSDK::MCPErrorCode::SessionExpired);
    client.close();
}

TEST(MCPClientTest, Listener监听缺口递增目录代次并回调) {
    auto transport = std::make_shared<ScriptedTransport>();
    aiSDK::MCPClient client(makeConfig(), transport);
    client.connect();
    transport->emitListenerEvent(aiSDK::MCPTransportEventType::ListenerStarted);
    const auto catalog = client.listTools();
    ASSERT_TRUE(catalog.valid());

    std::vector<std::uint64_t> callback_revisions;
    client.setCatalogChangedCallback(
        [&](const std::string&, std::uint64_t revision) { callback_revisions.push_back(revision); });
    transport->emitListenerEvent(aiSDK::MCPTransportEventType::ListenerRecovering);
    transport->emitListenerEvent(aiSDK::MCPTransportEventType::ListenerUnavailable);

    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Ready);
    EXPECT_EQ(client.listenerState(), aiSDK::MCPListenerState::Unavailable);
    EXPECT_TRUE(client.isToolCatalogStale());
    EXPECT_EQ(client.catalogRevision(), 2U);
    EXPECT_EQ(callback_revisions, (std::vector<std::uint64_t>{1U, 2U}));
    client.close();
}

TEST(MCPClientTest, Listener意外停止先使目录失效再进入故障) {
    auto transport = std::make_shared<ScriptedTransport>();
    aiSDK::MCPClient client(makeConfig(), transport);
    client.connect();
    transport->emitListenerEvent(aiSDK::MCPTransportEventType::ListenerStarted);
    static_cast<void>(client.listTools());

    std::uint64_t callback_revision = 0U;
    client.setCatalogChangedCallback([&](const std::string&, std::uint64_t revision) { callback_revision = revision; });
    transport->emitListenerEvent(aiSDK::MCPTransportEventType::ListenerStopped, aiSDK::MCPErrorCode::ServerExited);

    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Faulted);
    EXPECT_TRUE(client.isToolCatalogStale());
    EXPECT_EQ(client.catalogRevision(), 1U);
    EXPECT_EQ(callback_revision, 1U);
    ASSERT_TRUE(client.lastFailureCode().has_value());
    EXPECT_EQ(*client.lastFailureCode(), aiSDK::MCPErrorCode::ServerExited);
    client.close();
}

TEST(MCPClientTest, Server超大无符号ID保持原值响应) {
    auto transport = std::make_shared<ScriptedTransport>();
    aiSDK::MCPClient client(makeConfig(), transport);
    client.connect();
    const auto request_id = std::numeric_limits<std::uint64_t>::max();

    transport->emitServerPing(request_id);

    ASSERT_TRUE(transport->waitForServerResponse(1s));
    const auto response_id = transport->serverResponseId();
    ASSERT_TRUE(response_id.has_value());
    ASSERT_TRUE(response_id->is_number_unsigned());
    EXPECT_EQ(response_id->get<std::uint64_t>(), request_id);
    client.close();
}

TEST(MCPToolAdapterTest, 显式绑定注册执行和注销保持可审计) {
    auto transport = std::make_shared<ScriptedTransport>();
    auto client = std::make_shared<aiSDK::MCPClient>(makeConfig(), transport);
    client->connect();
    const auto catalog = client->listTools();

    const auto bindings = aiSDK::MCPToolAdapter::adaptTools(client, catalog,
                                                            {
                                                                {"echo", std::nullopt, std::nullopt}
    });
    ASSERT_EQ(bindings.size(), 1U);
    EXPECT_EQ(bindings[0].tool.name, "fake__echo");
    EXPECT_EQ(bindings[0].tool.risk_level, aiSDK::ToolRiskLevel::High);

    aiSDK::ToolRegistry registry;
    aiSDK::MCPToolAdapter::registerBindings(registry, bindings);
    const auto result = registry.execute("fake__echo", {
                                                           {"value", 7}
    });
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.data.at("structuredContent").at("arguments").at("value"), 7);

    aiSDK::MCPToolAdapter::unregisterBindings(registry, bindings);
    EXPECT_FALSE(registry.hasTool("fake__echo"));
    client->close();
}

TEST(MCPToolAdapterTest, 目录过期的旧Binding返回稳定失败且不调用远端) {
    auto transport = std::make_shared<ScriptedTransport>();
    auto client = std::make_shared<aiSDK::MCPClient>(makeConfig(), transport);
    client->connect();
    const auto catalog = client->listTools();
    const auto bindings = aiSDK::MCPToolAdapter::adaptTools(client, catalog,
                                                            {
                                                                {"echo", "safe_echo", aiSDK::ToolRiskLevel::Medium}
    });
    const std::size_t writes_before = transport->commitCount();

    std::mutex invalidation_mutex;
    std::condition_variable invalidation_cv;
    bool catalog_invalidated = false;
    client->setCatalogChangedCallback([&](const std::string&, std::uint64_t) {
        std::lock_guard<std::mutex> lock(invalidation_mutex);
        catalog_invalidated = true;
        invalidation_cv.notify_all();
    });
    transport->emitListChanged();
    {
        std::unique_lock<std::mutex> lock(invalidation_mutex);
        ASSERT_TRUE(invalidation_cv.wait_for(lock, 1s, [&] { return catalog_invalidated; }));
    }
    const auto result = bindings[0].handler(nlohmann::json::object());
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_message, "MCP 工具目录已失效，请重新列举并绑定");
    EXPECT_EQ(transport->commitCount(), writes_before);
    client->close();
}

TEST(MCPToolAdapterTest, 所有固定失败文本也遵守适配器字节上限) {
    auto transport = std::make_shared<ScriptedTransport>();
    auto client = std::make_shared<aiSDK::MCPClient>(makeConfig(), transport);
    client->connect();
    const auto catalog = client->listTools();
    aiSDK::MCPToolAdapterOptions options;
    options.max_error_text_bytes = 6U;
    const auto bindings = aiSDK::MCPToolAdapter::adaptTools(client, catalog,
                                                            {
                                                                {"echo", "bounded_echo", std::nullopt}
    },
                                                            options);

    std::mutex invalidation_mutex;
    std::condition_variable invalidation_cv;
    bool catalog_invalidated = false;
    client->setCatalogChangedCallback([&](const std::string&, std::uint64_t) {
        std::lock_guard<std::mutex> lock(invalidation_mutex);
        catalog_invalidated = true;
        invalidation_cv.notify_all();
    });
    transport->emitListChanged();
    {
        std::unique_lock<std::mutex> lock(invalidation_mutex);
        ASSERT_TRUE(invalidation_cv.wait_for(lock, 1s, [&] { return catalog_invalidated; }));
    }
    const auto result = bindings[0].handler(nlohmann::json::object());
    EXPECT_FALSE(result.success);
    EXPECT_LE(result.error_message.size(), options.max_error_text_bytes);
    client->close();
}

}  // namespace
