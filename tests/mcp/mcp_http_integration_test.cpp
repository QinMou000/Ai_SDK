#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WS2tcpip.h>
#include <WinSock2.h>
#include <Windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "McpTlsTestCertificates.h"
#include "McpTlsTestServer.h"
#include "mcp/MCPClient.h"
#include "mcp/detail/MCPHttpTestFactory.h"

namespace {

using namespace std::chrono_literals;

// 测试合同 A：范围、隔离与可重复性。
// A01. 本文件只从 MCPClient 公开入口验证 Streamable HTTP，不直接调用生产 Core 私有方法。
// A02. 普通 HTTP 用例只绑定字面量回环地址，不向局域网或公网建立连接。
// A03. TLS 用例通过测试专用名称解析覆盖连接回环端口，不修改系统 hosts 文件。
// A04. 每个用例拥有独立 Server 实例、端口和 Client，状态不能跨用例泄漏。
// A05. Server 监听端口由操作系统分配，避免固定端口与本机服务冲突。
// A06. 所有等待都由 Client 或夹具的有界关闭流程控制，不允许无限测试超时。
// A07. 测试不读取用户配置文件、系统凭据或外部 MCP Server 配置。
// A08. 固定凭据只存在于当前测试进程内，并且不写入任何持久化文件。
// A09. 代理陷阱只观察连接计数，不解析或保存可能包含凭据的请求正文。
// A10. 环境变量改动由 RAII 守卫恢复，异常退出路径同样执行还原。
// A11. 代理测试同时覆盖变量名大小写，避免平台差异造成遗漏。
// A12. NO_PROXY 在陷阱作用域内清除，防止回环特例掩盖代理读取行为。
// A13. 原环境快照在作用域外再次比较，确保测试自身没有污染进程状态。
// A14. HTTP 夹具只实现当前用例需要的协议分支，不冒充完整 MCP Server。
// A15. TLS 夹具只实现初始化、监听降级和关闭所需的最小交互。
// A16. 测试错误文本使用固定中文，不依赖 libcurl 或操作系统动态错误详情。
// A17. JSON 预言机只检查稳定字段，不比较无序对象的序列化文本。
// A18. 请求摘要仅用于断言失败诊断，不作为测试通过的唯一依据。
// A19. 线程和 Socket 都由夹具对象所有，析构前必须显式进入停止流程。
// A20. Client close 在 Server 析构前执行，避免把测试端提前断链误判为正常关闭。
// A21. 故障用例仍调用 close，验证失败后的资源回收与幂等终态。
// A22. 资源测试先预热运行时，排除动态库首次加载产生的一次性句柄。
// A23. 资源样本只在一轮 Server 和 Client 均销毁后采集。
// A24. Windows 使用进程句柄总量作为可重复的资源预言机。
// A25. POSIX 使用 /proc/self/fd 枚举作为文件描述符资源预言机。
// A26. 缺少平台资源预言机时明确失败，不把未验证分支标记为通过。
// A27. 二十五轮重复次数用于识别线性增长，同时控制本地测试耗时。
// A28. 最终资源容差只覆盖运行时瞬时活动，不能掩盖每轮一个资源的泄漏。
// A29. 严格单调增长单独断言，补充最终值可能偶然回落的单点判断。
// A30. 测试专用活动计数器记录请求、SSE Worker 与 Session 三类所有权。
// A31. 每轮关闭后三个活动计数必须同时归零，不能只依赖进程级总量。
// A32. 测试覆盖结构通过 detail 工厂注入，公开生产 API 不暴露测试开关。
// A33. 测试 CA 只传入当前 Transport，不安装到系统信任库。
// A34. 测试 DNS 解析只绑定当前 Session，不改变其他网络库解析结果。
// A35. 普通回环 HTTP 必须显式设置 allow_loopback_http 才能通过配置校验。
// A36. HTTPS 用例不启用回环 HTTP 例外，始终走生产 TLS 安全路径。
// A37. 每个测试使用短连接超时，使网络回归在本地快速、确定地失败。
// A38. 绝对请求超时大于单段超时，为分页和恢复保留统一总预算。
// A39. 关闭超时独立配置，避免请求预算被错误复用为资源回收预算。
// A40. 所有测试数据均为仓库内固定值，重复运行不依赖随机外部状态。

// 测试合同 B：跨平台 Socket 与 HTTP 报文边界。
// B01. SocketHandle 在 Windows 使用 SOCKET，在 POSIX 使用整数描述符。
// B02. 无效句柄常量按平台定义，关闭辅助函数先过滤无效值。
// B03. shutdown 与 close 分离调用，使阻塞读写先被唤醒再释放句柄。
// B04. Windows 夹具在创建 Socket 前完成 WSAStartup。
// B05. Windows 夹具析构时调用 WSACleanup，与构造成功次数配对。
// B06. POSIX 的 SocketRuntime 不引入全局网络初始化副作用。
// B07. sendAll 处理短写，不能假设一次 send 覆盖完整响应。
// B08. 单次发送块限制为十六 KiB，避免 size_t 到 int 的窄化溢出。
// B09. 任意 send 失败都终止当前夹具连接，不伪造部分成功响应。
// B10. 请求读取先寻找 CRLF 双空行，明确区分 Header 与正文。
// B11. Header 聚合设置六十四 KiB 上限，防止异常 Client 无限占用测试内存。
// B12. recv 返回零或错误时按读取失败处理，不能把截断报文解析为完整请求。
// B13. 请求行必须同时包含 method 与 path，缺失任一字段即拒绝。
// B14. HTTP 版本只为完成请求行解析而读取，不参与 MCP 业务分支选择。
// B15. 每条 Header 去除末尾 CR，保持 Windows 与 POSIX 文本解析一致。
// B16. Header 名称转换为 ASCII 小写，验证时采用 HTTP 不区分大小写语义。
// B17. Header 值只移除字段分隔符后的前导空格或制表符。
// B18. 不含冒号的 Header 行被忽略，夹具不会越界截取字段。
// B19. Content-Length 缺失时正文长度按零处理，适配 GET 与 DELETE。
// B20. Content-Length 转换失败会终止当前连接，避免测试端接受歧义长度。
// B21. 已随 Header 读取到的正文前缀会被保留，不重复从 Socket 获取。
// B22. 正文不足时继续 recv，直到达到声明的 Content-Length。
// B23. 正文超过声明长度时裁剪，避免下一个流水化请求混入当前 JSON。
// B24. 夹具响应始终使用 HTTP/1.1 状态行，匹配生产解析路径。
// B25. Content-Type 只在非空时发送，控制响应可验证无正文媒体类型路径。
// B26. 附加响应 Header 按调用方顺序写入，便于初始化注入会话标识。
// B27. 普通响应显式发送 Content-Length，使 Client 能确定正文边界。
// B28. 普通响应显式发送 Connection: close，简化一连接一请求的夹具模型。
// B29. 长期 SSE 响应故意省略 Content-Length，以保持流式连接开放。
// B30. 长期 SSE 首个注释事件用于触发实际正文活动而不产生 JSON 消息。
// B31. 监听 Socket 仅绑定 INADDR_LOOPBACK，其他网络接口无法访问夹具。
// B32. bind 使用零端口让内核选择空闲端口，随后以 getsockname 读取结果。
// B33. listen backlog 只服务本地并发 POST 与 GET，不用于性能基准。
// B34. SO_REUSEADDR 仅降低本地重跑端口回收影响，不扩大监听地址范围。
// B35. accept 失败时先检查 stopping，关闭期直接退出接收循环。
// B36. 非关闭期的瞬时 accept 失败允许继续，避免单次系统中断结束夹具。
// B37. 每个已接受 Socket 在创建 Worker 前登记到活动集合。
// B38. 活动集合让 stop 能中断所有仍阻塞的连接 Worker。
// B39. finishConnection 在关闭句柄前先移除登记，防止重复关闭复用句柄。
// B40. 所有网络异常都限制在对应 Worker，不得逃逸并终止测试进程。

// 测试合同 C：回环 Server 生命周期与并发所有权。
// C01. ServerOptions 在构造时按值保存，运行期间不允许外部线程修改。
// C02. listener_supported 只控制独立 GET Listener，不能改变 POST 行为。
// C03. tool_call_as_sse 只控制工具结果的响应媒体类型与分帧方式。
// C04. expire_session_on_call 只在 tools/call 分支返回会话失效状态。
// C05. required_token 为空时不要求鉴权 Header，覆盖匿名模式。
// C06. required_token 非空时精确比较固定 Header 值，覆盖逐请求 Provider。
// C07. tool_call_requires_recovery 只启用 POST SSE 断线恢复场景。
// C08. 构造函数在启动 accept 线程前完成端口绑定和端口号读取。
// C09. 端口绑定失败时立即关闭监听 Socket，避免构造异常泄漏句柄。
// C10. Server 析构统一调用幂等 stop，测试无需维护额外清理分支。
// C11. stop 使用原子比较交换保证只有一个线程执行实际回收。
// C12. stop 先设置停止标志，再关闭监听 Socket，建立接收循环退出条件。
// C13. stop 通知 Listener 条件变量，长期 SSE Worker 可在关闭期退出。
// C14. accept 线程先于连接 Worker 回收，确保不再新增 Worker。
// C15. 活动 Socket 在锁内只执行 shutdown，不在锁内执行 Worker join。
// C16. Worker join 位于活动锁外，避免与 finishConnection 形成锁等待环。
// C17. workers_ 只由 accept 线程追加，并在 accept 线程退出后由 stop 遍历。
// C18. accepted_connections 使用原子计数，测试线程可无锁读取连接数量。
// C19. 协议 Header、凭据与 DELETE 观察值分别使用独立原子标志。
// C20. 工具 POST 与恢复 GET 使用独立计数，直接验证不重放不变量。
// C21. 请求摘要受互斥量保护，连接 Worker 可并发记录方法标签。
// C22. POST 标签附加 JSON-RPC method，便于失败时定位握手所处阶段。
// C23. 非对象 POST 正文只记录通用 response 标签，不在诊断中抛解析异常。
// C24. 活动 Socket 列表只保存当前连接，不累积已经完成的历史句柄。
// C25. Listener 条件变量只等待 stopping，不依赖虚假唤醒次数。
// C26. 恢复请求 ID 受专用互斥量保护，POST Worker 与 GET Worker 不竞态。
// C27. Server 成员声明顺序保证 SocketRuntime 覆盖全部 Socket 生命周期。
// C28. stopping 初始为 false，构造成功后 accept 线程立即可观察一致状态。
// C29. listener_ 在关闭后重置为无效值，使重复清理保持安全。
// C30. 单个连接只调用一次 finishConnection，异常与正常路径最终汇合。
// C31. 鉴权失败响应发送后立即结束处理，未授权请求不能触发协议状态变更。
// C32. credential_observed 只在值精确匹配后置位，存在 Header 不等于鉴权成功。
// C33. method 分派只接受 POST、GET 与 DELETE，其他方法明确返回 405。
// C34. DELETE 观察标志在发送响应前置位，确保关闭连接异常不抹除请求事实。
// C35. handleConnection 捕获关闭导致的网络异常，资源释放仍由统一尾部执行。
// C36. Server 不主动重试任何连接，所有恢复行为必须来自 Client 请求。
// C37. Server 不共享连接状态，会话语义只通过固定 Header 值验证。
// C38. 每个 Worker 只拥有一个 Socket 值，不跨线程传递正文缓冲。
// C39. 测试不启用 HTTP keep-alive，连接计数可直接反映物理请求次数下界。
// C40. Server 统计信息仅用于测试观察，不参与生成后续协议响应。

// 测试合同 D：JSON-RPC 与 MCP 交互预言机。
// D01. initialize 响应复用请求 ID，建立精确 JSON-RPC 关联。
// D02. initialize 固定协商协议版本 2025-11-25。
// D03. initialize 声明 Tools 能力和 listChanged，用于覆盖目录能力解析。
// D04. initialize 返回固定 Server 名称与版本，避免依赖外部元数据。
// D05. 初始化成功响应携带唯一 MCP-Session-Id。
// D06. 初始化之前不要求协议版本和会话 Header，符合握手阶段边界。
// D07. 初始化之后的 POST 与 GET 必须同时携带协议版本和会话 Header。
// D08. observeProtocolHeaders 只在两个字段均精确匹配时置位。
// D09. notifications/initialized 返回 202，不生成不存在的 JSON-RPC 响应。
// D10. ping 返回空 result 对象，验证最小请求响应闭合。
// D11. 首次 tools/list 返回 inspect 工具和固定下一页游标。
// D12. 带 cursor 的 tools/list 返回 second 工具并终止分页。
// D13. 分页结果名称稳定，Client 合并顺序可由集成用例直接断言。
// D14. 工具 inputSchema 固定为对象，避免 Schema 错误干扰传输场景。
// D15. 工具 content 返回中文文本，覆盖 UTF-8 正文传输。
// D16. structuredContent 原样回显 arguments，验证 JSON 对象没有二次编码。
// D17. 工具结果显式设置 isError=false，覆盖业务成功字段解析。
// D18. 工具 404 只在会话已经建立且选项启用时返回。
// D19. 会话 404 的错误正文不作为公开异常断言依据。
// D20. Client 必须把已提交工具的会话失效提升为 OutcomeUnknown。
// D21. OutcomeUnknown 的 causeCode 必须保留 SessionExpired。
// D22. 会话失效后 Client 主状态必须进入 Faulted。
// D23. 失败后 close 仍需进入 Closed，不能停留在 Faulted。
// D24. 普通工具 JSON 响应使用 application/json 并携带 charset 参数。
// D25. POST SSE 工具响应使用 text/event-stream 并携带 charset 参数。
// D26. SSE data 字段包含完整 JSON-RPC 对象，不依赖多 data 行拼接。
// D27. SSE id 字段为后续恢复提供明确游标。
// D28. 无恢复场景的 SSE 直接发送终局工具响应。
// D29. 恢复场景的首次 SSE 只发送 progress 通知后关闭连接。
// D30. progress 通知不包含工具请求 ID，不能错误完成前台调用。
// D31. 恢复 GET 必须携带首次事件的 Last-Event-ID。
// D32. Server 仅在游标精确为 post-recovery-1 时发送终局结果。
// D33. 恢复终局结果复用原始工具请求 ID，而非 GET 的内部标识。
// D34. 恢复结果 structuredContent 标记 recovered=true，形成明确预言机。
// D35. 恢复完成后工具 POST 计数必须保持一，证明没有副作用重放。
// D36. 恢复 GET 计数必须为一，证明 Client 没有重复续接同一游标。
// D37. 独立 Listener 405 只改变 Listener 状态，不影响 ping、list 与 call。
// D38. 独立 Listener 支持时保持连接，前台 POST 仍由其他连接完成。
// D39. 未识别 POST method 返回 202，用于兼容 Client 的控制通知。
// D40. JSON 响应辅助函数统一构造 jsonrpc、id 与 result 三个稳定字段。

// 测试合同 E：凭据、代理与 HTTP 安全边界。
// E01. FixedCredentialProvider 按值持有测试 Token，不借用临时字符串。
// E02. Provider 每次调用递增原子计数，验证逐请求取值而非永久缓存。
// E03. Provider 在取消视图为真时拒绝返回凭据。
// E04. Provider 在截止时间到达后拒绝返回凭据。
// E05. Provider 异常文本不包含 Token 内容。
// E06. FixedHeader 测试使用非 Authorization 名称覆盖自定义 Header 路径。
// E07. Server 精确查找小写后的 x-test-token，验证 Header 名称大小写无关。
// E08. Server 对错误或缺失 Token 返回 401，不继续解析 MCP method。
// E09. 正常流程观察到凭据后仍继续验证协议版本与会话 Header。
// E10. connect、initialized、Listener GET、ping、list、call 与 DELETE 都应按需取凭据。
// E11. Provider 调用下界而非精确次数，允许 Listener 调度造成合法额外获取。
// E12. DELETE 观察值证明 close 尝试释放已建立的远端会话。
// E13. 代理陷阱把所有常见代理变量指向可计数回环 Server。
// E14. 代理陷阱自身不要求鉴权，任何意外代理连接都会被可靠计数。
// E15. 生产 Session 显式禁用环境代理时，陷阱连接数必须保持零。
// E16. 代理连接断言在陷阱析构后执行，避免遗漏稍晚到达的连接。
// E17. HTTP 回环 Server 与代理陷阱使用不同端口，连接来源不会混淆。
// E18. 环境恢复断言覆盖值恢复和原先未设置两种状态。
// E19. 测试不会打印原始环境变量值，失败信息只说明恢复不一致。
// E20. HTTP 配置只允许字面量 127.0.0.1 的明文例外。
// E21. 明文例外不允许通过名称解析把任意主机伪装成回环地址。
// E22. TLS 配置始终使用 https scheme，不依赖 allow_loopback_http。
// E23. Endpoint 路径固定为 /mcp，Server 可拒绝意外路径扩展。
// E24. connect_timeout 小于请求总预算，连接失败不会耗尽所有协议阶段。
// E25. credential_timeout 小于请求超时，Provider 不应支配整个操作预算。
// E26. stream_idle_timeout 为长期连接提供独立空闲故障边界。
// E27. max_reconnect_attempts 有限，恢复失败不会无限创建 GET。
// E28. Session ID 由初始化响应产生，测试不在配置中预置会话秘密。
// E29. Server 不回显凭据，工具结果和诊断摘要中都不含 Token。
// E30. HTTP 错误正文固定且不携带请求 Header，避免测试诱导秘密泄漏。
// E31. 测试只断言闭合错误枚举，不依赖底层 TLS 或 Socket 错误文本。
// E32. 生产 Transport 覆盖的代理禁用行为在普通 HTTP 与 TLS 各验证一次。
// E33. TLS 代理陷阱与正确主机名控制用例位于同一环境守卫作用域。
// E34. 正确 TLS 连接成功时代理计数仍为零，排除“失败所以未走代理”的假阳性。
// E35. 代理守卫不可复制，避免两个析构对象以错误顺序恢复同一变量。
// E36. 环境访问按平台使用安全 API，返回值立即复制到 std::string。
// E37. Windows _dupenv_s 分配的内存由读取辅助函数立即释放。
// E38. POSIX getenv 的借用指针不会保存到环境快照之外。
// E39. writeEnvironment 能恢复空值与非空值，不把缺失变量误写为空字符串语义。
// E40. 安全用例不提供关闭证书校验或允许重定向的测试逃生开关。

// 测试合同 F：Listener、SSE 与断线恢复。
// F01. 初始化完成后 Client 默认尝试建立独立 GET Listener。
// F02. Server 返回 405 时 ListenerState 必须变为 Unsupported。
// F03. 405 是能力降级而非 Client 故障，主状态必须保持 Ready。
// F04. 降级后 ping 必须完成，证明前台 POST 不依赖 Listener 存活。
// F05. 降级后 tools/list 必须完成全部分页。
// F06. 降级后 tools/call 必须返回结构化结果。
// F07. 降级后 close 仍需发送 DELETE 清理会话。
// F08. 支持 Listener 时 Server 先发送成功 Header 和注释事件。
// F09. Client 观察到成功流后 ListenerState 必须进入 Listening。
// F10. Listener 长期占用独立连接，不占用公开操作在途名额。
// F11. Listener 活跃期间 listTools 仍可发出前台 POST。
// F12. Listener 活跃期间 callTool 仍可接收 POST SSE 结果。
// F13. 并存用例要求至少四条物理连接，覆盖握手、Listener 和前台操作。
// F14. Listener 注释事件不应触发 JSON-RPC 消息解析。
// F15. Listener 等待条件只由 Server 停止触发，不自行伪造 EOF。
// F16. Client close 应取消 Listener，使 Server Worker 能离开阻塞等待。
// F17. POST SSE 的事件游标与 Listener 游标属于不同恢复域。
// F18. 工具 POST 首次断线后不得重发原始 POST。
// F19. POST SSE 只有在收到非空事件 ID 后才具备 GET 恢复依据。
// F20. 恢复 GET 的 Header 必须带原 POST SSE 事件 ID。
// F21. 恢复 GET 不携带新的工具调用正文，避免副作用重复执行。
// F22. 恢复响应通过原 JSON-RPC ID 完成既有前台等待者。
// F23. 首次 progress 通知可以先于终局响应到达而不结束调用。
// F24. 恢复结果文本与普通结果不同，测试能确认实际走过恢复分支。
// F25. 恢复结构字段与文本字段同时断言，避免仅凭单一标志假通过。
// F26. toolCallPostCount 直接观察 Server 收到的请求，不依赖 Client 内部计数。
// F27. recoveryGetCount 直接观察正确游标分支被命中。
// F28. Listener GET 若无支持选项必须立即 405，不保持悬挂连接。
// F29. Listener GET 若支持则使用 text/event-stream 媒体类型。
// F30. POST SSE 同样使用 text/event-stream，但生命周期由单次调用控制。
// F31. SSE data 中的中文 JSON 必须保持 UTF-8 完整性。
// F32. SSE event 以双换行结束，覆盖标准事件分隔规则。
// F33. Last-Event-ID 名称按小写映射查找，兼容 HTTP Header 大小写。
// F34. 错误游标不会误进入恢复成功分支。
// F35. Listener 的 GET 也必须携带初始化后的协议 Header。
// F36. 恢复 GET 同样必须携带会话 Header，不能脱离当前会话续接。
// F37. Server 停止通知会唤醒所有 Listener Worker，避免析构永久等待。
// F38. Listener 与恢复请求分别计数和处理，测试不会混淆两类 GET。
// F39. 前台工具结果确定后 close 不得把成功改写成取消或结果未知。
// F40. 所有 SSE 场景都通过真实 Socket 字节流覆盖解码边界。

// 测试合同 G：TLS 信任、主机名与测试注入。
// G01. TLS Server 证书模式在构造时固定为自签名或测试根签名。
// G02. 自签名证书的有效期和主机名正确，失败应来自信任链而非其他条件。
// G03. 自签名用例不向 Transport 注入测试根 CA。
// G04. 自签名连接必须映射为 TransportFailure。
// G05. 自签名握手失败后 Client 主状态必须进入 Faulted。
// G06. 自签名故障后的 close 必须安全完成资源回收。
// G07. 正确控制用例使用由固定测试根签发的 Server 证书。
// G08. 正确控制用例只把根 CA PEM 传给测试 Transport 实例。
// G09. 正确控制用例使用证书 SAN 中的 mcp.test.local 名称。
// G10. 名称解析覆盖把该 DNS 名映射到 127.0.0.1 和动态端口。
// G11. TLS Endpoint 仍保留 DNS 名，主机名校验不能被 IP 地址替代。
// G12. 正确根 CA 与正确主机名必须完成真实 TLS 握手。
// G13. TLS 初始化成功后 Client 状态必须为 Ready。
// G14. 最小 TLS Server 对 Listener 返回 405，Client 应正常降级。
// G15. TLS 控制用例关闭后 Client 状态必须为 Closed。
// G16. TLS 控制用例同时放置环境代理陷阱，验证安全选项未因 CA 注入改变。
// G17. TLS 控制用例的代理连接数必须为零。
// G18. TLS 控制用例结束后必须恢复全部代理环境变量。
// G19. 错误主机名用例复用同一受信测试根，隔离主机名校验因素。
// G20. 错误主机名仍解析到同一回环 Server，网络可达性不是失败原因。
// G21. 错误主机名不在 Server 证书 SAN 中，握手必须被拒绝。
// G22. 错误主机名失败同样映射 TransportFailure。
// G23. 错误主机名故障后 Client 必须进入 Faulted。
// G24. 错误主机名故障后 close 必须收敛，不留下 TLS Session。
// G25. 测试覆盖不允许设置 verify_peer=false。
// G26. 测试覆盖不允许设置 verify_host=false。
// G27. 测试覆盖只提供 CA PEM、名称解析和活动计数三类受控输入。
// G28. CA PEM 复制到当前请求所需的 libcurl Blob，不引用临时缓冲。
// G29. 名称解析项包含主机、端口与回环地址，避免影响其他目标。
// G30. TLS Server 启动失败会显式使当前用例失败，不允许跳过安全测试。
// G31. 测试根 CA 不写入 Windows 证书库或 POSIX 系统 CA 目录。
// G32. TLS 私钥只由测试 Server 使用，Client 测试接口无法访问私钥。
// G33. 自签名、正确链和错误主机三组用例形成证书验证控制矩阵。
// G34. 正确链成功证明失败用例不是 TLS Server 普遍不可用。
// G35. 自签名失败证明 Client 没有暗中接受任意 Server 证书。
// G36. 错误主机失败证明受信证书链不能绕过名称约束。
// G37. 代理零连接证明 TLS 请求没有向环境代理泄露 Endpoint 或凭据。
// G38. TLS 用例不依赖公网 DNS，离线本地验证结果保持确定。
// G39. TLS 用例不依赖系统时间之外的外部证书状态服务。
// G40. TLS 安全断言只使用闭合状态与错误码，不暴露证书私密材料。

// 测试合同 H：公开状态、断言与资源收敛。
// H01. 正常 connect 返回后 ClientState 必须为 Ready。
// H02. Listener 支持状态通过 listenerState 单独断言，不从主状态推测。
// H03. listTools 返回值保存在当前用例栈内，不跨 Client 生命周期使用。
// H04. callTool 使用同一 Client 签发的 Catalog，避免目录归属错误干扰场景。
// H05. 工具参数包含中文，验证序列化、Socket 和解析端到端编码一致。
// H06. structuredContent 断言直接访问原始 JSON 字段，不依赖文本 content。
// H07. 协议 Header 观察值证明初始化后请求带有版本和会话信息。
// H08. 凭据观察值证明 Provider 返回内容实际进入对应请求 Header。
// H09. DELETE 观察值证明 close 触发远端会话清理请求。
// H10. 所有成功用例显式调用 close，而非只依赖析构。
// H11. 所有错误用例在 catch 后继续断言 Client 终态。
// H12. 预期 MCPException 的用例不接受任意异常类型替代。
// H13. OutcomeUnknown 用例同时断言外层错误、根因与失败时状态。
// H14. 未抛异常的失败路径通过 FAIL 明确终止，避免静默通过。
// H15. Listener 并存用例断言响应文本、结构字段和连接数三个维度。
// H16. 恢复用例断言结果、POST 次数和 GET 次数三个独立预言机。
// H17. TLS 正确控制用例断言 Ready、Unsupported 与 Closed 状态序列。
// H18. 资源轮次中的 Server 位于 lambda 局部作用域，轮次结束立即析构。
// H19. 资源轮次中的 Transport 由 Client 独占并随 Client 析构释放。
// H20. 测试活动计数器跨轮次共享，只观察当前活动数量而非累计次数。
// H21. active_requests 在 close 返回后必须为零。
// H22. active_sse_workers 在 close 返回后必须为零。
// H23. active_sessions 在 close 返回后必须为零。
// H24. 每轮还断言 ClientState::Closed，避免只看内部计数遗漏状态错误。
// H25. 预热轮次执行完整 connect 与 close，与正式轮次路径一致。
// H26. 基线在预热轮次 Server 析构后读取，避免包含监听 Socket。
// H27. samples 预留二十五项，测试自身不会因反复扩容形成资源噪声。
// H28. 每轮样本在 run_round 返回后读取，所有局部对象已经离开作用域。
// H29. 严格增长判断从第二个样本开始，避免索引下溢。
// H30. samples 非空断言位于访问 back 之前，保证边界安全。
// H31. 最终资源上限只比基线多三个，符合 HTTP 运行时允许的瞬时波动。
// H32. 资源测试不解析操作系统句柄类型，保持跨平台断言口径一致。
// H33. Server Worker 在 stop 中全部 join，活动线程不能逃出用例作用域。
// H34. Client 后台 Listener 与 POST Worker 在 close 中回收，计数器负责佐证。
// H35. 失败诊断中的 requestSummary 只列方法，不包含 Header 或正文。
// H36. 测试断言信息使用简体中文，便于本地失败直接定位业务边界。
// H37. 测试不依赖执行顺序，每个用例都独立建立所需初始状态。
// H38. 测试不使用 sleep 猜测协议完成，公开同步操作返回即作为线性化点。
// H39. 唯一长期等待由 Listener Server 条件变量承担，并由 stop 明确唤醒。
// H40. 文件末尾关闭匿名命名空间，所有夹具符号保持测试编译单元私有。
// H41. 夹具辅助函数只服务断言准备，不绕过 Client 的配置与状态校验。
// H42. 每条安全失败用例都有同条件成功控制或独立状态预言机，避免单因子假阳性。
// H43. 本文件验证传输集成，不重复单元测试已经覆盖的全部 JSON Schema 边界。
// H44. 所有本地验证都可离线运行，网络访问范围由回环绑定和解析覆盖共同限定。

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

void closeSocket(SocketHandle socket) noexcept {
    if(socket == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

void shutdownSocket(SocketHandle socket) noexcept {
    if(socket == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    shutdown(socket, SD_BOTH);
#else
    shutdown(socket, SHUT_RDWR);
#endif
}

// currentProcessResourceCount 与 stdio 集成测试使用同一平台原生口径。
// 每轮会先销毁测试 Server，避免把夹具自身的连接 Worker 误判为 Transport 泄漏。
std::size_t currentProcessResourceCount() {
#ifdef _WIN32
    DWORD handle_count = 0U;
    if(!GetProcessHandleCount(GetCurrentProcess(), &handle_count)) {
        throw std::runtime_error("无法读取当前进程句柄数量");
    }
    return static_cast<std::size_t>(handle_count);
#else
    const std::filesystem::path fd_directory("/proc/self/fd");
    if(!std::filesystem::is_directory(fd_directory)) {
        throw std::runtime_error("当前 POSIX 平台缺少 /proc/self/fd 资源预言机");
    }
    return static_cast<std::size_t>(
        std::distance(std::filesystem::directory_iterator(fd_directory), std::filesystem::directory_iterator{}));
#endif
}

// SocketRuntime 在 Windows 初始化一次 Winsock；POSIX 构造和析构保持无操作。
// 每个测试 Server 持有独立实例，失败会在监听端口创建前立即终止用例。
class SocketRuntime {
   public:
    SocketRuntime() {
#ifdef _WIN32
        WSADATA data{};
        if(WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("初始化本地 HTTP 测试网络失败");
        }
#endif
    }

    ~SocketRuntime() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

std::string asciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

std::optional<std::string> readEnvironment(const char* name) {
#ifdef _WIN32
    char* raw_value = nullptr;
    std::size_t value_size = 0U;
    if(_dupenv_s(&raw_value, &value_size, name) != 0 || raw_value == nullptr) {
        return std::nullopt;
    }
    std::string value(raw_value);
    std::free(raw_value);
    return value;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
#endif
}

void writeEnvironment(const char* name, const std::optional<std::string>& value) {
#ifdef _WIN32
    _putenv_s(name, value.has_value() ? value->c_str() : "");
#else
    if(value.has_value()) {
        setenv(name, value->c_str(), 1);
    } else {
        unsetenv(name);
    }
#endif
}

using EnvironmentSnapshot = std::map<std::string, std::optional<std::string>>;

EnvironmentSnapshot captureProxyEnvironment() {
    EnvironmentSnapshot snapshot;
    for(const char* name :
        {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY", "no_proxy", "NO_PROXY"}) {
        snapshot.emplace(name, readEnvironment(name));
    }
    return snapshot;
}

// ProxyEnvironmentGuard 把所有大小写代理变量指向本地陷阱，同时清除 NO_PROXY。
// 析构逐项恢复“未设置/原值”状态，保证串行安全测试不污染后续测试进程。
class ProxyEnvironmentGuard {
   public:
    explicit ProxyEnvironmentGuard(std::string proxy_origin) : original_(captureProxyEnvironment()) {
        for(const char* name : {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"}) {
            writeEnvironment(name, proxy_origin);
        }
        writeEnvironment("no_proxy", std::nullopt);
        writeEnvironment("NO_PROXY", std::nullopt);
    }

    ~ProxyEnvironmentGuard() {
        for(const auto& entry : original_) {
            writeEnvironment(entry.first.c_str(), entry.second);
        }
    }

    ProxyEnvironmentGuard(const ProxyEnvironmentGuard&) = delete;
    ProxyEnvironmentGuard& operator=(const ProxyEnvironmentGuard&) = delete;

   private:
    EnvironmentSnapshot original_;
};

void sendAll(SocketHandle socket, const std::string& data) {
    std::size_t offset = 0U;
    while(offset < data.size()) {
        const std::size_t remaining = data.size() - offset;
        const int chunk = static_cast<int>(std::min<std::size_t>(remaining, 16U * 1024U));
#ifdef _WIN32
        const int written = send(socket, data.data() + offset, chunk, 0);
#else
        const int written = static_cast<int>(send(socket, data.data() + offset, static_cast<std::size_t>(chunk), 0));
#endif
        if(written <= 0) {
            throw std::runtime_error("写入本地 HTTP 夹具连接失败");
        }
        offset += static_cast<std::size_t>(written);
    }
}

struct HttpRequest {
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

HttpRequest readRequest(SocketHandle socket) {
    std::string bytes;
    std::array<char, 4096U> buffer{};
    std::size_t header_end = std::string::npos;
    while((header_end = bytes.find("\r\n\r\n")) == std::string::npos) {
#ifdef _WIN32
        const int received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
        const int received = static_cast<int>(recv(socket, buffer.data(), buffer.size(), 0));
#endif
        if(received <= 0) {
            throw std::runtime_error("读取本地 HTTP 请求头失败");
        }
        bytes.append(buffer.data(), static_cast<std::size_t>(received));
        if(bytes.size() > 64U * 1024U) {
            throw std::runtime_error("本地 HTTP 请求头超过测试上限");
        }
    }

    HttpRequest request;
    std::istringstream header_stream(bytes.substr(0U, header_end));
    std::string request_line;
    std::getline(header_stream, request_line);
    if(!request_line.empty() && request_line.back() == '\r') {
        request_line.pop_back();
    }
    std::istringstream request_line_stream(request_line);
    std::string version;
    request_line_stream >> request.method >> request.path >> version;
    if(request.method.empty() || request.path.empty()) {
        throw std::runtime_error("本地 HTTP 请求行非法");
    }

    std::string line;
    while(std::getline(header_stream, line)) {
        if(!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t separator = line.find(':');
        if(separator == std::string::npos) {
            continue;
        }
        std::string value = line.substr(separator + 1U);
        while(!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.erase(value.begin());
        }
        request.headers[asciiLower(line.substr(0U, separator))] = std::move(value);
    }

    std::size_t content_length = 0U;
    const auto content_length_header = request.headers.find("content-length");
    if(content_length_header != request.headers.end()) {
        content_length = static_cast<std::size_t>(std::stoull(content_length_header->second));
    }
    request.body = bytes.substr(header_end + 4U);
    while(request.body.size() < content_length) {
#ifdef _WIN32
        const int received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
        const int received = static_cast<int>(recv(socket, buffer.data(), buffer.size(), 0));
#endif
        if(received <= 0) {
            throw std::runtime_error("读取本地 HTTP 请求正文失败");
        }
        request.body.append(buffer.data(), static_cast<std::size_t>(received));
    }
    request.body.resize(content_length);
    return request;
}

void sendResponse(SocketHandle socket, int status, const std::string& reason, const std::string& content_type,
                  const std::string& body, const std::vector<std::pair<std::string, std::string>>& headers = {}) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << ' ' << reason << "\r\n";
    if(!content_type.empty()) {
        response << "Content-Type: " << content_type << "\r\n";
    }
    for(const auto& header : headers) {
        response << header.first << ": " << header.second << "\r\n";
    }
    response << "Content-Length: " << body.size() << "\r\nConnection: close\r\n\r\n" << body;
    sendAll(socket, response.str());
}

// ServerOptions 让同一个回环夹具覆盖 GET 405、长期 SSE、POST SSE 和会话 404。
// 每个开关在 Server 构造前固定，运行中不会因 Client 输入改变安全边界。
struct ServerOptions {
    bool listener_supported = false;
    bool tool_call_as_sse = false;
    bool expire_session_on_call = false;
    std::string required_token;
    bool tool_call_requires_recovery = false;
    // 会话失效并发测试可在两个 POST Worker 都到达后统一释放 404 响应。
    bool hold_expiring_calls = false;
    // 非零时覆盖独立 Listener GET 的状态，用于验证运行时不会把其他 2xx 当成成功流。
    int listener_status_override = 0;
    // 覆盖状态为 200 时可同时验证必须是 text/event-stream，而不是任意成功媒体类型。
    std::string listener_content_type_override;
    // 仅用于计时用例：先完成 POST SSE 响应头，再等待测试明确放行终局事件。
    bool hold_tool_sse_after_headers = false;
};

class LoopbackMCPServer {
   public:
    explicit LoopbackMCPServer(ServerOptions options) : options_(std::move(options)) {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if(listener_ == kInvalidSocket) {
            throw std::runtime_error("创建本地 HTTP 监听 Socket 失败");
        }
        int reuse = 1;
#ifdef _WIN32
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if(bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
           listen(listener_, 16) != 0) {
            closeSocket(listener_);
            listener_ = kInvalidSocket;
            throw std::runtime_error("绑定本地 HTTP 回环端口失败");
        }
        sockaddr_in bound{};
#ifdef _WIN32
        int bound_size = sizeof(bound);
#else
        socklen_t bound_size = sizeof(bound);
#endif
        if(getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
            throw std::runtime_error("读取本地 HTTP 回环端口失败");
        }
        port_ = ntohs(bound.sin_port);
        accept_thread_ = std::thread([this] { acceptLoop(); });
    }

    ~LoopbackMCPServer() {
        stop();
    }

    std::string endpoint() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/mcp";
    }

    std::string origin() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    bool protocolHeadersObserved() const noexcept {
        return protocol_headers_observed_.load();
    }

    bool credentialObserved() const noexcept {
        return credential_observed_.load();
    }

    bool deleteObserved() const noexcept {
        return delete_observed_.load();
    }

    std::size_t acceptedConnections() const noexcept {
        return accepted_connections_.load();
    }

    std::size_t toolCallPostCount() const noexcept {
        return tool_call_post_count_.load();
    }

    std::size_t recoveryGetCount() const noexcept {
        return recovery_get_count_.load();
    }

    bool waitForExpiringCalls(std::size_t expected, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(expiring_call_mutex_);
        return expiring_call_cv_.wait_for(lock, timeout, [&] { return expiring_calls_waiting_ >= expected; });
    }

    void releaseExpiringCalls() {
        std::lock_guard<std::mutex> lock(expiring_call_mutex_);
        release_expiring_calls_ = true;
        expiring_call_cv_.notify_all();
    }

    bool waitForToolSseHeaders(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(tool_sse_mutex_);
        return tool_sse_cv_.wait_for(lock, timeout, [this] { return tool_sse_headers_sent_; });
    }

    void releaseToolSse() {
        std::lock_guard<std::mutex> lock(tool_sse_mutex_);
        release_tool_sse_ = true;
        tool_sse_cv_.notify_all();
    }

    std::string requestSummary() const {
        std::lock_guard<std::mutex> lock(request_mutex_);
        std::ostringstream summary;
        for(std::size_t index = 0U; index < request_methods_.size(); ++index) {
            if(index != 0U) {
                summary << ',';
            }
            summary << request_methods_[index];
        }
        return summary.str();
    }

    void stop() noexcept {
        bool expected = false;
        if(!stopping_.compare_exchange_strong(expected, true)) {
            return;
        }
        shutdownSocket(listener_);
        closeSocket(listener_);
        listener_ = kInvalidSocket;
        listener_cv_.notify_all();
        expiring_call_cv_.notify_all();
        tool_sse_cv_.notify_all();
        if(accept_thread_.joinable()) {
            accept_thread_.join();
        }

        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            // finishConnection 会先取得同一锁再关闭，锁内 shutdown 不会命中已复用句柄。
            for(const auto socket : active_sockets_) {
                shutdownSocket(socket);
            }
        }
        for(auto& worker : workers_) {
            if(worker.joinable()) {
                worker.join();
            }
        }
    }

   private:
    void acceptLoop() noexcept {
        while(!stopping_.load()) {
            sockaddr_in peer{};
#ifdef _WIN32
            int peer_size = sizeof(peer);
#else
            socklen_t peer_size = sizeof(peer);
#endif
            const SocketHandle client = accept(listener_, reinterpret_cast<sockaddr*>(&peer), &peer_size);
            if(client == kInvalidSocket) {
                if(stopping_.load()) {
                    return;
                }
                continue;
            }
            ++accepted_connections_;
            {
                std::lock_guard<std::mutex> lock(active_mutex_);
                active_sockets_.push_back(client);
            }
            workers_.emplace_back([this, client] { handleConnection(client); });
        }
    }

    void handleConnection(SocketHandle client) noexcept {
        try {
            const HttpRequest request = readRequest(client);
            recordRequest(request);
            if(!options_.required_token.empty()) {
                const auto token = request.headers.find("x-test-token");
                if(token == request.headers.end() || token->second != options_.required_token) {
                    sendResponse(client, 401, "Unauthorized", "text/plain", "需要凭据");
                    finishConnection(client);
                    return;
                }
                credential_observed_.store(true);
            }
            if(request.method == "POST") {
                handlePost(client, request);
            } else if(request.method == "GET") {
                handleGet(client, request);
            } else if(request.method == "DELETE") {
                delete_observed_.store(true);
                sendResponse(client, 200, "OK", "", "");
            } else {
                sendResponse(client, 405, "Method Not Allowed", "text/plain", "不支持的方法");
            }
        } catch(...) {
            // 网络取消和 Client close 会中断读取；夹具不得把它升级为进程级异常。
        }
        finishConnection(client);
    }

    void handlePost(SocketHandle client, const HttpRequest& request) {
        const nlohmann::json message = nlohmann::json::parse(request.body);
        const std::string method = message.value("method", std::string{});
        if(method == "initialize") {
            const nlohmann::json result = {
                {"protocolVersion", "2025-11-25"                                     },
                {"capabilities",    {{"tools", {{"listChanged", true}}}}             },
                {"serverInfo",      {{"name", "HTTP 回环夹具"}, {"version", "1"}}}
            };
            sendJson(client, message.at("id"), result,
                     {
                         {"MCP-Session-Id", "session-test"}
            });
            return;
        }

        observeProtocolHeaders(request);
        if(method == "notifications/initialized" || method == "notifications/cancelled" || method.empty()) {
            sendResponse(client, 202, "Accepted", "", "");
            return;
        }
        if(method == "ping") {
            sendJson(client, message.at("id"), nlohmann::json::object());
            return;
        }
        if(method == "tools/list") {
            const nlohmann::json tool = {
                {"name",           "echo"                   },
                {"description",    "HTTP 回环回显工具"},
                {"inputSchema",    {{"type", "object"}}     },
                {"x-http-fixture", true                     }
            };
            sendJson(client, message.at("id"),
                     {
                         {"tools", nlohmann::json::array({tool})}
            });
            return;
        }
        if(method == "tools/call") {
            ++tool_call_post_count_;
            if(options_.expire_session_on_call) {
                if(options_.hold_expiring_calls) {
                    std::unique_lock<std::mutex> lock(expiring_call_mutex_);
                    ++expiring_calls_waiting_;
                    expiring_call_cv_.notify_all();
                    expiring_call_cv_.wait(lock, [&] { return release_expiring_calls_ || stopping_.load(); });
                }
                sendResponse(client, 404, "Not Found", "text/plain", "会话不存在");
                return;
            }
            const nlohmann::json result = {
                {"content",           nlohmann::json::array({{{"type", "text"}, {"text", "HTTP 调用成功"}}})},
                {"structuredContent", {{"arguments", message.at("params").at("arguments")}}                     },
                {"isError",           false                                                                     }
            };
            if(options_.tool_call_requires_recovery) {
                {
                    std::lock_guard<std::mutex> lock(recovery_mutex_);
                    recovery_request_id_ = message.at("id");
                }
                // 首条 POST SSE 只提供可恢复游标和普通通知，随后确定关闭且绝不包含终局响应。
                const nlohmann::json notification = {
                    {"jsonrpc", "2.0"                   },
                    {"method",  "notifications/progress"},
                    {"params",  {{"progress", 1}}       }
                };
                const std::string event = "id: post-recovery-1\ndata: " + notification.dump() + "\n\n";
                sendResponse(client, 200, "OK", "text/event-stream; charset=utf-8", event);
            } else if(options_.tool_call_as_sse) {
                const nlohmann::json response = {
                    {"jsonrpc", "2.0"           },
                    {"id",      message.at("id")},
                    {"result",  result          }
                };
                const std::string event = "id: post-event-1\ndata: " + response.dump() + "\n\n";
                if(options_.hold_tool_sse_after_headers) {
                    // Header 与空闲流之间的屏障验证 Client 已切换到 SSE 的绝对操作上限。
                    sendAll(client,
                            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nConnection: "
                            "close\r\n\r\n");
                    std::unique_lock<std::mutex> lock(tool_sse_mutex_);
                    tool_sse_headers_sent_ = true;
                    tool_sse_cv_.notify_all();
                    tool_sse_cv_.wait(lock, [this] { return release_tool_sse_ || stopping_.load(); });
                    if(stopping_.load()) {
                        return;
                    }
                    lock.unlock();
                    sendAll(client, event);
                } else {
                    sendResponse(client, 200, "OK", "text/event-stream; charset=utf-8", event);
                }
            } else {
                sendJson(client, message.at("id"), result);
            }
            return;
        }
        sendResponse(client, 202, "Accepted", "", "");
    }

    void recordRequest(const HttpRequest& request) {
        std::string label = request.method;
        if(request.method == "POST") {
            const auto message = nlohmann::json::parse(request.body, nullptr, false);
            if(message.is_object()) {
                label += ":" + message.value("method", std::string("response"));
            }
        }
        std::lock_guard<std::mutex> lock(request_mutex_);
        request_methods_.push_back(std::move(label));
    }

    void handleGet(SocketHandle client, const HttpRequest& request) {
        observeProtocolHeaders(request);
        const auto last_event_id = request.headers.find("last-event-id");
        if(options_.tool_call_requires_recovery && last_event_id != request.headers.end() &&
           last_event_id->second == "post-recovery-1") {
            ++recovery_get_count_;
            nlohmann::json request_id;
            {
                std::lock_guard<std::mutex> lock(recovery_mutex_);
                request_id = recovery_request_id_;
            }
            const nlohmann::json result = {
                {"content",           nlohmann::json::array({{{"type", "text"}, {"text", "SSE 恢复成功"}}})},
                {"structuredContent", {{"recovered", true}}                                                    },
                {"isError",           false                                                                    }
            };
            const nlohmann::json response = {
                {"jsonrpc", "2.0"     },
                {"id",      request_id},
                {"result",  result    }
            };
            const std::string event = "id: post-recovery-2\ndata: " + response.dump() + "\n\n";
            sendResponse(client, 200, "OK", "text/event-stream; charset=utf-8", event);
            return;
        }
        if(options_.listener_status_override != 0) {
            // 测试覆盖只改变本地回环夹具的最终 Header，不改变生产 Transport 的安全选项。
            const std::string content_type = options_.listener_content_type_override.empty()
                                                 ? "text/event-stream"
                                                 : options_.listener_content_type_override;
            sendResponse(client, options_.listener_status_override, "Fixture Status", content_type,
                         "测试 Listener 状态");
            return;
        }
        if(!options_.listener_supported) {
            sendResponse(client, 405, "Method Not Allowed", "text/plain", "不支持独立 Listener");
            return;
        }

        // 长期 SSE 不发送 Content-Length；响应头到达后 Client 应立即进入 Listening。
        sendAll(client,
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n: listener-ready\n\n");
        std::unique_lock<std::mutex> lock(listener_mutex_);
        listener_cv_.wait(lock, [this] { return stopping_.load(); });
    }

    void observeProtocolHeaders(const HttpRequest& request) {
        const auto version = request.headers.find("mcp-protocol-version");
        const auto session = request.headers.find("mcp-session-id");
        if(version != request.headers.end() && version->second == "2025-11-25" && session != request.headers.end() &&
           session->second == "session-test") {
            protocol_headers_observed_.store(true);
        }
    }

    void sendJson(SocketHandle client, const nlohmann::json& id, const nlohmann::json& result,
                  const std::vector<std::pair<std::string, std::string>>& headers = {}) {
        const nlohmann::json response = {
            {"jsonrpc", "2.0" },
            {"id",      id    },
            {"result",  result}
        };
        sendResponse(client, 200, "OK", "application/json; charset=utf-8", response.dump(), headers);
    }

    void finishConnection(SocketHandle client) noexcept {
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            const auto iterator = std::find(active_sockets_.begin(), active_sockets_.end(), client);
            if(iterator != active_sockets_.end()) {
                active_sockets_.erase(iterator);
            }
        }
        shutdownSocket(client);
        closeSocket(client);
    }

    SocketRuntime runtime_;
    ServerOptions options_;
    SocketHandle listener_ = kInvalidSocket;
    std::uint16_t port_ = 0U;
    std::atomic<bool> stopping_{false};
    std::atomic<std::size_t> accepted_connections_{0U};
    std::atomic<bool> protocol_headers_observed_{false};
    std::atomic<bool> credential_observed_{false};
    std::atomic<bool> delete_observed_{false};
    std::atomic<std::size_t> tool_call_post_count_{0U};
    std::atomic<std::size_t> recovery_get_count_{0U};
    std::thread accept_thread_;
    std::vector<std::thread> workers_;
    std::mutex active_mutex_;
    std::vector<SocketHandle> active_sockets_;
    std::mutex listener_mutex_;
    std::condition_variable listener_cv_;
    mutable std::mutex request_mutex_;
    std::vector<std::string> request_methods_;
    std::mutex recovery_mutex_;
    nlohmann::json recovery_request_id_;
    std::mutex expiring_call_mutex_;
    std::condition_variable expiring_call_cv_;
    std::size_t expiring_calls_waiting_ = 0U;
    bool release_expiring_calls_ = false;
    std::mutex tool_sse_mutex_;
    std::condition_variable tool_sse_cv_;
    bool tool_sse_headers_sent_ = false;
    bool release_tool_sse_ = false;
};

class FixedCredentialProvider final : public aiSDK::IMCPHttpCredentialProvider {
   public:
    explicit FixedCredentialProvider(std::string value) : value_(std::move(value)) {}

    std::string credentialValue(const aiSDK::MCPHttpCredentialRequestContext& context) override {
        ++calls_;
        if(context.isCancellationRequested() || std::chrono::steady_clock::now() >= context.deadline) {
            throw std::runtime_error("测试凭据请求已经取消");
        }
        return value_;
    }

    std::size_t calls() const noexcept {
        return calls_.load();
    }

   private:
    std::string value_;
    std::atomic<std::size_t> calls_{0U};
};

// BlockingCredentialProvider 为凭据门测试提供确定屏障，并记录 Provider 的真实并发度。
// 它周期性读取协作取消视图，使 close 测试不依赖非协作用户代码这一能力边界。
class BlockingCredentialProvider final : public aiSDK::IMCPHttpCredentialProvider {
   public:
    std::string credentialValue(const aiSDK::MCPHttpCredentialRequestContext& context) override {
        std::unique_lock<std::mutex> lock(mutex_);
        ++calls_;
        ++active_;
        max_active_ = std::max(max_active_, active_);
        cv_.notify_all();

        while(!released_ && !context.isCancellationRequested() && std::chrono::steady_clock::now() < context.deadline) {
            cv_.wait_for(lock, 5ms);
        }
        --active_;
        cv_.notify_all();
        if(context.isCancellationRequested() || std::chrono::steady_clock::now() >= context.deadline) {
            throw std::runtime_error("测试凭据请求已经取消或超时");
        }
        return "gate-token";
    }

    bool waitForCalls(std::size_t expected, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return calls_ >= expected; });
    }

    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }

    std::size_t calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

    std::size_t maxActive() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return max_active_;
    }

   private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t calls_ = 0U;
    std::size_t active_ = 0U;
    std::size_t max_active_ = 0U;
    bool released_ = false;
};

struct PrepareResult {
    bool succeeded = false;
    std::optional<aiSDK::MCPErrorCode> error;
};

// TransportProbe 只保存闭合枚举和 JSON 值，回调本身不递归调用 Transport。
class TransportProbe final {
   public:
    aiSDK::MCPTransportCallbacks callbacks() {
        return {[this](nlohmann::json message) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    messages_.push_back(std::move(message));
                    cv_.notify_all();
                },
                [this](aiSDK::MCPTransportEvent event) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    events_.push_back(event);
                    cv_.notify_all();
                }};
    }

