#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "mcp/detail/MCPDeadline.h"
#include "mcp/detail/MCPTransportFactory.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cpr/cpr.h>
#include <curl/curl.h>

#include <nlohmann/json.hpp>

#include "mcp/detail/MCPHttpTestFactory.h"
#include "mcp/detail/MCPProtocol.h"
#include "mcp/detail/MCPSseDecoder.h"

namespace aiSDK {
namespace detail {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kPostWorkerCount = 2U;
constexpr std::string_view kJsonContentType = "application/json";
constexpr std::string_view kSseContentType = "text/event-stream";

// 本文件保持以下并发不变量：
// 1. 构造函数只复制配置，不创建线程或执行网络 I/O。
// 2. prepareMessage 在队列锁外序列化并调用凭据 Provider。
// 3. commitPrepared 只校验所有权并向有界队列移动一次性对象。
// 4. 两个固定 POST Worker 消费全部前台与控制 POST。
// 5. Listener 使用唯一长期线程，不占用 POST Worker。
// 6. 每个 HTTP 尝试拥有独立 cpr Session 与取消标志。
// 7. Listener 的事件 ID 和 retry 从不与 POST SSE 共享。
// 8. 任何工具请求都不会因 POST SSE 中断而自动重放。
// 9. 所有 Client 回调均在 Transport 状态锁外执行。
// 10. close 先关闭回调入口，再取消 Session 和回收线程。
// 11. 回调正文、HTTP 错误正文和凭据不会进入公开错误文本。
// 12. 会话 ID 只接受有界可见 ASCII，并在 close 时主动擦除。
// 13. 每次请求都显式禁用重定向、代理并启用 TLS 校验。
// 14. Listener 只有限恢复，连续成功事件才重置失败次数。
// 15. 凭据 Provider 在单个 Client 内串行调用，避免刷新竞态。
// 16. 前台 POST 与监听 GET 分别保存恢复游标，禁止跨通道续接事件。
// 17. 所有响应正文均受 max_message_bytes 限制，错误正文不参与诊断拼接。
// 18. 初始化响应是唯一可建立会话标识的响应，其他响应不能覆盖会话。
// 19. 控制请求只接受协议明确规定的空响应状态，不猜测成功语义。
// 20. 关闭先使新提交失败，再取消活动请求，最后回收固定后台线程。
// 21. HTTP 状态、协议错误与取消原因在传输边界映射为闭合错误枚举。
// 22. 测试覆盖项通过专用内部工厂注入，生产配置不暴露降低 TLS 强度的入口。
// 23. 资源计数仅存在于测试构建，不能改变生产请求的生命周期和调度顺序。
// 24. 每个超时判断都基于 steady_clock，系统时间调整不会延长网络操作。
// 25. 分段重试共享同一绝对截止点，恢复次数不会重置公开操作总预算。
// 26. 请求队列容量在提交线性化点检查，失败请求不会产生网络副作用。
// 27. close 对队列中尚未发送的请求给出确定取消，不允许静默丢弃等待者。
// 28. 已提交工具请求的传输不确定性由 Client 层提升为结果未知。
// 29. Transport 不依据工具注解猜测幂等性，也不自动重试工具调用。
// 30. Listener 的 405 只表示监听能力不支持，不改变会话可用状态。
// 31. Listener 的认证或协议故障会通过状态回调交由 Client 决定主状态。
// 32. SSE 解析器只接收当前响应字节，响应结束后不会保留正文引用。
// 33. Header 值进入状态前完成大小和字符约束校验，拒绝歧义会话标识。
// 34. 每个取消标志只归属一次物理请求，禁止跨 Session 复用取消状态。
// 35. Worker 顶层异常边界保证线程异常不会越过库边界终止进程。
// 36. 测试活动计数采用 RAII 配对，异常路径与正常路径使用同一减计数逻辑。
// 37. 内部日志和回调不输出认证 Header、会话标识或远端错误正文。
// 38. 公开事件只携带协议处理所需的 JSON 和闭合元数据，不暴露网络对象。
// 39. open 使用 Client 生成的初始化绝对截止点，线程创建不得另起超时段。
// 40. open 在发布每个固定 Worker 前检查同一截止点，避免发布过期资源集合。
// 41. 部分线程创建失败与打开超时共享同一收敛路径，已经启动的线程必须 join。
// 42. 打开失败后先撤销回调再唤醒 Worker，Client 不会收到半初始化事件。
// 43. close 使用 Client 生成的唯一关闭绝对截止点，不读取配置重建预算。
// 44. DELETE 只能消费关闭截止点的剩余时间，失败不得阻碍本地资源回收。
// 45. 活动 Session 的取消标志在关闭锁内发布，使网络回调能及时停止传输。
// 46. 关闭时清空待发送队列，未提交的物理请求不会在 Closing 后开始。
// 47. 关闭等待谓词覆盖 POST Worker、Listener 和在途 Client 回调三类资源。
// 48. 固定线程最终仍要 join，禁止通过 detach 把 core_ 生命周期交给后台竞态。
// 49. 协作式 Provider 可观察取消并收敛，非协作用户代码不能由 C++ 安全强停。
// 50. 非协作 Provider 的能力边界不得通过捕获裸 this 或分离线程规避。
// 51. 析构仅使用立即截止点兜底，显式 MCPClient::close 是正常关闭入口。
// 52. close 返回后回调集合已经撤销，迟到网络完成不能再次进入 MCPClient。
// 53. close_complete 只在固定线程收敛后发布，不能把取消请求误当资源已释放。
// 54. 生命周期互斥量串行化 open 与 close，避免两个调用者重复 join 同一线程。
// 55. 状态互斥量只保护 core 数据，不跨越 DELETE、网络等待或线程 join。
// 56. 关闭截止时间使用 steady_clock，壁钟回拨不能延长会话清理。
// 57. 已耗尽关闭预算时跳过远端 DELETE，但仍执行取消和安全线程回收。
// 58. 测试覆盖的活动资源计数必须在 close 返回时归零，验证没有后台遗留。

// 实现合同 A：对象寿命与依赖边界。
// A01. 公开头文件不依赖 cpr、libcurl、线程或平台网络句柄。
// A02. 工厂返回 IMCPTransport，调用方不能观察具体传输类型。
// A03. Transport 构造只复制已校验配置，不解析 DNS 或打开 Socket。
// A04. open 只建立固定执行资源，不向 Endpoint 发出探测请求。
// A05. open 安装的两个回调在整个打开周期内保持同一语义。
// A06. 回调函数副本只在受保护状态中读取，实际调用发生在锁外。
// A07. 后台线程只捕获共享 Core，不捕获外层对象地址。
// A08. close 会等待全部固定线程退出，Transport 析构后不遗留后台任务。
// A09. 外层析构总是调用幂等 close，不要求应用显式清理。
// A10. close 完成后再次 close 只读取终态，不重复 DELETE 会话。
// A11. open 与 close 通过寿命互斥量串行，线程容器没有数据竞争。
// A12. prepare、commit 与状态读取不持有寿命互斥量，避免扩大临界区。
// A13. Core 状态锁只保护 SDK 自有字段，不保护用户 Provider 执行。
// A14. 凭据门独立于 Core 状态锁，避免 Provider 与 close 形成锁环。
// A15. 出站队列只保存 PreparedHttpMessage 的唯一所有权。
// A16. PreparedHttpMessage 不能复制，提交后原调用方不再持有秘密。
// A17. 活动取消集合保存弱引用，不人为延长已完成 Session 寿命。
// A18. 清理弱引用发生在登记新请求时，成本受活动并发数约束。
// A19. 两个 POST Worker 是常量，不随请求数扩展线程数量。
// A20. Listener 线程唯一，首次启动之后不创建替代监听线程。
// A21. 每个物理 HTTP 尝试仍使用独立 cpr Session，禁止跨线程共享。
// A22. cpr 与 libcurl 具体类型只存在本编译单元的匿名命名空间。
// A23. nlohmann::json 只跨 IMCPTransport 的既有公共消息边界。
// A24. Transport 不解释 Tools 能力、目录代次或工具副作用语义。
// A25. Transport 只依据 RequestKind 选择 HTTP 成功与凭据用途规则。
// A26. Client 负责 JSON-RPC ID 生命周期，Transport 只辅助匹配响应。
// A27. Listener 事件 dispatch_id 固定为零，因为它不属于前台操作。
// A28. POST 事件保留 Client 分配的 dispatch_id，以闭合精确等待者。
// A29. 错误事件只携带闭合枚举与状态码，不携带远端正文。
// A30. 内部异常文本使用固定中文，不拼接 URL、Token 或 Session ID。
// A31. cpr 动态错误文本不会穿过本文件的错误映射边界。
// A32. 远端 Header 只保留协议决策需要的两个字段。
// A33. 远端正文完成解析后立即离开 Transport 聚合缓冲。
// A34. Listener 不聚合整个响应，只保留单个 SSE 事件状态。
// A35. POST JSON 聚合由 max_message_bytes 给出绝对内存上限。
// A36. 关闭清空队列会同步析构尚未发送的秘密快照。
// A37. 关闭中的线程无法再取得 Client 回调，因为 closing 永久为真。
// A38. 所有 Core 终态转换都通过同一互斥量建立可见性。

// 实现合同 B：准备、凭据与提交线性化。
// B01. prepareMessage 是两阶段合同中唯一允许调用 Provider 的阶段。
// B02. prepareMessage 不创建 cpr Session，也不调用 curl_easy_perform。
// B03. JSON 序列化在入队前完成，序列化失败保证零网络写入。
// B04. 序列化正文先检查 max_message_bytes，再允许获取队列资格。
// B05. prepare 捕获协议版本、会话 ID 与鉴权值的同次请求快照。
// B06. Client 在 prepare 返回后负责按自身状态代次做第二次校验。
// B07. 第二次校验失败时 Prepared 析构，秘密和正文不会进入队列。
// B08. commitPrepared 成功返回是唯一 Submitted 线性化点。
// B09. commit 不获取凭据、不序列化 JSON，也不执行网络 I/O。
// B10. commit 只进行所有权、关闭状态与队列容量校验。
// B11. 来自其他 Transport 的 Prepared 对象会在入队前被拒绝。
// B12. owner 指针只用于身份比较，从不在 Core 析构后解引用。
// B13. 队列达到 max_pending_messages 时返回 MessageQueueOverflow。
// B14. 队列满失败发生在 Submitted 之前，不会产生不确定工具结果。
// B15. Worker 从队列移出对象后才开始构造物理 Session。
// B16. 队列锁不会跨越 cpr Session 构造或 curl_easy_perform。
// B17. 多个控制 POST 可以与前台 POST 使用不同 Worker 并发执行。
// B18. 单前台操作限制仍由 MCPClient 承担，而非 Transport 猜测。
// B19. 匿名鉴权不调用 Provider，也不生成空鉴权 Header。
// B20. Bearer 模式固定 Header 名称为 Authorization。
// B21. Bearer 前缀由 SDK 添加，Provider 只返回当前 Token 值。
// B22. FixedHeader 模式原样使用构造期已校验的 Header 名称。
// B23. FixedHeader 值不会被误加 Bearer 前缀。
// B24. Provider 每条 POST、每次 GET 恢复和 DELETE 都重新调用。
// B25. 同 Client 的 Provider 调用由 credential_mutex 串行。
// B26. 不同 Client 拥有不同凭据门，允许共享 Provider 自行并发。
// B27. Provider 上下文只暴露脱敏 RequestKind 和单调截止时间。
// B28. Provider 上下文的取消视图不暴露 Core 或 Session 对象。
// B29. 凭据截止取操作截止与 credential_timeout 的较早者。
// B30. Provider 开始前先检查关闭、操作截止和取消状态。
// B31. Provider 返回后再次检查关闭，避免关闭后提交旧 Token。
// B32. Provider 返回后再次检查操作截止，避免迟到凭据触发网络。
// B33. Provider 自身超时映射 CredentialUnavailable。
// B34. 更早到达的公开操作截止映射 RequestTimeout。
// B35. close 更早线性化时映射 OperationCancelled。
// B36. Provider 异常统一脱敏为 CredentialUnavailable。
// B37. Provider 返回空值会被视为不可用而不是发送空凭据。
// B38. Provider 返回 CR 或 LF 会在创建 Header 前被拒绝。
// B39. 凭据 Header 名称的合法性由静态配置校验负责。
// B40. 实现仍不信任 Provider 值，执行逐请求动态校验。
// B41. 凭据快照析构时尽力覆盖当前字符串存储。
// B42. Prepared 析构也覆盖 cpr Session 之前持有的原始快照。
// B43. cpr Header 副本仅存在于当前 Worker 栈和 Session 寿命内。
// B44. 凭据不会写入事件、异常、Trace 或诊断字符串。
// B45. Provider 非协作永久阻塞属于公开接口明确排除的行为。
// B46. close 不会为正在占用的凭据门排队等待 DELETE 凭据。
// B47. DELETE 只有 try_lock 成功且预算充足时才调用 Provider。
// B48. DELETE Provider 使用独立 DeleteSession 脱敏用途。
// B49. DELETE 允许在 Closing 下获取凭据，但仍受关闭截止约束。
// B50. 普通请求 Provider 在 Closing 下立即观察协作取消。
// B51. Listener 首次 GET 使用 ListenerGet 凭据用途。
// B52. Listener 每次恢复 GET 使用 RecoveryGet 凭据用途。
// B53. ServerResponse POST 与 Cancellation POST 使用各自用途。
// B54. initialize、ping、list 和 call 统一归类为 ForegroundPost。
// B55. initialized 通知也使用 ForegroundPost，但按控制成功码闭合。
// B56. 凭据准备不读取或修改 Catalog，保持协议与 Agent 分层。
// B57. Session ID 变化只由成功初始化响应捕获。
// B58. prepare 不尝试刷新失效会话，也不创建新 Client。
// B59. 任何准备失败都不会自动重试 Provider 或网络请求。
// B60. Worker 不会再次调用 Provider，保证提交后的请求内容不可变。

// 实现合同 C：HTTP 安全与请求构造。
// C01. Endpoint 已由配置层限定为 HTTPS 或显式字面量回环 HTTP。
// C02. 每个 Session 都显式启用证书链校验。
// C03. 每个 Session 都显式启用 TLS 主机名校验。
// C04. 生产实现只使用系统信任库，不注入自定义 CA。
// C05. 生产实现不提供 mTLS 证书或私钥入口。
// C06. 生产实现不提供关闭 TLS 校验的逃生参数。
// C07. cpr Redirect 明确设置 follow=false。
// C08. Prepare 之后再次把 CURLOPT_FOLLOWLOCATION 固定为零。
// C09. 双重设置防止 cpr 默认重定向策略覆盖安全合同。
// C10. Prepare 之后把 CURLOPT_PROXY 明确设置为空字符串。
// C11. 空字符串覆盖 http_proxy、HTTPS_PROXY 和 ALL_PROXY 等环境值。
// C12. 实现不依赖空 Proxies map，因为它不会覆盖 libcurl 环境默认。
// C13. 请求不携带代理认证，也不会把凭据发送给环境代理。
// C14. CURLOPT_NOSIGNAL 避免多线程超时依赖进程信号。
// C15. connect_timeout 取配置值和当前剩余截止的较小值。
// C16. POST 总 Timeout 取准备后剩余的请求段截止。
// C17. Listener 不设置总寿命 Timeout，允许真正长期监听。
// C18. Listener 用 ProgressCallback 单独约束响应头截止。
// C19. Listener 用最后活动时间单独约束流空闲截止。
// C20. Listener 收到 Header 或正文分块都会刷新活动时间。
// C21. cpr CancellationParam 连接到每次请求独立原子标志。
// C22. close 设置所有仍存活取消标志，不需要共享 cpr Session。
// C23. POST 固定发送 Content-Type: application/json。
// C24. POST 固定声明 Accept: application/json, text/event-stream。
// C25. Listener GET 固定只声明 Accept: text/event-stream。
// C26. Listener GET 不发送没有语义的 Content-Type。
// C27. 初始化前请求不发送 MCP-Protocol-Version。
// C28. completeInitialization 后请求固定发送协商协议版本。
// C29. 本实现只接受 2025-11-25，其他版本立即拒绝。
// C30. 初始化请求本身不发送尚未获得的 Session ID。
// C31. 初始化成功捕获会话后，所有后续 POST 都附加该值。
// C32. Listener GET 和 DELETE 同样使用会话快照。
// C33. Last-Event-ID 只出现在 Listener 或前台响应流的恢复 GET。
// C34. 空事件 ID 表示清除游标，不生成空 Last-Event-ID Header。
// C35. POST SSE 从不读取 Listener 的 Last-Event-ID。
// C36. Listener 从不读取 POST SSE 的任何事件游标。
// C37. Header 名称由 SDK 或静态配置确定，Server 不能改写请求头。
// C38. HeaderState 采用 ASCII 不区分大小写比较字段名。
// C39. Content-Type 允许参数，但媒体类型必须精确匹配。
// C40. application/json 前缀或 text/event-stream 前缀都不会误接受。
// C41. HeaderState 支持 1xx 后继续解析最终状态块。
// C42. 新状态行会清除前一响应块的媒体类型和会话字段。
// C43. 重复 MCP-Session-Id 被视为歧义并拒绝。
// C44. Session ID 超过 max_session_id_bytes 会中止请求。
// C45. Session ID 不能为空且每个字节必须是可见 ASCII。
// C46. 空格、DEL、NUL 与非 ASCII 字节都不能进入会话存储。
// C47. 只有初始化的成功 2xx 响应可以设置会话。
// C48. 错误响应携带的 Session ID 不会污染后续请求。
// C49. Session ID 按秘密处理，不进入任何错误事件。
// C50. close 在发起 DELETE 前复制会话，并立即擦除 Core 中的原值。

// 实现合同 D：POST 响应与错误闭合。
// D01. Initialize、Ping、ListTools 和 CallTool 都要求 JSON-RPC 响应。
// D02. InitializedNotification 不要求响应正文，只接受控制成功码。
// D03. ServerResponse 不要求响应正文，只接受控制成功码。
// D04. CancellationNotification 是尽力控制 POST，但仍产生发送事件。
// D05. 控制 POST 规范成功码为 202。
// D06. 控制 POST 兼容接受 204，且不会伪造 JSON-RPC 消息。
// D07. 控制 POST 的其他 2xx 不会被宽松视为成功。
// D08. 请求 POST 的任意非 2xx 映射闭合 HTTP 错误。
// D09. 所有 POST 的 401 与 403 优先映射 AuthenticationRequired。
// D10. 已有会话的后续 POST 404 优先映射 SessionExpired。
// D11. SessionExpired 使用独立终命事件让 Client 使整个会话失效。
// D12. 没有会话的初始化 404 只是普通 HttpStatusError。
// D13. 3xx 不跟随 Location，按 HttpStatusError 完成当前请求。
// D14. 5xx 不自动重放 POST，避免重复工具副作用。
// D15. libcurl超时映射 RequestTimeout，不暴露错误详情。
// D16. 其他 libcurl 失败映射 TransportFailure。
// D17. POST JSON 正文只在成功媒体类型下聚合。
// D18. 错误状态正文被丢弃，不进入日志或异常。
// D19. JSON 聚合在追加每个分块前执行剩余容量检查。
// D20. 超限正文中止回调并映射 MessageLimitExceeded。
// D21. JSON 解析关闭异常抛出，通过 discarded 明确判断非法输入。
// D22. 非法 JSON 映射 ProtocolViolation。
// D23. 解析成功的 JSON 先识别是否为本请求终局响应。
// D24. 终局响应必须包含匹配 ID 以及 result 或 error。
// D25. 仅收到通知不会错误地完成当前前台 POST。
// D26. 通知仍通过 on_message 交给 Client 的协议状态机处理。
// D27. JSON 正文没有匹配响应时报告 ProtocolViolation。
// D28. POST SSE 使用独立 MCPSseDecoder，不与 Listener 共用状态。
// D29. POST SSE 每个完整 data 事件单独解析 JSON。
// D30. POST SSE 的 id 和 retry 不用于重发原始 POST。
// D31. POST SSE 注释和无 data 控制事件不会产生 JSON 回调。
// D32. POST SSE 非法 NUL 映射 ProtocolViolation。
// D33. POST SSE 事件或 ID 超限映射 MessageLimitExceeded。
// D34. POST SSE 收到通知后断线时，只有存在非空游标才尝试恢复。
// D35. POST SSE 收到匹配终局响应后即建立结果确定性。
// D36. 匹配响应后的流关闭不能把已投递成功改成失败。
// D37. POST SSE 未收到匹配响应时不会重 POST。
// D38. 未恢复的 POST SSE 断线通过 SendFailed 闭合等待者。
// D39. Client 根据是否为已提交工具调用提升 OutcomeUnknown。
// D40. Transport 不自行构造 OutcomeUnknown，避免跨层判断副作用。
// D41. 未知成功 Content-Type 映射 ProtocolViolation。
// D42. 成功响应没有 Content-Type 也不会猜测 JSON。
// D43. SSE EOF 不把未以空行结束的尾部伪造成事件。
// D44. Header 回调失败优先保留协议或大小根因。
// D45. close 触发的回调中止不发送新的 SendFailed。
// D46. 所有 SendFailed 都携带原 dispatch_id。
// D47. SendCompleted 只用于无 JSON-RPC 正文的控制 POST。
// D48. 请求响应通过 on_message 闭合，不额外发送 SendCompleted。
// D49. Worker 顶层 catch 把未知内部异常脱敏为 TransportFailure。
// D50. 顶层 catch 不会重复执行或重排已经提交的 POST。
// D51. 每条 POST 使用独立 HeaderState 和正文缓冲。
// D52. 不同 Worker 的 Header、Body 与取消标志之间没有共享可变状态。
// D53. 会话和协议只在创建 Prepared 快照时读取一次。
// D54. 物理发送不回头读取可能变化的 Core 会话值。
// D55. cpr Session 销毁时释放当前请求的 Header 和正文副本。
// D56. Worker 完成后 Prepared 析构并覆盖剩余秘密字段。
// D57. 迟到响应仍带原 JSON-RPC ID，由 Client 判断是否已经退休。
// D58. Transport 不静默丢弃合法迟到 JSON-RPC 消息。
// D59. Client Faulted 后 close 关闭回调入口，后续网络字节不会穿透。
// D60. POST 结果分类只依赖状态码、媒体类型、稳定 curl 错误和 JSON。

// 实现合同 E：Listener 建流与有限恢复。
// E01. startListener 只允许成功调用一次。
// E02. startListener 本身不调用 Provider，也不执行网络 I/O。
// E03. Listener 线程在 open 后等待明确启动信号。
// E04. close 可以在启动信号前唤醒并终止 Listener 线程。
// E05. 首次 GET 使用 Client 提供的初始化阶段截止时间。
// E06. 首次 GET 建连失败不做隐藏重试。
// E07. 首次 GET 响应头超时不做隐藏重试。
// E08. 首次 GET Provider 失败不做隐藏重试。
// E09. 首次 GET 401/403 进入 ListenerUnavailable。
// E10. 首次 GET 405 明确进入 ListenerUnsupported。
// E11. 首次 GET 带会话的 404 触发 SessionExpired。
// E12. 首次 GET 其他 3xx/4xx 进入 ListenerUnavailable。
// E13. 首次 GET 5xx 进入 ListenerUnavailable，不延迟 connect 失败。
// E14. 200 只有搭配 text/event-stream 才建立 Listener。
// E15. 200 的其他媒体类型立即映射 ProtocolViolation。
// E16. 最终响应头一完成就发布 ListenerStarted。
// E17. ListenerStarted 不等待长期流 EOF，因此 connect 可以进入 Ready。
// E18. HeaderCallback 对已知错误状态主动停止读取无用正文。
// E19. 长期 GET 的响应正文不进入 cpr 的默认聚合字符串。
// E20. 每个网络分块直接交给有界增量 SSE 解码器。
// E21. SSE 注释可作为活动字节刷新空闲计时。
// E22. 空事件不会伪造 JSON-RPC 回调。
// E23. id-only 事件只更新当前 Listener 恢复游标。
// E24. retry-only 事件只更新当前 Listener 重连等待。
// E25. data 事件解析成功后通过 on_message 投递。
// E26. Listener 可在空闲期接收 Server 请求与通知。
// E27. Server 请求的响应由独立 POST Worker 发送。
// E28. Listener 长期占用不会消耗公开前台操作槽。
// E29. Listener 长期占用不会阻塞另一个 Worker 的工具 POST。
// E30. Listener 意外 EOF 被视为监听空档，而非正常完成。
// E31. 已建立 Listener 的网络错误进入 Recovering。
// E32. 已建立 Listener 的响应头或空闲超时进入 Recovering。
// E33. 已建立 Listener 的 5xx 进入有限 Recovering。
// E34. 运行期 Provider 失败直接进入 Unavailable，不自动刷新。
// E35. 运行期 401/403 直接进入 Unavailable，不自动重试凭据。
// E36. 运行期 405 进入 Unsupported，不探测旧 HTTP+SSE。
// E37. 运行期其他 3xx/4xx 进入 Unavailable。
// E38. 运行期错误 Content-Type 进入 Unavailable。
// E39. 运行期会话 404 使整个 Client 会话失效。
// E40. SessionExpired 不消耗剩余 Listener 重连次数。
// E41. 每次恢复 GET 都创建全新的 cpr Session。
// E42. 每次恢复 GET 都重新调用当前凭据 Provider。
// E43. 每次恢复 GET 都有新的 connect_timeout。
// E44. 每次恢复 GET 都有新的响应头 request_timeout。
// E45. Listener 本身没有公开操作绝对寿命上限。
// E46. stream_idle_timeout 从最近 Header 或正文活动重新计时。
// E47. 恢复等待使用 SSE retry，且已在 Decoder 内裁剪。
// E48. Server 未提供 retry 时使用固定一秒默认等待。
// E49. retry 为零允许立即恢复，但仍受有限次数约束。
// E50. close 通过条件变量打断任何 retry 等待。
// E51. 连续失败计数在意外空档后增加一次。
// E52. 每次失败恢复 GET 继续增加同一连续计数。
// E53. 仅建立 200 响应头不会重置连续失败计数。
// E54. 收到至少一个合法完整 SSE 事件才重置计数。
// E55. 重置后再次断线开始新的有限恢复序列。
// E56. max_reconnect_attempts 表示空档后的实际恢复 GET 数量。
// E57. 恢复次数耗尽后发布 ListenerUnavailable 并停止线程。
// E58. Unavailable 不自动周期重试，避免后台无限流量。
// E59. 恢复成功的响应头再次发布 ListenerStarted。
// E60. Client 使用 ListenerStarted 恢复 Listening 只读状态。
// E61. Client 负责在监听空档时使 Catalog 代次过期。
// E62. Transport 不读取 tools.listChanged 能力，保持层次单一。
// E63. Unsupported 与意外空档不同，不由 Transport推断目录 stale。
// E64. Listener 游标只属于 listenerLoop 栈上的状态。
// E65. event.id 缺失时保留之前最后显式游标。
// E66. event.id 为空时明确清除之前游标。
// E67. 非空游标才生成 Last-Event-ID 请求头。
// E68. 游标长度由 MCPSseDecoder 的 max_event_id_bytes 约束。
// E69. Listener retry 与游标不会写入 Core 公共状态。
// E70. 并发 POST SSE 无法观察或覆盖 Listener 游标。
// E71. Listener SSE 非法 JSON 终止当前流并报告 ProtocolViolation。
// E72. Listener SSE 大小超限终止当前流并报告 MessageLimitExceeded。
// E73. 终命协议错误不通过网络重连隐藏。
// E74. 可恢复网络错误只暴露稳定 MCPErrorCode。
// E75. Listener 事件不包含 Endpoint、响应正文或凭据。
// E76. Listener 回调与状态事件均在 Core 锁外执行。
// E77. Listener 回调期间 close 可安全设置当前 Session 取消标志。
// E78. close 后 emit 辅助函数会永久抑制 Listener 新事件。
// E79. Listener 线程退出前设置 done 标志并通知关闭等待者。
// E80. Listener 退出并被 join 后，Core 才允许完成关闭。

// 实现合同 F：关闭、DELETE 与资源上限。
// F01. close 的总预算由 common close_timeout 给出。
// F02. 首个 close 在 Core 锁下把 closing 线性化为真。
// F03. 并发 close 只等待首个关闭者发布 close_complete。
// F04. close 首先清空回调对象，阻止新回调取得函数副本。
// F05. 已经取得副本的回调由 callbacks_inflight 精确计数。
// F06. CallbackGuard 在正常返回和异常路径都递减计数。
// F07. Client 内部回调异常被隔离，不终止 Worker。
// F08. close 设置 queue_stop 并清空所有未发送 Prepared。
// F09. 队列清理发生在任何线程 join 之前，立即释放秘密内存。
// F10. close 设置全部活动取消标志，覆盖 POST 与 Listener。
// F11. cpr Progress/Cancellation 回调协作中止当前 easy perform。
// F12. Worker 在看到 queue_stop 且队列为空后退出。
// F13. 空闲 Worker 通过条件变量立即观察关闭。
// F14. Listener 的启动等待与 retry 等待都由同一条件变量打断。
// F15. 从未 open 的 Transport 不等待不存在的后台线程。
// F16. 部分 open 失败会同步回收已经创建的线程。
// F17. open 失败会永久发布 close_complete，析构不重复等待。
// F18. 存在会话时 close 才考虑远端 DELETE。
// F19. DELETE 在本地取消信号发出之后执行，优先停止业务请求。
// F20. DELETE 使用关闭开始时捕获的协议和会话快照。
// F21. Core 中的会话快照在 DELETE 前已经尽力覆盖。
// F22. DELETE 不为忙碌凭据门等待，避免突破关闭上限。
// F23. DELETE Provider 仍必须在 credential_timeout 内协作返回。
// F24. DELETE connect_timeout 不超过关闭剩余预算。
// F25. DELETE 总 Timeout 不超过关闭剩余预算。
// F26. DELETE 使用独立 cpr Session，不复用正被取消的句柄。
// F27. DELETE 同样显式禁用代理和重定向。
// F28. DELETE 同样发送协议版本与会话 ID。
// F29. DELETE 同样按当前 Provider 获取一次凭据。
// F30. DELETE 响应 Header 和正文通过丢弃回调避免聚合。
// F31. DELETE 任意 2xx 允许继续本地关闭。
// F32. DELETE 404 表示会话已不存在，允许继续本地关闭。
// F33. DELETE 405 表示不支持主动删除，允许继续本地关闭。
// F34. DELETE 其他失败也不能阻止本地资源回收。
// F35. DELETE 异常不会产生 Client 回调或覆盖原错误。
// F36. close 等待 Worker、Listener 与在途回调到统一截止。
// F37. 截止前退出的线程使用 join 完整回收线程对象。
// F38. 全部网络尝试都安装协作取消和不超过关闭预算的建连上限。
// F39. Provider 也必须按公开合同响应取消，不允许永久阻塞关闭。
// F40. close 会 join 每个固定 Worker，禁止把残留任务留到返回之后。
// F41. Worker join 前会析构 Prepared 并尽力覆盖秘密。
// F42. Listener join 前会释放 Decoder、游标与当前 cpr Session。
// F43. 外层线程对象全部不再 joinable 后才允许 Transport 析构。
// F44. close_complete 只在所有线程 join 完成后发布。
// F45. 第二次 close 不重新设置取消或重新调用 Provider。
// F46. close 返回后状态读取不会触发任何网络或用户代码。
// F47. max_pending_messages 限制排队 POST 数量。
// F48. max_message_bytes 限制 JSON 请求和响应正文。
// F49. max_sse_event_bytes 限制每条 SSE 事件累计内存。
// F50. max_event_id_bytes 限制全部 SSE 恢复 Header 的来源大小。
// F51. max_session_id_bytes 限制所有后续请求重复携带的秘密。
// F52. max_reconnect_attempts 限制意外空档后的网络尝试数量。
// F53. max_sse_retry_delay 限制 Server 控制的后台等待时长。
// F54. 固定 Worker 数限制并发 cpr Session 的 POST 数量。
// F55. 唯一 Listener 线程限制长期 GET Session 数量。
// F56. 每个请求的局部缓冲在函数返回时确定释放。
// F57. HeaderState 不保存未知 Header 或错误响应正文。
// F58. 活动取消集合定期清除过期弱引用，避免持续增长。
// F59. 时间比较全部使用 steady_clock，不受系统时钟调整影响。
// F60. 所有关闭异常都被 noexcept 边界吸收并完成本地终态。

// 实现合同 G：可观测性与验证预言机。
// G01. HTTP 状态码保留在事件中，测试无需解析动态错误文本。
// G02. MCPErrorCode 是调用方判断错误类别的唯一稳定入口。
// G03. AuthenticationRequired 可由 401 与 403 的回环夹具稳定触发。
// G04. SessionExpired 只能由已有会话的后续 404 稳定触发。
// G05. ListenerUnsupported 只能由 GET 405 稳定触发。
// G06. ProtocolViolation 可由错误媒体类型、非法 JSON 或非法会话触发。
// G07. MessageLimitExceeded 可由正文、SSE 事件或事件 ID 上限触发。
// G08. RequestTimeout 可由 POST 总截止、GET 建流或空闲截止触发。
// G09. TransportFailure 可由建连失败和非超时 libcurl 错误触发。
// G10. 每个 POST Session 独立，回环 Server 可按连接验证并发隔离。
// G11. Listener Session 独立，测试可同时暂停 GET 并完成工具 POST。
// G12. 固定 Worker 数使并发资源上界可以通过活动计数器验证。
// G13. 原 POST 不重放可由 Server 端请求计数精确断言为一次。
// G14. Last-Event-ID 可由断流前事件 ID 和恢复请求 Header 验证。
// G15. 空事件 ID 可由恢复请求缺少 Last-Event-ID 验证。
// G16. retry 裁剪可由可控断流间隔和本地最大值验证。
// G17. 代理禁用可由环境代理陷阱零连接验证。
// G18. 禁重定向可由 3xx 目标 Server 零连接验证。
// G19. TLS 主机校验可由正确主机与错误主机证书对照验证。
// G20. 会话 Header 可由初始化响应和后续 POST/GET/DELETE 记录验证。
// G21. 协议 Header 可由初始化前后请求记录对照验证。
// G22. Provider 逐请求调用可由递增凭据夹具验证。
// G23. Provider 串行门可由并发进入计数最大值为一验证。
// G24. prepare 零网络写入可由阻塞 Provider 与 Server 连接计数验证。
// G25. commit 有界入队可由暂停 Worker 和队列溢出验证。
// G26. close 无后续回调可由返回后回调计数保持不变验证。
// G27. DELETE 尽力语义可分别用 2xx、404、405 和断连验证。
// G28. 资源收敛可通过重复连接关闭和活动 Session 计数验证。
// G29. 错误脱敏可用唯一 Token、Session 与游标哨兵全文检索验证。
// G30. UTF-8 中文错误与注释不改变代码标识符和协议字段英文拼写。

// 实现合同 H：前台 POST SSE 响应恢复。
// H01. 前台恢复只在原 POST 已提交且仍未收到匹配终局响应时考虑。
// H02. 原 POST 必须先返回成功 2xx 与 text/event-stream 媒体类型。
// H03. 原 POST 流必须提供非空、未清除的事件 ID 才能恢复。
// H04. 没有事件 ID 时禁止伪造续传位置，直接按断线根因失败。
// H05. Server 显式发送空 ID 时清除旧游标并停止后续恢复。
// H06. 恢复只发送 GET，任何路径都不会重新发送原 POST 正文。
// H07. 原工具 POST 次数因此始终保持一次，避免重复副作用。
// H08. 恢复 GET 使用原固定 Endpoint，Server 不能提供替代 URL。
// H09. 恢复 GET 使用该 POST Worker 栈上的独立游标。
// H10. 恢复 GET 不读取后台 Listener 的游标或 retry。
// H11. 后台 Listener 也不读取前台恢复流的任何状态。
// H12. 两类 GET 可以使用不同 cpr Session 同时存在。
// H13. 恢复 GET 使用 RecoveryGet 凭据用途重新调用 Provider。
// H14. 原 POST 的旧凭据快照不会复用于恢复 GET。
// H15. 每次恢复尝试都获得独立凭据值和独立取消标志。
// H16. Provider 失败按 CredentialUnavailable 闭合父请求。
// H17. Provider 更早观察关闭时不再产生父请求回调。
// H18. 恢复 GET 自动携带当前协议版本和当前会话 ID。
// H19. 初始化 POST 恢复会读取刚从响应头捕获的会话 ID。
// H20. 初始化协议尚未协商完成时不会伪造版本 Header。
// H21. Ready 下恢复 GET 使用已经固定的 2025-11-25 版本。
// H22. Last-Event-ID 只取当前前台流最后显式非空 ID。
// H23. 恢复响应的新 ID 会替代旧 ID 用于下一次尝试。
// H24. 恢复响应不含 ID 时继续保留先前安全游标。
// H25. 恢复响应的 retry 会替代当前流的下一次等待。
// H26. retry 始终裁剪到 max_sse_retry_delay。
// H27. 原流未给 retry 时使用一秒默认等待。
// H28. retry 等待计入父请求截止时间，不延长公开操作。
// H29. 等待通过 Core 条件变量执行，close 可立即打断。
// H30. 每个恢复 GET 的 connect timeout 都取当前剩余预算。
// H31. 每个恢复 GET 的总 Timeout 都不超过父请求剩余时间。
// H32. 恢复流空闲仍由 stream_idle_timeout 独立检测。
// H33. 父请求截止先到时根因固定为 RequestTimeout。
// H34. 空闲中断但父截止尚有余量时可以继续有限恢复。
// H35. 建连与接收失败使用稳定 libcurl 错误映射。
// H36. HTTP 5xx 属于可恢复尝试并消耗连续次数。
// H37. HTTP 401/403 终止恢复并返回 AuthenticationRequired。
// H38. 已有会话的 HTTP 404 终止整个 Client 会话。
// H39. 其他 3xx/4xx 终止恢复并返回 HttpStatusError。
// H40. 恢复 GET 不跟随重定向，也不探测旧传输。
// H41. 200 但非 text/event-stream 属于 ProtocolViolation。
// H42. 其他 2xx 媒体类型同样不能被宽松接受。
// H43. 非法 SSE framing 终止恢复而不是隐藏协议故障。
// H44. 非法 JSON 终止恢复并通过 Client 协议边界处理。
// H45. SSE 事件或消息超限终止恢复并保留大小根因。
// H46. 恢复流可以在终局响应前投递任意合法通知。
// H47. 恢复流可以投递 Server 请求，由另一个 Worker 响应。
// H48. 通知和 Server 请求不会误判为父请求终局响应。
// H49. 只有匹配父 JSON-RPC ID 的 result 或 error 才完成父请求。
// H50. 匹配终局响应到达后主动中止当前 GET，及时释放 Worker。
// H51. 主动中止产生的 libcurl 写回调错误不会覆盖已确定结果。
// H52. 完成响应通过 on_message 交给 MCPClient 做严格协议解析。
// H53. Transport 不解析 ToolResult，也不判断工具业务错误。
// H54. 恢复失败通过原 dispatch_id 精确闭合父等待者。
// H55. 已提交工具调用由 Client 把根因提升为 OutcomeUnknown。
// H56. 非工具公开操作保留原 RequestTimeout 或传输根因。
// H57. 每次失败恢复 GET 增加当前连续尝试计数。
// H58. 收到至少一个合法完整事件后重置连续尝试计数。
// H59. 单纯收到 200 响应头不足以重置连续计数。
// H60. max_reconnect_attempts 限制每段连续恢复尝试数量。
// H61. 即使每段成功事件重置计数，父截止仍限制总寿命。
// H62. 恢复循环不创建新线程，继续占用原固定 POST Worker。
// H63. 第二 POST Worker 仍可发送协议必需控制响应。
// H64. 恢复循环不占 MCPClient 的第二个公开操作槽。
// H65. close 取消当前恢复 Session 并打断 retry 等待。
// H66. close 之后恢复循环不能再取得 Client 回调副本。
// H67. 恢复局部 Decoder 在每个物理 GET 结束时销毁。
// H68. 恢复局部凭据在每个物理 GET 结束时尽力覆盖。
// H69. 恢复游标不写入 Core，因此其他请求无法读取。
// H70. 恢复结束后 Worker 立即返回有界出站队列消费循环。

// 实现合同 I：仅测试 TLS 覆盖。
// I01. 测试覆盖声明只位于 src/mcp/detail 内部头。
// I02. 公开 include/mcp 配置不出现 CA、resolve 或测试开关。
// I03. 生产工厂总是构造空测试覆盖对象。
// I04. 测试工厂和生产工厂使用同一具体 Transport 类型。
// I05. 两个工厂共享完全相同的 prepare、commit 与 Worker 路径。
// I06. 测试工厂构造仍不执行文件、DNS 或网络 I/O。
// I07. PEM CA 以字符串按值固定在单个测试 Transport 内。
// I08. 空 PEM 表示不覆盖系统信任库。
// I09. 非空 PEM 只通过当前 easy handle 的 CAINFO_BLOB 设置。
// I10. CAINFO_BLOB 使用 CURL_BLOB_COPY，局部 curl_blob 可安全离开作用域。
// I11. 测试覆盖不能把 VerifyPeer 改为 false。
// I12. 测试覆盖不能把 VerifyHost 改为 false。
// I13. URL 主机名仍是证书身份校验的唯一目标名称。
// I14. resolve 只改变连接地址，不改变 URL、Host 或 SNI。
// I15. 正确主机证书和错误主机证书因此能形成有效对照。
// I16. 每个 resolve 条目使用 libcurl host:port:address 语法。
// I17. 每个物理 Session 都构造自己的 curl_slist。
// I18. 不同并发 Session 从不共享可变 curl_slist。
// I19. CURLOPT_RESOLVE 不复制列表，因此列表覆盖整个 perform。
// I20. perform 返回后先解除 easy handle 引用，再释放 slist。
// I21. slist 分配失败映射稳定的 libcurl 内存错误。
// I22. setopt 失败在网络执行前闭合当前请求。
// I23. 测试 CA 大小受 common message 上限约束。
// I24. resolve 条目数量受 pending message 上限约束。
// I25. 单个 resolve 条目受错误文本大小上限约束。
// I26. resolve 条目拒绝空值、NUL、CR 与 LF。
// I27. 测试覆盖不能添加请求 Header 或修改凭据值。
// I28. 测试覆盖不能修改固定 Endpoint 或 HTTP Method。
// I29. 测试覆盖不能启用环境代理或代理认证。
// I30. 每个测试 Session 仍显式设置 CURLOPT_PROXY 为空字符串。
// I31. 每个测试 Session 仍显式禁用 FOLLOWLOCATION。
// I32. 每个测试 Session 仍显式设置 TLS peer 校验为一。
// I33. 每个测试 Session 仍显式设置 TLS host 校验为二。
// I34. POST、Listener、恢复 GET 与 DELETE 都经过同一覆盖应用函数。
// I35. 生产空覆盖不会调用 CAINFO_BLOB 或 CURLOPT_RESOLVE。
// I36. 测试覆盖数据不会进入异常、事件、Trace 或请求正文。
// I37. 测试覆盖随 Core 关闭释放，不进入进程级全局 curl 状态。
// I38. 多个测试 Client 可以使用不同 CA 和解析表而互不污染。
// I39. 测试工厂不导出到安装接口，也不供生产示例引用。
// I40. 回环测试负责提供正确主机和错误主机的稳定证书预言机。

// secureClear 尽力覆盖本实现持有的秘密副本。
// 标准库仍可能在分配器内部保留旧页，因此公开面绝不提供秘密读取入口。
void secureClear(std::string& value) noexcept {
    std::fill(value.begin(), value.end(), '\0');
    value.clear();
}

// lowerAscii 仅用于 HTTP 令牌；HTTP 字段名和媒体类型均限定为 ASCII。
std::string lowerAscii(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for(const unsigned char character : value) {
        if(character >= 'A' && character <= 'Z') {
            lowered.push_back(static_cast<char>(character - 'A' + 'a'));
        } else {
            lowered.push_back(static_cast<char>(character));
        }
    }
    return lowered;
}

// trimHttpWhitespace 只移除 HTTP 允许的空格和水平制表，不吞掉其他控制字节。
std::string_view trimHttpWhitespace(std::string_view value) noexcept {
    while(!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1U);
    }
    while(!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1U);
    }
    return value;
}