    bool waitForMessageId(std::uint64_t id, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return std::any_of(messages_.begin(), messages_.end(), [&](const nlohmann::json& message) {
                return message.is_object() && message.value("id", std::uint64_t{0U}) == id;
            });
        });
    }

    bool waitForSessionExpired(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return sessionExpiredCountLocked() > 0U; });
    }

    std::size_t sessionExpiredCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sessionExpiredCountLocked();
    }

   private:
    std::size_t sessionExpiredCountLocked() const {
        return static_cast<std::size_t>(
            std::count_if(events_.begin(), events_.end(), [](const aiSDK::MCPTransportEvent& event) {
                return event.type == aiSDK::MCPTransportEventType::SessionExpired;
            }));
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<nlohmann::json> messages_;
    std::vector<aiSDK::MCPTransportEvent> events_;
};

aiSDK::MCPServerConfig makeHttpConfig(const LoopbackMCPServer& server,
                                      aiSDK::MCPHttpCredential credential = aiSDK::MCPAnonymousCredential{}) {
    aiSDK::MCPServerConfig config;
    config.server_id = "local_http";
    config.limits.request_timeout = 2s;
    config.limits.absolute_request_timeout = 8s;
    config.limits.close_timeout = 2s;
    aiSDK::MCPStreamableHttpConfig http;
    http.endpoint = server.endpoint();
    http.allow_loopback_http = true;
    http.credential = std::move(credential);
    http.connect_timeout = 1s;
    http.stream_idle_timeout = 5s;
    http.credential_timeout = 500ms;
    http.max_reconnect_attempts = 2U;
    config.transport = std::move(http);
    return config;
}