// normalizeContentType 忽略合法媒体类型参数，但不会接受前缀匹配。
std::string normalizeContentType(std::string_view value) {
    const std::size_t parameter = value.find(';');
    if(parameter != std::string_view::npos) {
        value = value.substr(0U, parameter);
    }
    return lowerAscii(trimHttpWhitespace(value));
}

// durationUntil 把单调截止时间转换为 libcurl 可表达的正毫秒值。
// 已经过期时返回 1ms，调用方仍会在发起网络 I/O 前单独拒绝。
std::chrono::milliseconds durationUntil(Clock::time_point deadline) noexcept {
    const auto now = Clock::now();
    if(now >= deadline) {
        return std::chrono::milliseconds{1};
    }
    return std::max(std::chrono::milliseconds{1},
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
}

// HeaderState 只保存协议决策所需的有界响应头。
// 自定义 HeaderCallback 避免 cpr 把完整响应头无界累积到字符串。
struct HeaderState {
    int status = 0;
    std::string content_type;
    std::optional<std::string> session_id;
    bool duplicate_session_id = false;
    bool complete = false;
    Clock::time_point last_activity = Clock::now();

    // resetForStatusBlock 支持可能先出现的 1xx 响应头块。
    void resetForStatusBlock(int parsed_status) {
        status = parsed_status;
        content_type.clear();
        session_id.reset();
        duplicate_session_id = false;
        complete = false;
        last_activity = Clock::now();
    }

    // consume 按行处理状态、Content-Type 与 MCP-Session-Id。
    // 其他响应头不保存，避免远端通过无关字段占用长期内存。
    bool consume(std::string_view raw_line, std::size_t max_session_id_bytes) {
        last_activity = Clock::now();
        while(!raw_line.empty() && (raw_line.back() == '\r' || raw_line.back() == '\n')) {
            raw_line.remove_suffix(1U);
        }

        if(raw_line.rfind("HTTP/", 0U) == 0U) {
            const std::size_t first_space = raw_line.find(' ');
            if(first_space == std::string_view::npos) {
                return false;
            }
            std::string_view code = trimHttpWhitespace(raw_line.substr(first_space + 1U));
            const std::size_t next_space = code.find(' ');
            if(next_space != std::string_view::npos) {
                code = code.substr(0U, next_space);
            }
            if(code.size() != 3U ||
               !std::all_of(code.begin(), code.end(), [](char value) { return value >= '0' && value <= '9'; })) {
                return false;
            }
            resetForStatusBlock((code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0'));
            return true;
        }

        if(raw_line.empty()) {
            if(status >= 200) {
                complete = true;
            }
            return true;
        }

        const std::size_t separator = raw_line.find(':');
        if(separator == std::string_view::npos) {
            return true;
        }
        const std::string name = lowerAscii(trimHttpWhitespace(raw_line.substr(0U, separator)));
        const std::string_view value = trimHttpWhitespace(raw_line.substr(separator + 1U));
        if(name == "content-type") {
            content_type = normalizeContentType(value);
        } else if(name == "mcp-session-id") {
            if(session_id.has_value()) {
                duplicate_session_id = true;
                return false;
            }
            if(value.size() > max_session_id_bytes) {
                return false;
            }
            session_id = std::string{value};
        }
        return true;
    }
};

// PreparedHttpMessage 是只可由创建它的 Core 消费的一次性秘密快照。
// 析构时覆盖正文、会话和鉴权值，避免队列取消后继续保留。
class PreparedHttpMessage final : public IMCPPreparedMessage {
   public:
    ~PreparedHttpMessage() override {
        secureClear(body);
        secureClear(session_id);
        secureClear(credential_value);
    }

    const void* owner = nullptr;
    MCPTransportRequestContext context;
    std::string body;
    std::string protocol_version;
    std::string session_id;
    std::uint64_t session_generation = 0U;
    std::string credential_header;
    std::string credential_value;
    std::optional<nlohmann::json> expected_response_id;
};

// ActiveRequest 允许 close 在不知道 cpr 类型的公共边界内取消所有请求。
struct ActiveRequest {
    std::shared_ptr<std::atomic_bool> cancelled;
};

// TransportCore 统一承载固定线程共享状态。
// 所有后台线程只捕获 shared_ptr<TransportCore>，关闭会 join 后再释放外层对象。
struct TransportCore {
    TransportCore(MCPStreamableHttpConfig http_config, MCPCommonLimits common_limits,
                  MCPHttpTestOverrides http_test_overrides)
        : config(std::move(http_config)),
          limits(std::move(common_limits)),
          test_overrides(std::move(http_test_overrides)) {}

    MCPStreamableHttpConfig config;
    MCPCommonLimits limits;
    // 测试覆盖按 Transport 固定，只在 performPrepared 设置当前 easy handle 时读取。
    MCPHttpTestOverrides test_overrides;

    std::mutex mutex;
    std::condition_variable cv;
    MCPTransportCallbacks callbacks;
    bool opened = false;
    bool closing = false;
    // 原子镜像只供凭据门和 Provider 的无锁取消视图读取；状态真值仍由 mutex 保护。
    std::atomic_bool close_requested{false};
    bool close_complete = false;
    bool queue_stop = false;
    std::deque<std::unique_ptr<PreparedHttpMessage>> queue;
    std::vector<std::weak_ptr<std::atomic_bool>> active_cancellations;
    std::size_t callbacks_inflight = 0U;

    std::string protocol_version;
    std::string session_id;
    // generation 随新会话建立递增；expired 是当前会话唯一终命线性化标志。
    std::uint64_t session_generation = 0U;
    bool session_expired = false;
    std::atomic_bool session_expired_requested{false};

    bool listener_requested = false;
    bool listener_thread_done = false;
    Clock::time_point listener_initial_deadline{};
    std::array<bool, kPostWorkerCount> worker_done{};

    // Provider 门独立于状态锁；busy 只表示一个 Provider 调用拥有串行权，不要求调用期间持锁。
    std::mutex credential_gate_mutex;
    std::condition_variable credential_gate_cv;
    bool credential_gate_busy = false;
};

// CallbackGuard 让 close 能等待已经取走函数副本的回调完成。
class CallbackGuard {
   public:
    explicit CallbackGuard(std::shared_ptr<TransportCore> core) : core_(std::move(core)) {}

    ~CallbackGuard() {
        std::lock_guard<std::mutex> lock(core_->mutex);
        if(core_->callbacks_inflight > 0U) {
            --core_->callbacks_inflight;
        }
        core_->cv.notify_all();
    }

   private:
    std::shared_ptr<TransportCore> core_;
};

enum class TestActivityKind { Request, SseWorker, Session };

// TestActivityGuard 只在内部测试工厂提供计数器时生效。
// RAII 路径覆盖正常响应、取消、异常和提前返回，生产工厂没有额外共享状态。
class TestActivityGuard {
   public:
    TestActivityGuard(const std::shared_ptr<MCPHttpTestActivityCounters>& counters, TestActivityKind kind)
        : counters_(counters) {
        if(!counters_) {
            return;
        }
        switch(kind) {
            case TestActivityKind::Request:
                counter_ = &counters_->active_requests;
                break;
            case TestActivityKind::SseWorker:
                counter_ = &counters_->active_sse_workers;
                break;
            case TestActivityKind::Session:
                counter_ = &counters_->active_sessions;
                break;
        }
        counter_->fetch_add(1U);
    }

    ~TestActivityGuard() {
        if(counter_ != nullptr) {
            counter_->fetch_sub(1U);
        }
    }

    TestActivityGuard(const TestActivityGuard&) = delete;
    TestActivityGuard& operator=(const TestActivityGuard&) = delete;

   private:
    std::shared_ptr<MCPHttpTestActivityCounters> counters_;
    std::atomic<std::size_t>* counter_ = nullptr;
};

// emitEvent 和 emitMessage 都先登记在途回调，再于状态锁外调用 Client。
void emitEvent(const std::shared_ptr<TransportCore>& core, MCPTransportEvent event) noexcept {
    std::function<void(MCPTransportEvent)> callback;
    {
        std::lock_guard<std::mutex> lock(core->mutex);
        if(core->closing || core->session_expired || !core->callbacks.on_event) {
            return;
        }
        callback = core->callbacks.on_event;
        ++core->callbacks_inflight;
    }
    CallbackGuard guard(core);
    try {
        callback(std::move(event));
    } catch(...) {
        // Client 回调属于 Transport 内部边界，异常不得终止网络 Worker。
    }
}

void emitMessage(const std::shared_ptr<TransportCore>& core, nlohmann::json message) noexcept {
    std::function<void(nlohmann::json)> callback;
    {
        std::lock_guard<std::mutex> lock(core->mutex);
        if(core->closing || core->session_expired || !core->callbacks.on_message) {
            return;
        }
        callback = core->callbacks.on_message;
        ++core->callbacks_inflight;
    }
    CallbackGuard guard(core);
    try {
        callback(std::move(message));
    } catch(...) {
        // 协议回调失败由 Client 自身状态机处理，Transport 不泄漏异常。
    }
}

// linearizeSessionExpired 在 Core 状态锁下选择同一会话的唯一终命胜者。
// 回调副本与 callbacks_inflight 在同一临界区登记，close 若随后到达会等待该回调完成。
bool linearizeSessionExpired(const std::shared_ptr<TransportCore>& core, const std::string& observed_session_id,
                             std::uint64_t dispatch_id, int http_status) noexcept {
    std::function<void(MCPTransportEvent)> callback;
    {
        std::lock_guard<std::mutex> lock(core->mutex);
        // close 先取得状态锁、会话已经由其他 404 终止，或响应属于旧会话时均不得再次发事件。
        if(core->closing || core->session_expired || observed_session_id.empty() ||
           core->session_id != observed_session_id) {
            return false;
        }

        core->session_expired = true;
        core->session_expired_requested.store(true);
        core->queue_stop = true;
        secureClear(core->session_id);
        core->queue.clear();
        for(auto& weak_cancelled : core->active_cancellations) {
            if(auto cancelled = weak_cancelled.lock()) {
                cancelled->store(true);
            }
        }

        // std::function 复制可能分配；失败时会话仍保持终命，但绝不在 noexcept 边界终止宿主。
        try {
            callback = core->callbacks.on_event;
            if(callback) {
                ++core->callbacks_inflight;
            }
        } catch(...) {
            callback = {};
        }
    }

    core->cv.notify_all();
    core->credential_gate_cv.notify_all();
    if(callback) {
        CallbackGuard guard(core);
        try {
            callback({MCPTransportEventType::SessionExpired, dispatch_id, MCPErrorCode::SessionExpired, http_status,
                      MCPListenerState::Stopped});
        } catch(...) {
            // Client 回调异常不能破坏已经完成的会话终命线性化。
        }
    }
    return true;
}

// registerCancellation 返回独立取消标志，并把弱引用登记到关闭集合。
std::shared_ptr<std::atomic_bool> registerCancellation(const std::shared_ptr<TransportCore>& core) {
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    std::lock_guard<std::mutex> lock(core->mutex);
    core->active_cancellations.erase(
        std::remove_if(core->active_cancellations.begin(), core->active_cancellations.end(),
                       [](const auto& entry) { return entry.expired(); }),
        core->active_cancellations.end());
    core->active_cancellations.push_back(cancelled);
    if(core->closing || core->session_expired) {
        cancelled->store(true);
    }
    return cancelled;
}

// validateVisibleSessionId 同时执行长度、空值和可见 ASCII 校验。
bool validateVisibleSessionId(const std::string& value, std::size_t max_bytes) noexcept {
    if(value.empty() || value.size() > max_bytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char character) { return character >= 0x21U && character <= 0x7EU; });
}

// stateForKind 仅用于 Transport 主动产生的准备阶段 MCPException 快照。
// Client 在捕获后仍以自身状态机决定最终公开操作状态。
MCPClientState stateForKind(MCPTransportRequestKind kind) noexcept {
    switch(kind) {
        case MCPTransportRequestKind::Initialize:
        case MCPTransportRequestKind::InitializedNotification:
        case MCPTransportRequestKind::ListenerGet:
            return MCPClientState::Initializing;
        default:
            return MCPClientState::Ready;
    }
}

// operationDeadline 兼容直接测试 Transport 的旧式三字段上下文。
// MCPClient 始终显式传入绝对上限；独立 Transport 测试未提供时回退到 deadline，仍保持有界。
Clock::time_point operationDeadline(const MCPTransportRequestContext& context) noexcept {
    return context.operation_deadline == Clock::time_point::max() ? context.deadline : context.operation_deadline;
}

// credentialKind 把协议用途压缩为 Provider 可见的脱敏类别。
MCPHttpRequestKind credentialKind(MCPTransportRequestKind kind) noexcept {
    switch(kind) {
        case MCPTransportRequestKind::ServerResponse:
            return MCPHttpRequestKind::ServerResponsePost;
        case MCPTransportRequestKind::CancellationNotification:
            return MCPHttpRequestKind::CancellationPost;
        case MCPTransportRequestKind::ListenerGet:
            return MCPHttpRequestKind::ListenerGet;
        case MCPTransportRequestKind::RecoveryGet:
            return MCPHttpRequestKind::RecoveryGet;
        case MCPTransportRequestKind::DeleteSession:
            return MCPHttpRequestKind::DeleteSession;
        default:
            return MCPHttpRequestKind::ForegroundPost;
    }
}

struct CredentialSnapshot {
    std::string header_name;
    std::string value;

    ~CredentialSnapshot() {
        secureClear(value);
    }
};

enum class CredentialAbortReason { None, Closing, SessionExpired, RequestCancelled };

// credentialAbortReason 只读取原子取消视图，Provider 可安全调用且不会取得 Transport 状态锁。
CredentialAbortReason credentialAbortReason(const std::shared_ptr<TransportCore>& core,
                                            const std::shared_ptr<std::atomic_bool>& request_cancelled,
                                            bool allow_closing) noexcept {
    if(!allow_closing && core->close_requested.load()) {
        return CredentialAbortReason::Closing;
    }
    if(core->session_expired_requested.load()) {
        return CredentialAbortReason::SessionExpired;
    }
    if(request_cancelled && request_cancelled->load()) {
        return CredentialAbortReason::RequestCancelled;
    }
    return CredentialAbortReason::None;
}

[[noreturn]] void throwCredentialAbort(CredentialAbortReason reason, MCPTransportRequestKind kind) {
    if(reason == CredentialAbortReason::SessionExpired) {
        throw MCPException(MCPErrorCode::SessionExpired, stateForKind(kind), "MCP HTTP 会话已失效，凭据准备已取消");
    }
    throw MCPException(MCPErrorCode::OperationCancelled, MCPClientState::Closing, "MCP HTTP 凭据准备已被关闭取消");
}

// CredentialGateLease 只持有 busy 所有权，不在 Provider 调用期间持有任何互斥量。
// 释放时广播而不是只唤醒一个等待者，使截止或终命条件已经满足的排队者同时收敛。
class CredentialGateLease final {
   public:
    explicit CredentialGateLease(TransportCore* core = nullptr) noexcept : core_(core) {}

    ~CredentialGateLease() {
        if(core_ == nullptr) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(core_->credential_gate_mutex);
            core_->credential_gate_busy = false;
        }
        core_->credential_gate_cv.notify_all();
    }

    CredentialGateLease(const CredentialGateLease&) = delete;
    CredentialGateLease& operator=(const CredentialGateLease&) = delete;

    void activate(TransportCore* core) noexcept {
        core_ = core;
    }

   private:
    TransportCore* core_ = nullptr;
};

// acquireCredential 每次调用 Provider，且在单个 Client 内串行。
// 非协作且永久不返回的 Provider 违反公开接口合同，C++ 无法安全强杀用户代码。
CredentialSnapshot acquireCredential(const std::shared_ptr<TransportCore>& core, MCPTransportRequestKind kind,
                                     Clock::time_point operation_deadline,
                                     const std::shared_ptr<std::atomic_bool>& request_cancelled,
                                     bool allow_closing = false, bool gate_already_owned = false) {
    // DELETE 预占门后进入本函数，因此 Lease 必须先于任何可能抛出的截止或取消检查建立。
    CredentialGateLease gate_lease(gate_already_owned ? core.get() : nullptr);
    // Provider 子预算与公开操作截止取最小值；饱和加法避免极大合法配置回绕。
    const auto credential_deadline =
        std::min(operation_deadline, saturatingSteadyDeadlineAfter(core->config.credential_timeout));
    auto cancellation_requested = [core, request_cancelled, allow_closing] {
        return credentialAbortReason(core, request_cancelled, allow_closing) != CredentialAbortReason::None;
    };

    CredentialAbortReason abort_reason = credentialAbortReason(core, request_cancelled, allow_closing);
    if(abort_reason != CredentialAbortReason::None) {
        throwCredentialAbort(abort_reason, kind);
    }
    if(Clock::now() >= operation_deadline) {
        throw MCPException(MCPErrorCode::RequestTimeout, stateForKind(kind), "MCP HTTP 请求在凭据准备前已超时");
    }

    CredentialSnapshot snapshot;
    if(std::holds_alternative<MCPAnonymousCredential>(core->config.credential)) {
        return snapshot;
    }

    if(!gate_already_owned) {
        std::unique_lock<std::mutex> gate_lock(core->credential_gate_mutex);
        const bool gate_ready = core->credential_gate_cv.wait_until(
            gate_lock, credential_deadline, [&] { return !core->credential_gate_busy || cancellation_requested(); });
        abort_reason = credentialAbortReason(core, request_cancelled, allow_closing);
        if(abort_reason != CredentialAbortReason::None) {
            throwCredentialAbort(abort_reason, kind);
        }
        if(!gate_ready || core->credential_gate_busy) {
            if(Clock::now() >= operation_deadline) {
                throw MCPException(MCPErrorCode::RequestTimeout, stateForKind(kind),
                                   "MCP HTTP 请求在凭据门排队期间超时");
            }
            throw MCPException(MCPErrorCode::CredentialUnavailable, stateForKind(kind), "MCP HTTP 凭据门排队超时");
        }
        core->credential_gate_busy = true;
        gate_lease.activate(core.get());
    }

    std::shared_ptr<IMCPHttpCredentialProvider> provider;
    bool bearer_mode = false;
    if(const auto* bearer = std::get_if<MCPBearerCredential>(&core->config.credential)) {
        snapshot.header_name = "Authorization";
        provider = bearer->provider;
        bearer_mode = true;
    } else {
        const auto& fixed = std::get<MCPFixedHeaderCredential>(core->config.credential);
        snapshot.header_name = fixed.header_name;
        provider = fixed.provider;
    }

    try {
        MCPHttpCredentialRequestContext context{credentialKind(kind), credential_deadline, cancellation_requested};
        snapshot.value = provider->credentialValue(context);
    } catch(const MCPException&) {
        throw;
    } catch(...) {
        abort_reason = credentialAbortReason(core, request_cancelled, allow_closing);
        if(abort_reason != CredentialAbortReason::None) {
            throwCredentialAbort(abort_reason, kind);
        }
        throw MCPException(MCPErrorCode::CredentialUnavailable, stateForKind(kind), "MCP HTTP 凭据 Provider 调用失败");
    }

    abort_reason = credentialAbortReason(core, request_cancelled, allow_closing);
    if(abort_reason != CredentialAbortReason::None) {
        throwCredentialAbort(abort_reason, kind);
    }
    const auto completed_at = Clock::now();
    if(completed_at >= operation_deadline) {
        throw MCPException(MCPErrorCode::RequestTimeout, stateForKind(kind), "MCP HTTP 请求在凭据准备期间超时");
    }
    if(completed_at >= credential_deadline || snapshot.value.empty() ||
       snapshot.value.find_first_of("\r\n") != std::string::npos) {
        throw MCPException(MCPErrorCode::CredentialUnavailable, stateForKind(kind), "MCP HTTP 凭据不可用或格式非法");
    }

    if(bearer_mode) {
        snapshot.value.insert(0U, "Bearer ");
    }
    return snapshot;
}

// configureSession 设置全部生产网络安全选项。
// CURLOPT_PROXY 在 Prepare* 后还会再次写入，确保覆盖环境变量默认值。
void configureSession(cpr::Session& session, const std::shared_ptr<TransportCore>& core, const cpr::Header& headers,
                      const std::shared_ptr<std::atomic_bool>& cancelled, std::chrono::milliseconds connect_timeout) {
    session.SetUrl(cpr::Url{core->config.endpoint});
    session.SetHeader(headers);
    // 调用方已把建连预算限制在当前操作剩余时间内；普通请求不应被无关的 close_timeout 提前截断。
    // close 通过 CancellationParam 取消活动请求，并由独立的共享关闭截止统一约束收敛。
    session.SetConnectTimeout(cpr::ConnectTimeout{connect_timeout});
    session.SetRedirect(cpr::Redirect{false});
    session.SetSslOptions(cpr::Ssl(cpr::ssl::VerifyHost{true}, cpr::ssl::VerifyPeer{true}));
    session.SetCancellationParam(cancelled);
}

enum class HttpMethod { Post, Get, Delete };

// performPrepared 先让 cpr 配置请求，再直接覆盖安全关键 libcurl 选项。
// 这样既复用 cpr 的响应封装，又能证明环境代理不会改变生产路由。
cpr::Response performPrepared(cpr::Session& session, HttpMethod method, const std::shared_ptr<TransportCore>& core) {
    // 一个物理请求独占一个 cpr Session；两个计数在 Complete 返回后同时归零。
    TestActivityGuard session_guard(core->test_overrides.activity_counters, TestActivityKind::Session);
    TestActivityGuard request_guard(core->test_overrides.activity_counters, TestActivityKind::Request);
    switch(method) {
        case HttpMethod::Post:
            session.PreparePost();
            break;
        case HttpMethod::Get:
            session.PrepareGet();
            break;
        case HttpMethod::Delete:
            session.PrepareDelete();
            break;
    }

    const auto holder = session.GetCurlHolder();
    CURL* handle = holder->handle;
    // 安全关键 setopt 不能假定成功；任一失败都在发出请求前按 libcurl 错误完成。
    CURLcode option_result = curl_easy_setopt(handle, CURLOPT_PROXY, "");
    if(option_result == CURLE_OK) {
        option_result = curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    }
    if(option_result == CURLE_OK) {
        option_result = curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    }
    if(option_result == CURLE_OK) {
        option_result = curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
    }
    if(option_result == CURLE_OK) {
        option_result = curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    }
    if(option_result != CURLE_OK) {
        return session.Complete(option_result);
    }

    // 测试 CA 使用 COPY，libcurl 在 setopt 返回前取得自己的 PEM 副本。
    // 生产工厂的 ca_pem 为空，因此不会改写系统信任库行为。
    if(!core->test_overrides.ca_pem.empty()) {
        curl_blob ca_blob{};
        ca_blob.data = const_cast<char*>(core->test_overrides.ca_pem.data());
        ca_blob.len = core->test_overrides.ca_pem.size();
        ca_blob.flags = CURL_BLOB_COPY;
        const CURLcode ca_result = curl_easy_setopt(handle, CURLOPT_CAINFO_BLOB, &ca_blob);
        if(ca_result != CURLE_OK) {
            return session.Complete(ca_result);
        }
    }

    // CURLOPT_RESOLVE 不复制 slist；列表必须覆盖完整传输寿命，并在 easy handle 解除引用后释放。
    curl_slist* resolve_list = nullptr;
    for(const auto& entry : core->test_overrides.resolve_entries) {
        curl_slist* appended = curl_slist_append(resolve_list, entry.c_str());
        if(appended == nullptr) {
            curl_slist_free_all(resolve_list);
            return session.Complete(CURLE_OUT_OF_MEMORY);
        }
        resolve_list = appended;
    }
    if(resolve_list != nullptr) {
        const CURLcode resolve_result = curl_easy_setopt(handle, CURLOPT_RESOLVE, resolve_list);
        if(resolve_result != CURLE_OK) {
            // 即使 setopt 失败也主动清除选项，避免未来 libcurl 行为变化后残留悬空引用。
            curl_easy_setopt(handle, CURLOPT_RESOLVE, nullptr);
            curl_slist_free_all(resolve_list);
            return session.Complete(resolve_result);
        }
    }

    const CURLcode code = curl_easy_perform(handle);
    if(resolve_list != nullptr) {
        // 先解除 easy handle 对列表的引用，再释放当前 Session 专属 slist。
        curl_easy_setopt(handle, CURLOPT_RESOLVE, nullptr);
        curl_slist_free_all(resolve_list);
    }
    return session.Complete(code);
}

// makeHeaders 从已准备快照构造本次 Session 的短生命周期 Header 副本。
cpr::Header makeHeaders(const PreparedHttpMessage& prepared) {
    cpr::Header headers{
        {"Accept",       "application/json, text/event-stream"},
        {"Content-Type", "application/json; charset=utf-8"    }
    };
    if(!prepared.protocol_version.empty()) {
        headers["MCP-Protocol-Version"] = prepared.protocol_version;
    }
    if(!prepared.session_id.empty()) {
        headers["MCP-Session-Id"] = prepared.session_id;
    }
    if(!prepared.credential_header.empty()) {
        headers[prepared.credential_header] = prepared.credential_value;
    }
    return headers;
}

// makeListenerHeaders 同时服务后台 Listener 和前台 POST SSE 恢复 GET。
// 调用方分别传入各自栈上的事件 ID，因此两类流不会共享游标。
cpr::Header makeListenerHeaders(const std::string& protocol_version, const std::string& session_id,
                                const CredentialSnapshot& credential, const std::optional<std::string>& event_id) {
    cpr::Header headers{
        {"Accept", "text/event-stream"}
    };
    if(!protocol_version.empty()) {
        headers["MCP-Protocol-Version"] = protocol_version;
    }
    if(!session_id.empty()) {
        headers["MCP-Session-Id"] = session_id;
    }
    if(!credential.header_name.empty()) {
        headers[credential.header_name] = credential.value;
    }
    if(event_id.has_value() && !event_id->empty()) {
        headers["Last-Event-ID"] = *event_id;
    }
    return headers;
}

// mapCurlFailure 只依据稳定错误码分类，不暴露 libcurl 动态错误文本。
MCPErrorCode mapCurlFailure(const cpr::Response& response) noexcept {
    if(response.error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) {
        return MCPErrorCode::RequestTimeout;
    }
    return MCPErrorCode::TransportFailure;
}

// emitSendFailure 统一携带 dispatch_id，保证 Client 能闭合对应等待者。
void emitSendFailure(const std::shared_ptr<TransportCore>& core, std::uint64_t dispatch_id, MCPErrorCode code,
                     int status = 0) noexcept {
    emitEvent(core, {MCPTransportEventType::SendFailed, dispatch_id, code, status, MCPListenerState::NotApplicable});
}

// parseAndEmitJson 在进入 JSON 解析器前执行消息总上限。
bool parseAndEmitJson(const std::shared_ptr<TransportCore>& core, std::string_view data, MCPErrorCode& error,
                      const std::optional<nlohmann::json>& expected_response_id = std::nullopt,
                      bool* response_delivered = nullptr) {
    if(data.size() > core->limits.max_message_bytes) {
        error = MCPErrorCode::MessageLimitExceeded;
        return false;
    }
    auto parsed = nlohmann::json::parse(data, nullptr, false);
    if(parsed.is_discarded()) {
        error = MCPErrorCode::ProtocolViolation;
        return false;
    }
    if(response_delivered != nullptr && expected_response_id.has_value() && parsed.is_object() &&
       parsed.contains("id") && parsed["id"] == *expected_response_id &&
       (parsed.contains("result") || parsed.contains("error"))) {
        *response_delivered = true;
    }
    emitMessage(core, std::move(parsed));
    return true;
}

// PostRecoveryAttemptResult 只描述一条前台响应恢复 GET。
// 它不产生 Listener 状态事件，父 POST 仍是唯一公开请求。
struct PostRecoveryAttemptResult {
    enum class Disposition { Delivered, Closed, SessionExpired, Terminal, Retryable };

    Disposition disposition = Disposition::Retryable;
    MCPErrorCode error = MCPErrorCode::TransportFailure;
    int http_status = 0;
    bool received_event = false;
    std::optional<std::string> event_id;
    std::optional<std::chrono::milliseconds> retry;
};

// postRecoveryAttempt 使用独立 GET 续接一次 POST SSE 响应流。
// Provider、Session、Decoder、游标变化和取消标志都只属于当前尝试。
PostRecoveryAttemptResult postRecoveryAttempt(const std::shared_ptr<TransportCore>& core,
                                              const PreparedHttpMessage& prepared,
                                              const std::optional<std::string>& prior_event_id) {
    PostRecoveryAttemptResult result;
    if(!prior_event_id.has_value() || prior_event_id->empty()) {
        result.disposition = PostRecoveryAttemptResult::Disposition::Terminal;
        return result;
    }
    const Clock::time_point operation_deadline = operationDeadline(prepared.context);
    if(Clock::now() >= operation_deadline) {
        result.disposition = PostRecoveryAttemptResult::Disposition::Terminal;
        result.error = MCPErrorCode::RequestTimeout;
        return result;
    }
    // 每次恢复 GET 在本次尝试开始时获得新的短建流段，但永远不突破原公开操作上限。
    const Clock::time_point attempt_deadline =
        std::min(operation_deadline, saturatingSteadyDeadlineAfter(core->limits.request_timeout));

    auto cancelled = registerCancellation(core);
    std::string protocol_version;
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(core->mutex);
        if(core->closing || core->session_expired || prepared.session_generation != core->session_generation) {
            result.disposition = PostRecoveryAttemptResult::Disposition::Closed;
            result.error = core->closing ? MCPErrorCode::OperationCancelled : MCPErrorCode::SessionExpired;
            return result;
        }
        // 初始化 POST 的 Session ID 可能刚从响应头捕获，必须读取当前 Core 值。
        protocol_version = core->protocol_version;
        session_id = core->session_id;
    }

    CredentialSnapshot credential;
    try {
        credential = acquireCredential(core, MCPTransportRequestKind::RecoveryGet, operation_deadline, cancelled);
    } catch(const MCPException& exception) {
        result.disposition = exception.code() == MCPErrorCode::OperationCancelled
                                 ? PostRecoveryAttemptResult::Disposition::Closed
                                 : PostRecoveryAttemptResult::Disposition::Terminal;
        result.error = exception.code();
        return result;
    }

    cpr::Session session;
    HeaderState header_state;
    MCPSseDecoder decoder(core->config.max_sse_event_bytes, core->config.max_event_id_bytes,
                          core->config.max_sse_retry_delay);
    bool header_classified = false;
    bool stop_after_headers = false;
    bool callback_failed = false;
    bool response_delivered = false;
    MCPErrorCode callback_error = MCPErrorCode::ProtocolViolation;
    enum class AbortReason { None, AbsoluteTimeout, IdleTimeout } abort_reason = AbortReason::None;

    const cpr::Header headers = makeListenerHeaders(protocol_version, session_id, credential, prior_event_id);
    // SSE 建流成功后允许继续到操作绝对上限；头部到达前仍由 ProgressCallback 使用 attempt_deadline 限制。
    const auto remaining = durationUntil(operation_deadline);
    configureSession(session, core, headers, cancelled, std::min(core->config.connect_timeout, remaining));
    session.SetTimeout(cpr::Timeout{remaining});

    session.SetHeaderCallback(cpr::HeaderCallback([&](std::string_view line, intptr_t) {
        if(!header_state.consume(line, core->config.max_session_id_bytes)) {
            callback_failed = true;
            callback_error = MCPErrorCode::ProtocolViolation;
            return false;
        }
        if(header_state.complete && !header_classified) {
            header_classified = true;
            if(header_state.status != 200 || header_state.content_type != kSseContentType) {
                stop_after_headers = true;
                if(header_state.status >= 200 && header_state.status < 300) {
                    callback_failed = true;
                    callback_error = MCPErrorCode::ProtocolViolation;
                }
            }
        }
        return !cancelled->load() && !stop_after_headers;
    }));

    session.SetWriteCallback(cpr::WriteCallback([&](std::string_view chunk, intptr_t) {
        header_state.last_activity = Clock::now();
        if(cancelled->load()) {
            return false;
        }
        if(header_state.status != 200 || header_state.content_type != kSseContentType) {
            return true;
        }
        try {
            for(auto& event : decoder.feed(chunk)) {
                result.received_event = true;
                if(event.id.has_value()) {
                    result.event_id = std::move(event.id);
                }
                if(event.retry.has_value()) {
                    result.retry = event.retry;
                }
                if(!event.has_data) {
                    continue;
                }
                MCPErrorCode parse_error = MCPErrorCode::ProtocolViolation;
                if(!parseAndEmitJson(core, event.data, parse_error, prepared.expected_response_id,
                                     &response_delivered)) {
                    callback_failed = true;
                    callback_error = parse_error;
                    return false;
                }
                if(response_delivered) {
                    // 匹配终局响应已经闭合父请求，无需等待恢复流由 Server 主动关闭。
                    return false;
                }
            }
            return true;
        } catch(const std::runtime_error& exception) {
            callback_failed = true;
            callback_error = std::string_view{exception.what()}.find("上限") == std::string_view::npos
                                 ? MCPErrorCode::ProtocolViolation
                                 : MCPErrorCode::MessageLimitExceeded;
            return false;
        }
    }));

    session.SetProgressCallback(cpr::ProgressCallback(
        [&](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) {
            const auto now = Clock::now();
            if(cancelled->load()) {
                return false;
            }
            const auto active_deadline = header_state.complete ? operation_deadline : attempt_deadline;
            if(now >= active_deadline) {
                abort_reason = AbortReason::AbsoluteTimeout;
                return false;
            }
            if(header_state.complete && now - header_state.last_activity >= core->config.stream_idle_timeout) {
                abort_reason = AbortReason::IdleTimeout;
                return false;
            }
            return true;
        }));

    const cpr::Response response = performPrepared(session, HttpMethod::Get, core);
    decoder.finish();
    {
        std::lock_guard<std::mutex> lock(core->mutex);
        if(core->closing || core->session_expired || prepared.session_generation != core->session_generation) {
            result.disposition = PostRecoveryAttemptResult::Disposition::Closed;
            result.error = core->closing ? MCPErrorCode::OperationCancelled : MCPErrorCode::SessionExpired;
            return result;
        }
    }

    result.http_status = response.status_code > static_cast<long>(std::numeric_limits<int>::max())
                             ? 0
                             : static_cast<int>(response.status_code);
    if(response_delivered) {
        result.disposition = PostRecoveryAttemptResult::Disposition::Delivered;
        return result;
    }
    if(callback_failed) {
        result.disposition = PostRecoveryAttemptResult::Disposition::Terminal;
        result.error = callback_error;
        return result;
    }
    if(result.http_status == 404 && !session_id.empty()) {
        (void)linearizeSessionExpired(core, session_id, prepared.context.dispatch_id, result.http_status);
        result.disposition = PostRecoveryAttemptResult::Disposition::SessionExpired;
        result.error = MCPErrorCode::SessionExpired;
        return result;
    }
    if(result.http_status == 401 || result.http_status == 403) {
        result.disposition = PostRecoveryAttemptResult::Disposition::Terminal;
        result.error = MCPErrorCode::AuthenticationRequired;
        return result;
    }
    if(result.http_status >= 300 && result.http_status < 500) {
        result.disposition = PostRecoveryAttemptResult::Disposition::Terminal;
        result.error = MCPErrorCode::HttpStatusError;
        return result;
    }
    if(result.http_status >= 500) {
        result.disposition = PostRecoveryAttemptResult::Disposition::Retryable;
        result.error = MCPErrorCode::HttpStatusError;
        return result;
    }
    if(response.error) {
        if(abort_reason == AbortReason::AbsoluteTimeout || Clock::now() >= operation_deadline) {
            result.disposition = PostRecoveryAttemptResult::Disposition::Terminal;
            result.error = MCPErrorCode::RequestTimeout;
        } else {
            result.disposition = PostRecoveryAttemptResult::Disposition::Retryable;
            result.error =
                abort_reason == AbortReason::IdleTimeout ? MCPErrorCode::RequestTimeout : mapCurlFailure(response);
        }
        return result;
    }

    // 合法 SSE 的正常 EOF 在没有匹配响应时仍是可恢复空档。
    result.disposition = PostRecoveryAttemptResult::Disposition::Retryable;
    result.error = MCPErrorCode::TransportFailure;
    return result;
}

enum class PostRecoveryOutcome { Delivered, Failed, SessionExpired, Closed };

// recoverPostSse 在父请求 Worker 内执行有限 GET 恢复。
// retry 等待、Provider、所有 GET 和消息投递都计入父请求原始截止时间。
PostRecoveryOutcome recoverPostSse(const std::shared_ptr<TransportCore>& core, const PreparedHttpMessage& prepared,
                                   std::optional<std::string> event_id, std::chrono::milliseconds retry_delay,
                                   MCPErrorCode initial_error) {
    TestActivityGuard recovery_guard(core->test_overrides.activity_counters, TestActivityKind::SseWorker);
    const Clock::time_point operation_deadline = operationDeadline(prepared.context);
    MCPErrorCode last_error = initial_error;
    std::size_t consecutive_attempts = 0U;
    while(event_id.has_value() && !event_id->empty()) {
        if(Clock::now() >= operation_deadline) {
            emitSendFailure(core, prepared.context.dispatch_id, MCPErrorCode::RequestTimeout);
            return PostRecoveryOutcome::Failed;
        }
        if(consecutive_attempts >= core->config.max_reconnect_attempts) {
            emitSendFailure(core, prepared.context.dispatch_id, last_error);
            return PostRecoveryOutcome::Failed;
        }

        const auto wait_deadline = std::min(
            operation_deadline, saturatingSteadyDeadlineAfter(std::min(retry_delay, core->config.max_sse_retry_delay)));
        {
            std::unique_lock<std::mutex> lock(core->mutex);
            if(core->cv.wait_until(lock, wait_deadline, [&] { return core->closing || core->session_expired; })) {
                return PostRecoveryOutcome::Closed;
            }
        }
        if(Clock::now() >= operation_deadline) {
            emitSendFailure(core, prepared.context.dispatch_id, MCPErrorCode::RequestTimeout);
            return PostRecoveryOutcome::Failed;
        }

        ++consecutive_attempts;
        PostRecoveryAttemptResult attempt = postRecoveryAttempt(core, prepared, event_id);
        if(attempt.event_id.has_value()) {
            if(attempt.event_id->empty()) {
                event_id.reset();
            } else {
                event_id = std::move(attempt.event_id);
            }
        }
        if(attempt.retry.has_value()) {
            retry_delay = std::min(*attempt.retry, core->config.max_sse_retry_delay);
        }
        if(attempt.received_event) {
            consecutive_attempts = 0U;
        }

        switch(attempt.disposition) {
            case PostRecoveryAttemptResult::Disposition::Delivered:
                return PostRecoveryOutcome::Delivered;
            case PostRecoveryAttemptResult::Disposition::Closed:
                return PostRecoveryOutcome::Closed;
            case PostRecoveryAttemptResult::Disposition::SessionExpired:
                return PostRecoveryOutcome::SessionExpired;
            case PostRecoveryAttemptResult::Disposition::Terminal:
                emitSendFailure(core, prepared.context.dispatch_id, attempt.error, attempt.http_status);
                return PostRecoveryOutcome::Failed;
            case PostRecoveryAttemptResult::Disposition::Retryable:
                last_error = attempt.error;
                break;
        }
    }

    // Server 显式清除 ID 后无法继续证明续传位置，禁止发起无游标恢复。
    emitSendFailure(core, prepared.context.dispatch_id, last_error);
    return PostRecoveryOutcome::Failed;
}