std::shared_ptr<aiSDK::IMCPTransport> makeDirectHttpTransport(const aiSDK::MCPServerConfig& config) {
    const auto& http = std::get<aiSDK::MCPStreamableHttpConfig>(config.transport);
    return aiSDK::detail::createStreamableHttpMCPTransportForTest(http, config.limits, {});
}

PrepareResult preparePing(const std::shared_ptr<aiSDK::IMCPTransport>& transport, std::uint64_t dispatch_id,
                          std::chrono::steady_clock::time_point deadline) {
    PrepareResult result;
    try {
        auto prepared = transport->prepareMessage(
            {
                {"jsonrpc", "2.0"      },
                {"id",      dispatch_id},
                {"method",  "ping"     }
        },
            {aiSDK::MCPTransportRequestKind::Ping, dispatch_id, deadline});
        result.succeeded = static_cast<bool>(prepared);
    } catch(const aiSDK::MCPException& exception) {
        result.error = exception.code();
    } catch(...) {
        // 未知异常保持 succeeded=false 且没有闭合错误码，断言会给出确定失败。
    }
    return result;
}

// makeTlsConfig 使用证书 DNS 名构造生产合法 HTTPS Endpoint。
// 名称解析只通过内部测试工厂注入当前 Session，不修改 hosts 或系统 DNS。
aiSDK::MCPServerConfig makeTlsConfig(const aiSDK::test::McpTlsTestServer& server, std::string host) {
    aiSDK::MCPServerConfig config;
    config.server_id = "local_tls";
    config.limits.request_timeout = 2s;
    config.limits.absolute_request_timeout = 8s;
    config.limits.close_timeout = 2s;
    aiSDK::MCPStreamableHttpConfig http;
    http.endpoint = "https://" + host + ":" + std::to_string(server.endpointPort()) + "/mcp";
    http.connect_timeout = 2s;
    http.stream_idle_timeout = 2s;
    http.credential_timeout = 500ms;
    http.max_reconnect_attempts = 1U;
    config.transport = std::move(http);
    return config;
}