// processPost 执行一条已经完成两阶段提交的独立 POST。
// 每条消息都创建新的 cpr Session，因此控制 POST 与前台请求不会共享句柄。
void processPost(const std::shared_ptr<TransportCore>& core, std::unique_ptr<PreparedHttpMessage> prepared) noexcept {
    try {
        const Clock::time_point operation_deadline = operationDeadline(prepared->context);
        if(Clock::now() >= prepared->context.deadline || Clock::now() >= operation_deadline) {
            emitSendFailure(core, prepared->context.dispatch_id, MCPErrorCode::RequestTimeout);
            return;
        }

        auto cancelled = registerCancellation(core);
        if(cancelled->load()) {
            return;
        }
        cpr::Session session;
        HeaderState header_state;
        MCPErrorCode callback_error = MCPErrorCode::ProtocolViolation;
        bool callback_failed = false;
        bool response_delivered = false;
        bool session_captured = false;
        bool post_sse_established = false;
        std::string json_body;
        std::unique_ptr<MCPSseDecoder> decoder;
        std::optional<std::string> post_event_id;
        std::chrono::milliseconds post_retry_delay{1000};
        enum class PostAbortReason { None, AbsoluteTimeout, IdleTimeout } post_abort_reason = PostAbortReason::None;

        const cpr::Header headers = makeHeaders(*prepared);
        // cpr 的总超时覆盖 SSE 全寿命；短请求段由 ProgressCallback 在响应头前或 JSON 正文期裁决。
        const auto remaining = durationUntil(operation_deadline);
        configureSession(session, core, headers, cancelled, std::min(core->config.connect_timeout, remaining));
        session.SetTimeout(cpr::Timeout{remaining});
        session.SetBody(cpr::Body{prepared->body});

        // HeaderCallback 在正文回调之前完成媒体类型与会话分类。
        // 初始化会话 ID 必须先安全保存，随后才能向 Client 投递初始化结果。
        session.SetHeaderCallback(cpr::HeaderCallback([&](std::string_view line, intptr_t) {
            if(!header_state.consume(line, core->config.max_session_id_bytes)) {
                callback_failed = true;
                callback_error = MCPErrorCode::ProtocolViolation;
                return false;
            }
            if(header_state.complete && header_state.status >= 200 && header_state.status < 300 &&
               prepared->context.kind == MCPTransportRequestKind::Initialize && !session_captured) {
                session_captured = true;
                if(header_state.duplicate_session_id) {
                    callback_failed = true;
                    callback_error = MCPErrorCode::ProtocolViolation;
                    return false;
                }
                if(header_state.session_id.has_value()) {
                    if(!validateVisibleSessionId(*header_state.session_id, core->config.max_session_id_bytes)) {
                        callback_failed = true;
                        callback_error = MCPErrorCode::ProtocolViolation;
                        return false;
                    }
                    std::lock_guard<std::mutex> lock(core->mutex);
                    if(!core->closing && !core->session_expired) {
                        secureClear(core->session_id);
                        core->session_id = *header_state.session_id;
                        ++core->session_generation;
                        core->session_expired_requested.store(false);
                    }
                }
            }
            if(header_state.complete && header_state.status >= 200 && header_state.status < 300 &&
               header_state.content_type == kSseContentType && !post_sse_established) {
                // Header 已完整且媒体类型合法，Client 才可把前台等待从请求段切换到绝对上限。
                post_sse_established = true;
                emitEvent(core, {MCPTransportEventType::ResponseStreamEstablished, prepared->context.dispatch_id,
                                 MCPErrorCode::TransportFailure, header_state.status, MCPListenerState::NotApplicable});
            }
            return !cancelled->load();
        }));

        // WriteCallback 对 JSON 使用有界聚合，对 SSE 使用逐块解码。
        // POST SSE 断线绝不重发原始 POST；有游标时只用独立 GET 恢复响应流。
        session.SetWriteCallback(cpr::WriteCallback([&](std::string_view chunk, intptr_t) {
            header_state.last_activity = Clock::now();
            if(cancelled->load()) {
                return false;
            }
            if(header_state.content_type == kJsonContentType) {
                if(chunk.size() >
                   core->limits.max_message_bytes - std::min(core->limits.max_message_bytes, json_body.size())) {
                    callback_failed = true;
                    callback_error = MCPErrorCode::MessageLimitExceeded;
                    return false;
                }
                json_body.append(chunk.data(), chunk.size());
                return true;
            }
            if(header_state.content_type == kSseContentType) {
                try {
                    if(!decoder) {
                        decoder = std::make_unique<MCPSseDecoder>(core->config.max_sse_event_bytes,
                                                                  core->config.max_event_id_bytes,
                                                                  core->config.max_sse_retry_delay);
                    }
                    for(auto& event : decoder->feed(chunk)) {
                        if(event.id.has_value()) {
                            if(event.id->empty()) {
                                post_event_id.reset();
                            } else {
                                post_event_id = std::move(event.id);
                            }
                        }
                        if(event.retry.has_value()) {
                            post_retry_delay = std::min(*event.retry, core->config.max_sse_retry_delay);
                        }
                        if(!event.has_data) {
                            continue;
                        }
                        MCPErrorCode parse_error = MCPErrorCode::ProtocolViolation;
                        if(!parseAndEmitJson(core, event.data, parse_error, prepared->expected_response_id,
                                             &response_delivered)) {
                            callback_failed = true;
                            callback_error = parse_error;
                            return false;
                        }
                        if(response_delivered) {
                            // 匹配响应到达后主动结束当前 POST SSE，释放 Worker 给后续控制消息。
                            return false;
                        }
                    }
                    return true;
                } catch(const std::runtime_error& exception) {
                    callback_failed = true;
                    callback_error = std::string_view{exception.what()}.find("上限") == std::string_view::npos
                                         ? MCPErrorCode::ProtocolViolation
                                         : MCPErrorCode::MessageLimitExceeded;
                    return false;
                }
            }

            // 成功状态的未知媒体类型属于协议违规；错误状态正文则直接丢弃。
            if(header_state.status >= 200 && header_state.status < 300) {
                callback_failed = true;
                callback_error = MCPErrorCode::ProtocolViolation;
                return false;
            }
            return true;
        }));

        // POST SSE 的空闲中断应在父截止前触发，给带游标的恢复 GET 留出预算。
        session.SetProgressCallback(cpr::ProgressCallback(
            [&](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) {
                const auto now = Clock::now();
                if(cancelled->load()) {
                    return false;
                }
                const bool post_sse = header_state.complete && header_state.content_type == kSseContentType;
                const auto active_deadline = post_sse ? operation_deadline : prepared->context.deadline;
                if(now >= active_deadline) {
                    post_abort_reason = PostAbortReason::AbsoluteTimeout;
                    return false;
                }
                if(header_state.complete && header_state.content_type == kSseContentType &&
                   now - header_state.last_activity >= core->config.stream_idle_timeout) {
                    post_abort_reason = PostAbortReason::IdleTimeout;
                    return false;
                }
                return true;
            }));

        const cpr::Response response = performPrepared(session, HttpMethod::Post, core);
        if(decoder) {
            decoder->finish();
        }

        {
            std::lock_guard<std::mutex> lock(core->mutex);
            const bool generation_changed = prepared->context.kind != MCPTransportRequestKind::Initialize &&
                                            prepared->session_generation != core->session_generation;
            if(core->closing || core->session_expired || generation_changed) {
                return;
            }
        }

        const int status = response.status_code > static_cast<long>(std::numeric_limits<int>::max())
                               ? 0
                               : static_cast<int>(response.status_code);
        if(callback_failed) {
            emitSendFailure(core, prepared->context.dispatch_id, callback_error, status);
            return;
        }
        if((status == 401) || (status == 403)) {
            emitSendFailure(core, prepared->context.dispatch_id, MCPErrorCode::AuthenticationRequired, status);
            return;
        }

        const bool has_session = !prepared->session_id.empty();
        if(status == 404 && has_session) {
            (void)linearizeSessionExpired(core, prepared->session_id, prepared->context.dispatch_id, status);
            return;
        }

        const bool expects_response = prepared->context.kind == MCPTransportRequestKind::Initialize ||
                                      prepared->context.kind == MCPTransportRequestKind::Ping ||
                                      prepared->context.kind == MCPTransportRequestKind::ListTools ||
                                      prepared->context.kind == MCPTransportRequestKind::CallTool;

        // POST SSE 已经投递匹配响应后，即使 Server 随后关闭流也不能把成功改写为失败。
        if(status >= 200 && status < 300 && response_delivered) {
            return;
        }
        if(!expects_response) {
            if(response.error) {
                emitSendFailure(core, prepared->context.dispatch_id, mapCurlFailure(response), status);
                return;
            }
            if(status == 202 || status == 204) {
                emitEvent(core, {MCPTransportEventType::SendCompleted, prepared->context.dispatch_id,
                                 MCPErrorCode::TransportFailure, status, MCPListenerState::NotApplicable});
            } else {
                emitSendFailure(core, prepared->context.dispatch_id, MCPErrorCode::HttpStatusError, status);
            }
            return;
        }

        if(status < 200 || status >= 300) {
            emitSendFailure(core, prepared->context.dispatch_id, MCPErrorCode::HttpStatusError, status);
            return;
        }
        if(header_state.content_type == kJsonContentType) {
            if(response.error) {
                emitSendFailure(core, prepared->context.dispatch_id, mapCurlFailure(response), status);
                return;
            }
            MCPErrorCode parse_error = MCPErrorCode::ProtocolViolation;
            if(!parseAndEmitJson(core, json_body, parse_error, prepared->expected_response_id, &response_delivered)) {
                emitSendFailure(core, prepared->context.dispatch_id, parse_error, status);
            } else if(!response_delivered) {
                emitSendFailure(core, prepared->context.dispatch_id, MCPErrorCode::ProtocolViolation, status);
            }
            return;
        }
        if(header_state.content_type == kSseContentType) {
            MCPErrorCode disconnect_error = MCPErrorCode::TransportFailure;
            if(post_abort_reason == PostAbortReason::AbsoluteTimeout || Clock::now() >= operation_deadline) {
                disconnect_error = MCPErrorCode::RequestTimeout;
            } else if(post_abort_reason == PostAbortReason::IdleTimeout) {
                disconnect_error = MCPErrorCode::RequestTimeout;
            } else if(response.error) {
                disconnect_error = mapCurlFailure(response);
            }

            if(post_event_id.has_value() && !post_event_id->empty() && Clock::now() < operation_deadline) {
                (void)recoverPostSse(core, *prepared, std::move(post_event_id), post_retry_delay, disconnect_error);
            } else {
                emitSendFailure(core, prepared->context.dispatch_id, disconnect_error, status);
            }
            return;
        }
        emitSendFailure(core, prepared->context.dispatch_id, MCPErrorCode::ProtocolViolation, status);
    } catch(...) {
        emitSendFailure(core, prepared ? prepared->context.dispatch_id : 0U, MCPErrorCode::TransportFailure);
    }
}

// postWorkerLoop 是固定数量 Worker 的唯一队列消费入口。
// Worker 在网络 I/O 期间不持有队列锁，其他控制消息仍可由另一个 Worker 处理。
void postWorkerLoop(const std::shared_ptr<TransportCore>& core, std::size_t worker_index) noexcept {
    for(;;) {
        std::unique_ptr<PreparedHttpMessage> prepared;
        {
            std::unique_lock<std::mutex> lock(core->mutex);
            core->cv.wait(lock, [&] { return core->queue_stop || !core->queue.empty(); });
            if(core->queue_stop && core->queue.empty()) {
                break;
            }
            prepared = std::move(core->queue.front());
            core->queue.pop_front();
        }
        processPost(core, std::move(prepared));
    }

    std::lock_guard<std::mutex> lock(core->mutex);
    core->worker_done[worker_index] = true;
    core->cv.notify_all();
}

// ListenerAttemptResult 描述一次 GET 的完整闭合结果。
// 事件游标和 retry 由 listenerLoop 持有，不写入共享 POST 状态。
struct ListenerAttemptResult {
    enum class Disposition { Closed, StartedThenEnded, Unsupported, SessionExpired, Terminal, Retryable };

    Disposition disposition = Disposition::Retryable;
    MCPErrorCode error = MCPErrorCode::TransportFailure;
    int http_status = 0;
    bool received_event = false;
    std::optional<std::string> event_id;
    std::optional<std::chrono::milliseconds> retry;
};