std::shared_ptr<aiSDK::IMCPTransport> makeTlsTestTransport(const aiSDK::MCPServerConfig& config,
                                                           const std::string& host, std::uint16_t port,
                                                           bool trust_test_root) {
    aiSDK::detail::MCPHttpTestOverrides overrides;
    if(trust_test_root) {
        overrides.ca_pem = aiSDK::test::mcpTlsTestRootCaPem();
    }
    const auto& http = std::get<aiSDK::MCPStreamableHttpConfig>(config.transport);
    overrides.resolve_entries.push_back(host + ":" + std::to_string(port) + ":127.0.0.1");
    return aiSDK::detail::createStreamableHttpMCPTransportForTest(http, config.limits, std::move(overrides));
}

TEST(MCPHttpInternalTest, 凭据Provider调用严格串行) {
    LoopbackMCPServer server({false, false, false, {}});
    auto provider = std::make_shared<BlockingCredentialProvider>();
    aiSDK::MCPFixedHeaderCredential credential{"X-Test-Token", provider};
    auto config = makeHttpConfig(server, credential);
    std::get<aiSDK::MCPStreamableHttpConfig>(config.transport).credential_timeout = 2s;
    auto transport = makeDirectHttpTransport(config);
    transport->open({[](nlohmann::json) {}, [](aiSDK::MCPTransportEvent) {}}, std::chrono::steady_clock::now() + 2s);

    PrepareResult first;
    PrepareResult second;
    std::thread first_thread([&] { first = preparePing(transport, 1U, std::chrono::steady_clock::now() + 2s); });
    const bool first_entered = provider->waitForCalls(1U, 1s);
    if(!first_entered) {
        provider->release();
        first_thread.join();
        transport->close(std::chrono::steady_clock::now() + 1s);
        FAIL() << "首个 Provider 调用未在限定时间进入";
        return;
    }

    std::thread second_thread([&] { second = preparePing(transport, 2U, std::chrono::steady_clock::now() + 2s); });
    const bool second_entered_while_busy = provider->waitForCalls(2U, 100ms);
    provider->release();
    first_thread.join();
    second_thread.join();

    EXPECT_FALSE(second_entered_while_busy) << "第二个 Provider 不得越过单 Client 串行凭据门";
    EXPECT_TRUE(first.succeeded);
    EXPECT_TRUE(second.succeeded);
    EXPECT_EQ(provider->calls(), 2U);
    EXPECT_EQ(provider->maxActive(), 1U);
    transport->close(std::chrono::steady_clock::now() + 1s);
}

TEST(MCPHttpInternalTest, 凭据门排队受操作绝对截止约束) {
    LoopbackMCPServer server({false, false, false, {}});
    auto provider = std::make_shared<BlockingCredentialProvider>();
    aiSDK::MCPFixedHeaderCredential credential{"X-Test-Token", provider};
    auto config = makeHttpConfig(server, credential);
    std::get<aiSDK::MCPStreamableHttpConfig>(config.transport).credential_timeout = 2s;
    auto transport = makeDirectHttpTransport(config);
    transport->open({[](nlohmann::json) {}, [](aiSDK::MCPTransportEvent) {}}, std::chrono::steady_clock::now() + 2s);

    PrepareResult first;
    PrepareResult second;
    std::thread first_thread([&] { first = preparePing(transport, 1U, std::chrono::steady_clock::now() + 3s); });
    const bool first_entered = provider->waitForCalls(1U, 1s);
    if(!first_entered) {
        provider->release();
        first_thread.join();
        transport->close(std::chrono::steady_clock::now() + 1s);
        FAIL() << "首个 Provider 调用未在限定时间进入";
        return;
    }

    std::mutex completion_mutex;
    std::condition_variable completion_cv;
    bool second_completed = false;
    std::thread second_thread([&] {
        second = preparePing(transport, 2U, std::chrono::steady_clock::now() + 100ms);
        std::lock_guard<std::mutex> lock(completion_mutex);
        second_completed = true;
        completion_cv.notify_all();
    });
    bool completed_before_release = false;
    {
        std::unique_lock<std::mutex> lock(completion_mutex);
        completed_before_release = completion_cv.wait_for(lock, 1s, [&] { return second_completed; });
    }
    provider->release();
    first_thread.join();
    second_thread.join();

    EXPECT_TRUE(completed_before_release) << "排队请求必须自行在绝对截止时间收敛";
    EXPECT_TRUE(first.succeeded);
    ASSERT_TRUE(second.error.has_value());
    EXPECT_EQ(*second.error, aiSDK::MCPErrorCode::RequestTimeout);
    EXPECT_EQ(provider->calls(), 1U) << "排队超时的请求不得调用 Provider";
    transport->close(std::chrono::steady_clock::now() + 1s);
}