// listenerAttempt 建立一条 GET SSE，并在首个最终响应头完成时立即分类。
// 长期流没有总寿命，但建流截止和每次数据空闲都受独立上限约束。
ListenerAttemptResult listenerAttempt(const std::shared_ptr<TransportCore>& core, bool first_attempt,
                                      Clock::time_point header_deadline,
                                      const std::optional<std::string>& prior_event_id) {
    ListenerAttemptResult result;
    auto cancelled = registerCancellation(core);

    std::string protocol_version;
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(core->mutex);
        if(core->closing || core->session_expired) {
            result.disposition = ListenerAttemptResult::Disposition::Closed;
            return result;
        }
        protocol_version = core->protocol_version;
        session_id = core->session_id;
    }

    CredentialSnapshot credential;
    try {
        credential = acquireCredential(
            core, first_attempt ? MCPTransportRequestKind::ListenerGet : MCPTransportRequestKind::RecoveryGet,
            header_deadline, cancelled);
    } catch(const MCPException& exception) {
        result.disposition =
            first_attempt ? ListenerAttemptResult::Disposition::Terminal : ListenerAttemptResult::Disposition::Terminal;
        result.error = exception.code();
        return result;
    }

    cpr::Session session;
    HeaderState header_state;
    MCPSseDecoder decoder(core->config.max_sse_event_bytes, core->config.max_event_id_bytes,
                          core->config.max_sse_retry_delay);
    bool header_classified = false;
    bool stop_after_headers = false;
    bool callback_failed = false;
    MCPErrorCode callback_error = MCPErrorCode::ProtocolViolation;
    enum class AbortReason { None, HeaderTimeout, IdleTimeout } abort_reason = AbortReason::None;

    const cpr::Header headers = makeListenerHeaders(protocol_version, session_id, credential, prior_event_id);
    configureSession(session, core, headers, cancelled,
                     std::min(core->config.connect_timeout, durationUntil(header_deadline)));

    // HeaderCallback 必须在长流正文到来前发布 Listening，避免 connect 等待流关闭。
    session.SetHeaderCallback(cpr::HeaderCallback([&](std::string_view line, intptr_t) {
        if(!header_state.consume(line, core->config.max_session_id_bytes)) {
            callback_failed = true;
            callback_error = MCPErrorCode::ProtocolViolation;
            return false;
        }
        if(header_state.complete && !header_classified) {
            header_classified = true;
            if(header_state.status == 200 && header_state.content_type == kSseContentType) {
                emitEvent(core, {MCPTransportEventType::ListenerStarted, 0U, MCPErrorCode::TransportFailure, 200,
                                 MCPListenerState::Listening});
            } else {
                // 非 SSE 成功响应属于协议错误；其他状态在 perform 返回后按闭合矩阵分类。
                stop_after_headers = true;
                if(header_state.status == 200) {
                    callback_failed = true;
                    callback_error = MCPErrorCode::ProtocolViolation;
                }
            }
        }
        return !cancelled->load() && !stop_after_headers;
    }));

    // SSE 数据按事件解析；空 data、注释和仅 retry/id 的事件只维护流状态。
    session.SetWriteCallback(cpr::WriteCallback([&](std::string_view chunk, intptr_t) {
        header_state.last_activity = Clock::now();
        if(cancelled->load()) {
            return false;
        }
        if(header_state.status != 200 || header_state.content_type != kSseContentType) {
            return true;
        }
        try {
            for(auto& event : decoder.feed(chunk)) {
                result.received_event = true;
                if(event.id.has_value()) {
                    result.event_id = std::move(event.id);
                }
                if(event.retry.has_value()) {
                    result.retry = event.retry;
                }
                if(!event.has_data) {
                    continue;
                }
                MCPErrorCode parse_error = MCPErrorCode::ProtocolViolation;
                if(!parseAndEmitJson(core, event.data, parse_error)) {
                    callback_failed = true;
                    callback_error = parse_error;
                    return false;
                }
            }
            return true;
        } catch(const std::runtime_error& exception) {
            callback_failed = true;
            callback_error = std::string_view{exception.what()}.find("上限") == std::string_view::npos
                                 ? MCPErrorCode::ProtocolViolation
                                 : MCPErrorCode::MessageLimitExceeded;
            return false;
        }
    }));

    // ProgressCallback 提供响应头截止与流空闲截止；长期 Listener 不设置总 Timeout。
    session.SetProgressCallback(cpr::ProgressCallback(
        [&](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) {
            const auto now = Clock::now();
            if(cancelled->load()) {
                return false;
            }
            if(!header_state.complete && now >= header_deadline) {
                abort_reason = AbortReason::HeaderTimeout;
                return false;
            }
            if(header_state.complete && now - header_state.last_activity >= core->config.stream_idle_timeout) {
                abort_reason = AbortReason::IdleTimeout;
                return false;
            }
            return true;
        }));

    const cpr::Response response = performPrepared(session, HttpMethod::Get, core);
    decoder.finish();
    {
        std::lock_guard<std::mutex> lock(core->mutex);
        if(core->closing || core->session_expired) {
            result.disposition = ListenerAttemptResult::Disposition::Closed;
            return result;
        }
    }

    result.http_status = response.status_code > static_cast<long>(std::numeric_limits<int>::max())
                             ? 0
                             : static_cast<int>(response.status_code);
    if(callback_failed) {
        result.disposition = ListenerAttemptResult::Disposition::Terminal;
        result.error = callback_error;
        return result;
    }
    if(result.http_status == 405) {
        result.disposition = ListenerAttemptResult::Disposition::Unsupported;
        return result;
    }
    if(result.http_status == 404 && !session_id.empty()) {
        (void)linearizeSessionExpired(core, session_id, 0U, result.http_status);
        result.disposition = ListenerAttemptResult::Disposition::SessionExpired;
        result.error = MCPErrorCode::SessionExpired;
        return result;
    }
    if(result.http_status == 401 || result.http_status == 403) {
        result.disposition = ListenerAttemptResult::Disposition::Terminal;
        result.error = MCPErrorCode::AuthenticationRequired;
        return result;
    }
    if(result.http_status >= 300 && result.http_status < 500) {
        result.disposition = ListenerAttemptResult::Disposition::Terminal;
        result.error = MCPErrorCode::HttpStatusError;
        return result;
    }
    if(result.http_status >= 500) {
        result.disposition = ListenerAttemptResult::Disposition::Retryable;
        result.error = MCPErrorCode::HttpStatusError;
        return result;
    }
    // Listener 只有 200 + text/event-stream 是合法成功；201、202、204 不能被当作可恢复 EOF。
    // 该判断必须位于 response.error 之前，因为 HeaderCallback 为尽早中止非法 2xx 会得到本地取消错误。
    if(result.http_status >= 200 && result.http_status < 300 &&
       (result.http_status != 200 || header_state.content_type != kSseContentType)) {
        result.disposition = ListenerAttemptResult::Disposition::Terminal;
        result.error = MCPErrorCode::ProtocolViolation;
        return result;
    }
    if(response.error) {
        result.disposition = ListenerAttemptResult::Disposition::Retryable;
        result.error = abort_reason == AbortReason::HeaderTimeout || abort_reason == AbortReason::IdleTimeout
                           ? MCPErrorCode::RequestTimeout
                           : mapCurlFailure(response);
        return result;
    }

    // 正常 EOF 对长期 Listener 仍是意外空档，必须进入恢复路径。
    result.disposition = header_classified ? ListenerAttemptResult::Disposition::StartedThenEnded
                                           : ListenerAttemptResult::Disposition::Retryable;
    result.error = MCPErrorCode::TransportFailure;
    return result;
}