TEST(MCPHttpInternalTest, close唤醒并取消凭据门排队者) {
    LoopbackMCPServer server({false, false, false, {}});
    auto provider = std::make_shared<BlockingCredentialProvider>();
    aiSDK::MCPFixedHeaderCredential credential{"X-Test-Token", provider};
    auto config = makeHttpConfig(server, credential);
    std::get<aiSDK::MCPStreamableHttpConfig>(config.transport).credential_timeout = 2s;
    auto transport = makeDirectHttpTransport(config);
    transport->open({[](nlohmann::json) {}, [](aiSDK::MCPTransportEvent) {}}, std::chrono::steady_clock::now() + 2s);

    PrepareResult first;
    PrepareResult second;
    std::atomic_bool second_started{false};
    std::thread first_thread([&] { first = preparePing(transport, 1U, std::chrono::steady_clock::now() + 3s); });
    const bool first_entered = provider->waitForCalls(1U, 1s);
    if(!first_entered) {
        provider->release();
        first_thread.join();
        transport->close(std::chrono::steady_clock::now() + 1s);
        FAIL() << "首个 Provider 调用未在限定时间进入";
        return;
    }

    std::thread second_thread([&] {
        second_started.store(true);
        second = preparePing(transport, 2U, std::chrono::steady_clock::now() + 3s);
    });
    while(!second_started.load()) {
        std::this_thread::yield();
    }
    const bool second_reached_provider = provider->waitForCalls(2U, 50ms);
    transport->close(std::chrono::steady_clock::now() + 1s);
    provider->release();
    first_thread.join();
    second_thread.join();

    EXPECT_FALSE(second_reached_provider);
    ASSERT_TRUE(first.error.has_value());
    ASSERT_TRUE(second.error.has_value());
    EXPECT_EQ(*first.error, aiSDK::MCPErrorCode::OperationCancelled);
    EXPECT_EQ(*second.error, aiSDK::MCPErrorCode::OperationCancelled);
    EXPECT_EQ(provider->calls(), 1U) << "close 必须直接取消排队者，不能让其随后进入 Provider";
}

TEST(MCPHttpInternalTest, 并发会话404只终止一次并拒绝迟到提交) {
    LoopbackMCPServer server({false, false, true, {}, false, true});
    auto config = makeHttpConfig(server);
    TransportProbe probe;
    auto transport = makeDirectHttpTransport(config);
    transport->open(probe.callbacks(), std::chrono::steady_clock::now() + 2s);

    const nlohmann::json initialize = {
        {"jsonrpc", "2.0"                   },
        {"id",      1U                      },
        {"method",  "initialize"            },
        {"params",  nlohmann::json::object()}
    };
    auto initialize_prepared = transport->prepareMessage(
        initialize, {aiSDK::MCPTransportRequestKind::Initialize, 1U, std::chrono::steady_clock::now() + 2s});
    transport->commitPrepared(std::move(initialize_prepared), std::chrono::steady_clock::now() + 2s);
    if(!probe.waitForMessageId(1U, 1s)) {
        transport->close(std::chrono::steady_clock::now() + 1s);
        FAIL() << "直接 Transport 测试必须先建立带 Session ID 的会话";
        return;
    }
    transport->completeInitialization("2025-11-25");

    auto stale_prepared = transport->prepareMessage(
        {
            {"jsonrpc", "2.0" },
            {"id",      99U   },
            {"method",  "ping"}
    },
        {aiSDK::MCPTransportRequestKind::Ping, 99U, std::chrono::steady_clock::now() + 2s});
    std::vector<std::unique_ptr<aiSDK::IMCPPreparedMessage>> calls;
    for(std::uint64_t id = 2U; id < 6U; ++id) {
        const nlohmann::json call = {
            {"jsonrpc", "2.0"                                                      },
            {"id",      id                                                         },
            {"method",  "tools/call"                                               },
            {"params",  {{"name", "echo"}, {"arguments", nlohmann::json::object()}}}
        };
        calls.push_back(transport->prepareMessage(
            call, {aiSDK::MCPTransportRequestKind::CallTool, id, std::chrono::steady_clock::now() + 2s}));
    }
    for(auto& call : calls) {
        transport->commitPrepared(std::move(call), std::chrono::steady_clock::now() + 2s);
    }

    const bool two_requests_active = server.waitForExpiringCalls(2U, 1s);
    server.releaseExpiringCalls();
    if(!two_requests_active) {
        transport->close(std::chrono::steady_clock::now() + 1s);
        FAIL() << "两个固定 POST Worker 未同时到达会话 404 屏障";
        return;
    }
    if(!probe.waitForSessionExpired(1s)) {
        transport->close(std::chrono::steady_clock::now() + 1s);
        FAIL() << "并发 404 未在限定时间发布 SessionExpired";
        return;
    }
    // SessionExpired 回调已经是同一会话的第一胜者；后续断言不依赖任意等待窗口。
    EXPECT_EQ(probe.sessionExpiredCount(), 1U) << "同一会话的并发 404 只能发布一个 SessionExpired";
    EXPECT_EQ(server.toolCallPostCount(), 2U) << "首个 404 必须清空尚未发送的同会话队列";
    try {
        transport->commitPrepared(std::move(stale_prepared), std::chrono::steady_clock::now() + 2s);
        FAIL() << "会话失效前准备的消息不得迟到提交";
    } catch(const aiSDK::MCPException& exception) {
        EXPECT_EQ(exception.code(), aiSDK::MCPErrorCode::SessionExpired);
    }
    const PrepareResult fresh = preparePing(transport, 100U, std::chrono::steady_clock::now() + 1s);
    ASSERT_TRUE(fresh.error.has_value());
    EXPECT_EQ(*fresh.error, aiSDK::MCPErrorCode::SessionExpired);
    transport->close(std::chrono::steady_clock::now() + 1s);
}

TEST(MCPHttpIntegrationTest, Listener返回其他2xx会以协议错误终止连接) {
    ServerOptions options;
    // HTTP 204 是常见控制响应，但绝不能被解释成已建立的 SSE Listener。
    options.listener_status_override = 204;
    LoopbackMCPServer server(std::move(options));
    aiSDK::MCPClient client(makeHttpConfig(server));

    try {
        client.connect();
        FAIL() << "Listener 204 不能让 Client 进入 Ready";
    } catch(const aiSDK::MCPException& exception) {
        EXPECT_EQ(exception.code(), aiSDK::MCPErrorCode::ProtocolViolation);
        EXPECT_EQ(exception.clientStateAtFailure(), aiSDK::MCPClientState::Faulted);
    }
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Faulted);
    client.close();
}

TEST(MCPHttpIntegrationTest, Listener200但错误媒体类型会以协议错误终止连接) {
    ServerOptions options;
    // 状态码正确不足以建立监听；媒体类型是独立的协议前置条件。
    options.listener_status_override = 200;
    options.listener_content_type_override = "application/json; charset=utf-8";
    LoopbackMCPServer server(std::move(options));
    aiSDK::MCPClient client(makeHttpConfig(server));

    try {
        client.connect();
        FAIL() << "Listener 错误 Content-Type 不能让 Client 进入 Ready";
    } catch(const aiSDK::MCPException& exception) {
        EXPECT_EQ(exception.code(), aiSDK::MCPErrorCode::ProtocolViolation);
        EXPECT_EQ(exception.clientStateAtFailure(), aiSDK::MCPClientState::Faulted);
    }
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Faulted);
    client.close();
}

TEST(MCPHttpIntegrationTest, Listener非成功状态映射为闭合错误并终止初始化) {
    struct StatusCase {
        int status;
        aiSDK::MCPErrorCode expected_error;
    };
    const std::array<StatusCase, 9> cases{
        {
         {300, aiSDK::MCPErrorCode::HttpStatusError},
         {400, aiSDK::MCPErrorCode::HttpStatusError},
         {401, aiSDK::MCPErrorCode::AuthenticationRequired},
         {403, aiSDK::MCPErrorCode::AuthenticationRequired},
         {404, aiSDK::MCPErrorCode::SessionExpired},
         {409, aiSDK::MCPErrorCode::HttpStatusError},
         {429, aiSDK::MCPErrorCode::HttpStatusError},
         {500, aiSDK::MCPErrorCode::HttpStatusError},
         {503, aiSDK::MCPErrorCode::HttpStatusError},
         }
    };

    for(const StatusCase& test_case : cases) {
        SCOPED_TRACE("Listener HTTP 状态=" + std::to_string(test_case.status));
        ServerOptions options;
        options.listener_status_override = test_case.status;
        LoopbackMCPServer server(std::move(options));
        aiSDK::MCPClient client(makeHttpConfig(server));

        try {
            client.connect();
            FAIL() << "非 200/405 Listener 响应不能让 Client 进入 Ready";
        } catch(const aiSDK::MCPException& exception) {
            EXPECT_EQ(exception.code(), test_case.expected_error);
            EXPECT_EQ(exception.clientStateAtFailure(), aiSDK::MCPClientState::Faulted);
        }
        EXPECT_EQ(client.state(), aiSDK::MCPClientState::Faulted);
        client.close();
    }
}

TEST(MCPHttpIntegrationTest, GET405降级下逐请求凭据分页调用和DELETE均可用) {
    const EnvironmentSnapshot environment_before = captureProxyEnvironment();
    std::size_t proxy_connections = 0U;
    {
        LoopbackMCPServer proxy_trap({false, false, false, {}});
        ProxyEnvironmentGuard proxy_environment(proxy_trap.origin());
        auto provider = std::make_shared<FixedCredentialProvider>("token-value");
        LoopbackMCPServer server({false, false, false, "token-value"});
        aiSDK::MCPFixedHeaderCredential credential{"X-Test-Token", provider};
        aiSDK::MCPClient client(makeHttpConfig(server, credential));

        ASSERT_NO_THROW(client.connect())
            << "带固定 Header 凭据的 connect 必须完成；已收请求=" << server.requestSummary();
        EXPECT_EQ(client.listenerState(), aiSDK::MCPListenerState::Unsupported);
        ASSERT_NO_THROW(client.ping()) << "405 降级后 ping 必须继续可用";
        std::optional<aiSDK::MCPToolCatalog> catalog;
        ASSERT_NO_THROW(catalog.emplace(client.listTools())) << "405 降级后 tools/list 必须继续可用";
        std::optional<aiSDK::MCPToolCallResult> result;
        ASSERT_NO_THROW(result.emplace(client.callTool(*catalog, "echo",
                                                       {
                                                           {"value", "HTTP 中文"}
        })))
            << "405 降级后 tools/call 必须继续可用";
        EXPECT_EQ(result->structured_content.at("arguments").at("value"), "HTTP 中文");
        EXPECT_TRUE(server.protocolHeadersObserved());
        EXPECT_TRUE(server.credentialObserved());

        client.close();
        EXPECT_TRUE(server.deleteObserved());
        EXPECT_GE(provider->calls(), 6U);
        proxy_connections = proxy_trap.acceptedConnections();
    }
    EXPECT_EQ(proxy_connections, 0U) << "生产 HTTP Session 不得读取环境代理";
    EXPECT_EQ(captureProxyEnvironment(), environment_before) << "代理陷阱测试必须完整恢复进程环境";
}