// listenerLoop 持有 Listener 专属恢复状态。
// 首次建流不重试；只有已经成功建立过的长期流才进入有限恢复。
void listenerLoop(const std::shared_ptr<TransportCore>& core) noexcept {
    TestActivityGuard listener_guard(core->test_overrides.activity_counters, TestActivityKind::SseWorker);
    Clock::time_point initial_deadline;
    {
        std::unique_lock<std::mutex> lock(core->mutex);
        core->cv.wait(lock, [&] { return core->listener_requested || core->closing || core->session_expired; });
        if(core->closing || core->session_expired) {
            core->listener_thread_done = true;
            core->cv.notify_all();
            return;
        }
        initial_deadline = core->listener_initial_deadline;
    }

    bool first_attempt = true;
    bool ever_started = false;
    std::size_t consecutive_failures = 0U;
    std::optional<std::string> event_id;
    std::chrono::milliseconds retry_delay{1000};

    for(;;) {
        // 每次恢复只取得新的请求段预算，超长配置在 steady_clock 上界处饱和。
        const Clock::time_point header_deadline =
            first_attempt ? initial_deadline : saturatingSteadyDeadlineAfter(core->limits.request_timeout);
        ListenerAttemptResult attempt = listenerAttempt(core, first_attempt, header_deadline, event_id);

        if(attempt.event_id.has_value()) {
            if(attempt.event_id->empty()) {
                event_id.reset();
            } else {
                event_id = std::move(attempt.event_id);
            }
        }
        if(attempt.retry.has_value()) {
            retry_delay = std::min(*attempt.retry, core->config.max_sse_retry_delay);
        }
        if(attempt.received_event) {
            // 收到合法完整事件才证明恢复流稳定，响应头本身不足以清零计数。
            consecutive_failures = 0U;
        }

        if(attempt.disposition == ListenerAttemptResult::Disposition::Closed) {
            break;
        }
        if(attempt.disposition == ListenerAttemptResult::Disposition::Unsupported) {
            emitEvent(core, {MCPTransportEventType::ListenerUnsupported, 0U, MCPErrorCode::HttpStatusError,
                             attempt.http_status, MCPListenerState::Unsupported});
            break;
        }
        if(attempt.disposition == ListenerAttemptResult::Disposition::SessionExpired) {
            break;
        }
        if(attempt.disposition == ListenerAttemptResult::Disposition::Terminal) {
            if(attempt.error == MCPErrorCode::ProtocolViolation ||
               attempt.error == MCPErrorCode::MessageLimitExceeded) {
                emitEvent(core, {MCPTransportEventType::ListenerStopped, 0U, attempt.error, attempt.http_status,
                                 MCPListenerState::Stopped});
            } else {
                emitEvent(core, {MCPTransportEventType::ListenerUnavailable, 0U, attempt.error, attempt.http_status,
                                 MCPListenerState::Unavailable});
            }
            break;
        }

        // 200 + SSE 的响应头已经由 attempt 内部发布 ListenerStarted。
        if(attempt.disposition == ListenerAttemptResult::Disposition::StartedThenEnded || attempt.http_status == 200) {
            ever_started = true;
        }

        if(first_attempt && !ever_started) {
            // 初始化路径明确禁止隐藏重试，失败必须让 connect() 直接闭合。
            emitEvent(core, {MCPTransportEventType::ListenerUnavailable, 0U, attempt.error, attempt.http_status,
                             MCPListenerState::Unavailable});
            break;
        }
        first_attempt = false;
        ++consecutive_failures;

        if(consecutive_failures > core->config.max_reconnect_attempts) {
            emitEvent(core, {MCPTransportEventType::ListenerUnavailable, 0U, attempt.error, attempt.http_status,
                             MCPListenerState::Unavailable});
            break;
        }

        emitEvent(core, {MCPTransportEventType::ListenerRecovering, 0U, attempt.error, attempt.http_status,
                         MCPListenerState::Recovering});
        std::unique_lock<std::mutex> lock(core->mutex);
        if(core->cv.wait_for(lock, retry_delay, [&] { return core->closing || core->session_expired; })) {
            break;
        }
    }

    std::lock_guard<std::mutex> lock(core->mutex);
    core->listener_thread_done = true;
    core->cv.notify_all();
}

// deleteSessionBestEffort 只在关闭预算与凭据门都立即可用时执行。
// DELETE 的响应头和正文直接丢弃，避免清理请求接受远端无界数据。
void deleteSessionBestEffort(const std::shared_ptr<TransportCore>& core, const std::string& protocol_version,
                             const std::string& session_id, Clock::time_point close_deadline) noexcept {
    if(session_id.empty() || Clock::now() >= close_deadline) {
        return;
    }

    std::shared_ptr<std::atomic_bool> cancelled;
    try {
        cancelled = std::make_shared<std::atomic_bool>(false);
    } catch(...) {
        return;
    }
    std::unique_lock<std::mutex> credential_lock(core->credential_gate_mutex, std::try_to_lock);
    if(!credential_lock.owns_lock() || core->credential_gate_busy) {
        // 正在执行的 Provider 优先服从关闭取消；关闭不能为 DELETE 排队等待凭据门。
        return;
    }
    core->credential_gate_busy = true;
    credential_lock.unlock();

    try {
        CredentialSnapshot credential =
            acquireCredential(core, MCPTransportRequestKind::DeleteSession, close_deadline, cancelled, true, true);
        if(Clock::now() >= close_deadline) {
            return;
        }

        cpr::Header headers{
            {"Accept",         "application/json, text/event-stream"},
            {"MCP-Session-Id", session_id                           }
        };
        if(!protocol_version.empty()) {
            headers["MCP-Protocol-Version"] = protocol_version;
        }
        if(!credential.header_name.empty()) {
            headers[credential.header_name] = credential.value;
        }

        cpr::Session session;
        const auto remaining = durationUntil(close_deadline);
        configureSession(session, core, headers, cancelled, std::min(core->config.connect_timeout, remaining));
        session.SetTimeout(cpr::Timeout{remaining});
        session.SetHeaderCallback(cpr::HeaderCallback([](std::string_view, intptr_t) { return true; }));
        session.SetWriteCallback(cpr::WriteCallback([](std::string_view, intptr_t) { return true; }));
        (void)performPrepared(session, HttpMethod::Delete, core);
        // 任意 2xx、404、405 都允许本地关闭完成；其他结果同样不得阻塞资源回收。
    } catch(...) {
        // DELETE 是尽力清理，不得覆盖关闭结果或传播凭据异常。
    }
}

class StreamableHttpMCPTransport final : public IMCPTransport {
   public:
    StreamableHttpMCPTransport(MCPStreamableHttpConfig config, MCPCommonLimits limits,
                               MCPHttpTestOverrides test_overrides = {})
        : core_(std::make_shared<TransportCore>(std::move(config), std::move(limits), std::move(test_overrides))) {}

    ~StreamableHttpMCPTransport() override {
        // 析构只执行立即取消与安全回收，不为已耗尽的 Client 关闭流程重启预算。
        close(Clock::now());
    }

    // open 只建立固定后台执行资源；网络连接仍要等消息提交或 Listener 启动。
    void open(MCPTransportCallbacks callbacks, Clock::time_point absolute_deadline) override {
        if(Clock::now() >= absolute_deadline) {
            throw MCPException(MCPErrorCode::RequestTimeout, MCPClientState::Connecting,
                               "MCP Streamable HTTP Transport 打开前已超时");
        }
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        {
            std::lock_guard<std::mutex> lock(core_->mutex);
            if(core_->opened || core_->closing) {
                throw std::logic_error("MCP Streamable HTTP Transport 不能重复打开");
            }
            if(!callbacks.on_message || !callbacks.on_event) {
                throw std::invalid_argument("MCP Streamable HTTP Transport 回调不能为空");
            }
            core_->callbacks = std::move(callbacks);
            core_->opened = true;
        }

        try {
            for(std::size_t index = 0U; index < kPostWorkerCount; ++index) {
                if(Clock::now() >= absolute_deadline) {
                    throw MCPException(MCPErrorCode::RequestTimeout, MCPClientState::Connecting,
                                       "MCP Streamable HTTP Transport 打开期间超时");
                }
                workers_[index] = std::thread([core = core_, index] { postWorkerLoop(core, index); });
            }
            if(Clock::now() >= absolute_deadline) {
                throw MCPException(MCPErrorCode::RequestTimeout, MCPClientState::Connecting,
                                   "MCP Streamable HTTP Transport 打开期间超时");
            }
            listener_thread_ = std::thread([core = core_] { listenerLoop(core); });
            if(Clock::now() >= absolute_deadline) {
                throw MCPException(MCPErrorCode::RequestTimeout, MCPClientState::Connecting,
                                   "MCP Streamable HTTP Transport 打开期间超时");
            }
        } catch(...) {
            // 线程创建部分失败时必须先让已经创建的 Worker 收敛，再向 Client 报告。
            {
                std::lock_guard<std::mutex> lock(core_->mutex);
                core_->closing = true;
                core_->close_requested.store(true);
                core_->queue_stop = true;
                core_->callbacks = {};
                core_->cv.notify_all();
            }
            core_->credential_gate_cv.notify_all();
            for(auto& worker : workers_) {
                if(worker.joinable()) {
                    worker.join();
                }
            }
            if(listener_thread_.joinable()) {
                listener_thread_.join();
            }
            {
                std::lock_guard<std::mutex> lock(core_->mutex);
                core_->close_complete = true;
                core_->cv.notify_all();
            }
            throw;
        }
    }

    // prepareMessage 形成正文、协议、会话和逐请求凭据的一次性快照。
    // Provider 在 Transport 状态锁外运行，关闭通过协作取消视图通知它。
    std::unique_ptr<IMCPPreparedMessage> prepareMessage(const nlohmann::json& message,
                                                        const MCPTransportRequestContext& context) override {
        // 凭据尚未形成网络写入，请求段尚未开始；这里只能消耗公开操作绝对上限。
        const Clock::time_point operation_deadline = operationDeadline(context);
        if(Clock::now() >= operation_deadline) {
            throw MCPException(MCPErrorCode::RequestTimeout, stateForKind(context.kind), "MCP HTTP 请求在准备前已超时");
        }

        auto prepared = std::make_unique<PreparedHttpMessage>();
        prepared->owner = core_.get();
        prepared->context = context;
        try {
            prepared->body = message.dump();
        } catch(...) {
            throw MCPException(MCPErrorCode::ProtocolViolation, stateForKind(context.kind), "MCP HTTP 消息无法序列化");
        }
        if(prepared->body.size() > core_->limits.max_message_bytes) {
            throw MCPException(MCPErrorCode::MessageLimitExceeded, stateForKind(context.kind),
                               "MCP HTTP 消息超过配置上限");
        }
        if(message.is_object() && message.contains("id") && !message["id"].is_null()) {
            prepared->expected_response_id = message["id"];
        }

        {
            std::lock_guard<std::mutex> lock(core_->mutex);
            if(!core_->opened) {
                throw std::logic_error("MCP Streamable HTTP Transport 尚未打开");
            }
            if(core_->closing) {
                throw MCPException(MCPErrorCode::OperationCancelled, MCPClientState::Closing,
                                   "MCP HTTP 请求已被关闭取消");
            }
            prepared->protocol_version = core_->protocol_version;
            prepared->session_id = core_->session_id;
            prepared->session_generation = core_->session_generation;
        }

        auto cancelled = registerCancellation(core_);
        CredentialSnapshot credential = acquireCredential(core_, context.kind, operation_deadline, cancelled);
        {
            std::lock_guard<std::mutex> lock(core_->mutex);
            if(core_->closing) {
                throw MCPException(MCPErrorCode::OperationCancelled, MCPClientState::Closing,
                                   "MCP HTTP 请求已被关闭取消");
            }
            if(core_->session_expired || prepared->session_generation != core_->session_generation) {
                throw MCPException(MCPErrorCode::SessionExpired, stateForKind(context.kind),
                                   "MCP HTTP 会话在凭据准备期间失效");
            }
        }
        prepared->credential_header = std::move(credential.header_name);
        prepared->credential_value = std::move(credential.value);
        return prepared;
    }

    // commitPrepared 的成功返回是唯一 Submitted 线性化点。
    // 本方法不调用用户代码、不创建 Session，也不等待网络。
    void commitPrepared(std::unique_ptr<IMCPPreparedMessage> prepared, Clock::time_point request_deadline) override {
        auto* typed = dynamic_cast<PreparedHttpMessage*>(prepared.get());
        if(typed == nullptr || typed->owner != core_.get()) {
            throw std::invalid_argument("MCP HTTP 准备消息不属于当前 Transport");
        }
        std::unique_ptr<PreparedHttpMessage> owned(static_cast<PreparedHttpMessage*>(prepared.release()));

        std::lock_guard<std::mutex> lock(core_->mutex);
        if(core_->closing) {
            throw MCPException(MCPErrorCode::OperationCancelled, MCPClientState::Closing,
                               "MCP HTTP 请求提交已被关闭取消");
        }
        if(core_->session_expired || owned->session_generation != core_->session_generation) {
            throw MCPException(MCPErrorCode::SessionExpired, stateForKind(owned->context.kind),
                               "MCP HTTP 请求提交时会话已经失效");
        }
        if(core_->queue_stop) {
            throw MCPException(MCPErrorCode::OperationCancelled, MCPClientState::Closing,
                               "MCP HTTP 请求提交已被传输停止取消");
        }
        if(core_->queue.size() >= core_->limits.max_pending_messages) {
            throw MCPException(MCPErrorCode::MessageQueueOverflow, stateForKind(owned->context.kind),
                               "MCP HTTP 发送队列已满");
        }
        // 只有成功跨过队列提交点才激活请求段；准备阶段的凭据时间不会偷走该预算。
        owned->context.deadline = request_deadline;
        core_->queue.push_back(std::move(owned));
        // Listener 与 POST Worker 共用条件变量；notify_one 可能只唤醒谓词为假的 Listener。
        // 广播后只有队列谓词成立的 Worker 会消费消息，避免已提交控制 POST 永久滞留。
        core_->cv.notify_all();
    }

    // completeInitialization 固定后续请求头使用的唯一协议版本。
    void completeInitialization(const std::string& protocol_version) override {
        if(protocol_version != kMCPProtocolVersion) {
            throw MCPException(MCPErrorCode::VersionMismatch, MCPClientState::Initializing,
                               "MCP HTTP 协商版本不受支持");
        }
        std::lock_guard<std::mutex> lock(core_->mutex);
        if(core_->closing) {
            throw MCPException(MCPErrorCode::OperationCancelled, MCPClientState::Closing,
                               "MCP HTTP 初始化已被关闭取消");
        }
        if(core_->session_expired) {
            throw MCPException(MCPErrorCode::SessionExpired, MCPClientState::Faulted, "MCP HTTP 会话已经失效");
        }
        core_->protocol_version = protocol_version;
    }

    // startListener 仅设置一次启动请求，Provider 与网络 I/O 都由长期线程执行。
    void startListener(Clock::time_point deadline) override {
        std::lock_guard<std::mutex> lock(core_->mutex);
        if(!core_->opened) {
            throw std::logic_error("MCP Streamable HTTP Transport 尚未打开");
        }
        if(core_->closing) {
            throw MCPException(MCPErrorCode::OperationCancelled, MCPClientState::Closing,
                               "MCP Listener 启动已被关闭取消");
        }
        if(core_->session_expired) {
            throw MCPException(MCPErrorCode::SessionExpired, MCPClientState::Faulted, "MCP HTTP 会话已经失效");
        }
        if(core_->listener_requested) {
            throw std::logic_error("MCP Listener 不能重复启动");
        }
        core_->listener_requested = true;
        core_->listener_initial_deadline = deadline;
        core_->cv.notify_all();
    }

    // close 是幂等、有界的本地资源关闭入口。
    // 所有请求已安装取消与建连上限，关闭必须 join 固定线程，禁止返回后遗留任务。
    void close(Clock::time_point absolute_deadline) noexcept override {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        // 删除会话、取消请求和线程收敛共享 MCPClient 生成的唯一关闭截止点。
        const Clock::time_point close_deadline = absolute_deadline;
        std::string protocol_version;
        std::string session_id;
        {
            std::unique_lock<std::mutex> lock(core_->mutex);
            if(core_->close_complete) {
                return;
            }
            if(core_->closing) {
                core_->cv.wait_until(lock, close_deadline, [&] { return core_->close_complete; });
                return;
            }

            core_->closing = true;
            core_->close_requested.store(true);
            core_->queue_stop = true;
            core_->callbacks = {};
            core_->queue.clear();
            if(!core_->opened) {
                core_->worker_done.fill(true);
                core_->listener_thread_done = true;
            }
            protocol_version = core_->protocol_version;
            session_id = core_->session_id;
            secureClear(core_->protocol_version);
            secureClear(core_->session_id);
            for(auto& weak_cancelled : core_->active_cancellations) {
                if(auto cancelled = weak_cancelled.lock()) {
                    cancelled->store(true);
                }
            }
            core_->cv.notify_all();
        }
        core_->credential_gate_cv.notify_all();

        // 会话删除只消耗关闭总预算的剩余部分，且不会等待正在占用的凭据门。
        deleteSessionBestEffort(core_, protocol_version, session_id, close_deadline);
        secureClear(protocol_version);
        secureClear(session_id);

        {
            std::unique_lock<std::mutex> lock(core_->mutex);
            core_->cv.wait_until(lock, close_deadline, [&] {
                return std::all_of(core_->worker_done.begin(), core_->worker_done.end(),
                                   [](bool done) { return done; }) &&
                       core_->listener_thread_done && core_->callbacks_inflight == 0U;
            });
        }

        for(auto& worker : workers_) {
            if(worker.joinable()) {
                worker.join();
            }
        }
        if(listener_thread_.joinable()) {
            listener_thread_.join();
        }

        std::lock_guard<std::mutex> lock(core_->mutex);
        core_->close_complete = true;
        core_->cv.notify_all();
    }

   private:
    std::shared_ptr<TransportCore> core_;
    std::mutex lifecycle_mutex_;
    std::array<std::thread, kPostWorkerCount> workers_;
    std::thread listener_thread_;
};

}  // namespace

std::shared_ptr<IMCPTransport> createStreamableHttpMCPTransport(const MCPStreamableHttpConfig& config,
                                                                const MCPCommonLimits& limits) {
    // 工厂只创建无 I/O 对象；静态配置校验由 MCPClient 构造路径统一完成。
    return std::make_shared<StreamableHttpMCPTransport>(config, limits);
}

#if defined(AISDK_MCP_TESTING)
std::shared_ptr<IMCPTransport> createStreamableHttpMCPTransportForTest(const MCPStreamableHttpConfig& config,
                                                                       const MCPCommonLimits& limits,
                                                                       MCPHttpTestOverrides overrides) {
    // 测试覆盖仍需有界且拒绝控制字节，避免测试缝绕过请求头和 libcurl 字符串边界。
    if(overrides.ca_pem.size() > limits.max_message_bytes) {
        throw std::invalid_argument("MCP HTTP 测试 CA 超过消息大小上限");
    }
    if(overrides.resolve_entries.size() > limits.max_pending_messages) {
        throw std::invalid_argument("MCP HTTP 测试解析条目数量超过上限");
    }
    for(const auto& entry : overrides.resolve_entries) {
        if(entry.empty() || entry.size() > limits.max_error_text_bytes || entry.find('\0') != std::string::npos ||
           entry.find_first_of("\r\n") != std::string::npos) {
            throw std::invalid_argument("MCP HTTP 测试解析条目格式非法");
        }
    }
    return std::make_shared<StreamableHttpMCPTransport>(config, limits, std::move(overrides));
}
#endif

}  // namespace detail
}  // namespace aiSDK