TEST(MCPHttpIntegrationTest, 长期GETListener与POSTSSE工具结果可并存) {
    LoopbackMCPServer server({true, true, false, {}});
    aiSDK::MCPClient client(makeHttpConfig(server));

    ASSERT_NO_THROW(client.connect()) << "长期 Listener 用例必须先完成初始化；已收请求=" << server.requestSummary();
    EXPECT_EQ(client.listenerState(), aiSDK::MCPListenerState::Listening);
    const auto catalog = client.listTools();
    const auto result = client.callTool(catalog, "echo",
                                        {
                                            {"mode", "sse"}
    });
    EXPECT_EQ(result.content.at(0).at("text"), "HTTP 调用成功");
    EXPECT_EQ(result.structured_content.at("arguments").at("mode"), "sse");
    EXPECT_GE(server.acceptedConnections(), 4U);
    client.close();
}

TEST(MCPHttpIntegrationTest, POSTSSE建流后可超过请求段并仍受公开绝对上限约束) {
    // 回环夹具先只发送合法 SSE Header，再由测试在短请求段已经过去后明确释放终局事件。
    ServerOptions options;
    options.tool_call_as_sse = true;
    options.hold_tool_sse_after_headers = true;
    LoopbackMCPServer server(std::move(options));
    auto config = makeHttpConfig(server);
    config.limits.request_timeout = 100ms;
    config.limits.absolute_request_timeout = 2s;
    aiSDK::MCPClient client(std::move(config));
    ASSERT_NO_THROW(client.connect());
    const auto catalog = client.listTools();

    std::optional<aiSDK::MCPToolCallResult> result;
    std::exception_ptr failure;
    std::thread call_thread([&] {
        try {
            result.emplace(client.callTool(catalog, "echo",
                                           {
                                               {"timing", "sse"}
            }));
        } catch(...) {
            failure = std::current_exception();
        }
    });
    const bool headers_received = server.waitForToolSseHeaders(1s);
    if(!headers_received) {
        server.releaseToolSse();
        call_thread.join();
        FAIL() << "POST SSE 测试夹具未在限定时间发出响应头";
        return;
    }

    // 这是显式计时合同用例；等待两倍请求段后再释放，证明建流事件已延长 Client 等待。
    std::this_thread::sleep_for(250ms);
    server.releaseToolSse();
    call_thread.join();

    ASSERT_EQ(failure, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->content.at(0).at("text"), "HTTP 调用成功");
    EXPECT_EQ(result->structured_content.at("arguments").at("timing"), "sse");
    client.close();
}

TEST(MCPHttpIntegrationTest, 已有会话的工具POST404进入Faulted并保留结果未知) {
    LoopbackMCPServer server({false, false, true, {}});
    aiSDK::MCPClient client(makeHttpConfig(server));
    ASSERT_NO_THROW(client.connect()) << "会话 404 用例必须先完成初始化；已收请求=" << server.requestSummary();
    const auto catalog = client.listTools();

    try {
        static_cast<void>(client.callTool(catalog, "echo", nlohmann::json::object()));
        FAIL() << "已提交工具的会话 404 必须返回结果未知";
    } catch(const aiSDK::MCPException& exception) {
        EXPECT_EQ(exception.code(), aiSDK::MCPErrorCode::OutcomeUnknown);
        ASSERT_TRUE(exception.causeCode().has_value());
        EXPECT_EQ(*exception.causeCode(), aiSDK::MCPErrorCode::SessionExpired);
        EXPECT_EQ(exception.clientStateAtFailure(), aiSDK::MCPClientState::Faulted);
    }
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Faulted);
    client.close();
}

TEST(MCPHttpIntegrationTest, POSTSSE断线只用事件游标GET恢复且不重放工具POST) {
    LoopbackMCPServer server({false, false, false, {}, true});
    aiSDK::MCPClient client(makeHttpConfig(server));
    ASSERT_NO_THROW(client.connect()) << "SSE 恢复用例必须先完成初始化；已收请求=" << server.requestSummary();
    const auto catalog = client.listTools();

    const auto result = client.callTool(catalog, "echo",
                                        {
                                            {"recover", true}
    });
    EXPECT_EQ(result.content.at(0).at("text"), "SSE 恢复成功");
    EXPECT_TRUE(result.structured_content.at("recovered"));
    EXPECT_EQ(server.toolCallPostCount(), 1U) << "恢复只能续接流，绝不能重放原工具 POST";
    EXPECT_EQ(server.recoveryGetCount(), 1U);
    client.close();
}

TEST(MCPHttpTlsIntegrationTest, 当前有效且主机匹配的自签名证书仍被系统信任路径拒绝) {
    aiSDK::test::McpTlsTestServer server(aiSDK::test::McpTlsCertificateMode::SelfSigned);
    ASSERT_NO_THROW(server.start());
    const std::string host(aiSDK::test::kMcpTlsTestServerName);
    auto config = makeTlsConfig(server, host);
    auto transport = makeTlsTestTransport(config, host, server.endpointPort(), false);
    aiSDK::MCPClient client(std::move(config), std::move(transport));

    try {
        client.connect();
        FAIL() << "未注入测试根 CA 时，自签名 Server 必须被证书链校验拒绝";
    } catch(const aiSDK::MCPException& exception) {
        EXPECT_EQ(exception.code(), aiSDK::MCPErrorCode::TransportFailure);
    }
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Faulted);
    client.close();
}

TEST(MCPHttpTlsIntegrationTest, 测试根CA加正确主机名通过真实TLS与最小MCP交互) {
    const EnvironmentSnapshot environment_before = captureProxyEnvironment();
    std::size_t proxy_connections = 0U;
    {
        LoopbackMCPServer proxy_trap({false, false, false, {}});
        ProxyEnvironmentGuard proxy_environment(proxy_trap.origin());
        aiSDK::test::McpTlsTestServer server(aiSDK::test::McpTlsCertificateMode::RootSigned);
        ASSERT_NO_THROW(server.start());
        const std::string host(aiSDK::test::kMcpTlsTestServerName);
        auto config = makeTlsConfig(server, host);
        auto transport = makeTlsTestTransport(config, host, server.endpointPort(), true);
        aiSDK::MCPClient client(std::move(config), std::move(transport));

        ASSERT_NO_THROW(client.connect());
        EXPECT_EQ(client.state(), aiSDK::MCPClientState::Ready);
        EXPECT_EQ(client.listenerState(), aiSDK::MCPListenerState::Unsupported);
        client.close();
        EXPECT_EQ(client.state(), aiSDK::MCPClientState::Closed);
        proxy_connections = proxy_trap.acceptedConnections();
    }
    EXPECT_EQ(proxy_connections, 0U) << "TLS 生产路径不得读取环境代理";
    EXPECT_EQ(captureProxyEnvironment(), environment_before) << "TLS 代理陷阱必须恢复全部环境变量";
}

TEST(MCPHttpTlsIntegrationTest, 同一测试根CA与Server在错误主机名下被明确拒绝) {
    aiSDK::test::McpTlsTestServer server(aiSDK::test::McpTlsCertificateMode::RootSigned);
    ASSERT_NO_THROW(server.start());
    const std::string wrong_host = "mcp-wrong.test.local";
    auto config = makeTlsConfig(server, wrong_host);
    auto transport = makeTlsTestTransport(config, wrong_host, server.endpointPort(), true);
    aiSDK::MCPClient client(std::move(config), std::move(transport));

    try {
        client.connect();
        FAIL() << "同一受信证书链在错误主机名下必须被主机名校验拒绝";
    } catch(const aiSDK::MCPException& exception) {
        EXPECT_EQ(exception.code(), aiSDK::MCPErrorCode::TransportFailure);
    }
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Faulted);
    client.close();
}

TEST(MCPHttpIntegrationTest, 重复二十五轮关闭后内部活动计数归零且本进程资源不增长) {
    const auto counters = std::make_shared<aiSDK::detail::MCPHttpTestActivityCounters>();
    auto run_round = [&] {
        // Server 也在本轮作用域内销毁，资源样本只观察进程的稳定静止状态。
        LoopbackMCPServer server({false, false, false, {}});
        auto config = makeHttpConfig(server);
        aiSDK::detail::MCPHttpTestOverrides overrides;
        overrides.activity_counters = counters;
        const auto& http = std::get<aiSDK::MCPStreamableHttpConfig>(config.transport);
        auto transport =
            aiSDK::detail::createStreamableHttpMCPTransportForTest(http, config.limits, std::move(overrides));
        aiSDK::MCPClient client(std::move(config), std::move(transport));
        client.connect();
        client.close();
        EXPECT_EQ(client.state(), aiSDK::MCPClientState::Closed);
        EXPECT_EQ(counters->active_requests.load(), 0U);
        EXPECT_EQ(counters->active_sse_workers.load(), 0U);
        EXPECT_EQ(counters->active_sessions.load(), 0U);
    };

    // 首轮只用于预热 cpr、libcurl、Winsock/线程库的惰性全局资源。
    run_round();
    const std::size_t baseline = currentProcessResourceCount();
    std::vector<std::size_t> samples;
    samples.reserve(25U);
    for(std::size_t round = 0U; round < 25U; ++round) {
        run_round();
        samples.push_back(currentProcessResourceCount());
    }

    bool strictly_increasing = true;
    for(std::size_t index = 1U; index < samples.size(); ++index) {
        strictly_increasing = strictly_increasing && samples[index] > samples[index - 1U];
    }
    EXPECT_FALSE(strictly_increasing);
    ASSERT_FALSE(samples.empty());
    EXPECT_LE(samples.back(), baseline + 3U);
}

}  // namespace
