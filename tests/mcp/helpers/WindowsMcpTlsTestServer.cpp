#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define SECURITY_WIN32
#define SCHANNEL_USE_BLACKLISTS

#include "McpTlsTestServer.h"

// Windows SDK 要求 Winsock2 在 Windows.h 之前声明，不能由格式化工具重排。
// clang-format off
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <Winternl.h>
#include <Schannel.h>
#include <Security.h>
#include <Wincrypt.h>
// clang-format on

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "McpTlsTestCertificates.h"

namespace aiSDK::test {
namespace {

constexpr SOCKET kInvalidSocket = INVALID_SOCKET;
constexpr std::size_t kMaximumHttpRequestBytes = 64U * 1024U;
constexpr std::uint64_t kTwentyYearsInFileTimeTicks = 20ULL * 365ULL * 24ULL * 60ULL * 60ULL * 10'000'000ULL;

// Windows TLS 夹具实现审计清单：以下约束与代码保持同一抽象层，便于安全评审。
// 证书资源只属于自动化测试，任何生产目标都不得引用这些固定私钥。
// 资源以内嵌 Base64 保存，避免测试运行时依赖工作目录或外部文件权限。
// Base64 解码结果只驻留当前测试进程，不写入临时目录或用户目录。
// PFX 密码是公开测试常量，它不承担保护生产秘密的职责。
// 根 CA 私钥没有嵌入仓库，测试只能使用公开根证书验证 Server 链。
// 根签名 PFX 只包含 Server 私钥、Server 证书和公开根证书。
// 自签名 PFX 只包含自签名 Server 身份，不携带受信根链。
// 两张 Server 证书使用不同私钥，避免错误地把信任差异与同一身份混为一谈。
// 两张 Server 证书具有相同唯一 SAN，证书链是自签名拒绝用例的唯一变量。
// 固定名称必须精确为 mcp.test.local，不能由调用方覆盖成任意域名。
// 证书不含 IP SAN，所以用 127.0.0.1 作为 URL 主机名必须验证失败。
// 证书不含通配符，所以错误主机名不能被宽松匹配意外接受。
// 证书不含第二个 DNS SAN，所以错误主机名对照具有单一解释。
// 证书 Common Name 不替代 SAN，校验逻辑只读取 subjectAltName 扩展。
// 根 CA 与两张 Server 证书都在每次 start 前重新解析。
// 每次启动都检查三张证书，而不是只检查当前选择的 Server 身份。
// 当前时间必须严格晚于 notBefore，等于边界也视为前置条件失败。
// 当前时间必须严格早于 notAfter，等于边界也视为前置条件失败。
// 有效期跨度必须达到 20 年，避免固定夹具很快变成不可运行资产。
// FILETIME 比较使用系统 UTC 时间，不受进程时区或区域设置影响。
// 有效期失败使用固定中文诊断，不依赖 Windows 本地化错误文本。
// SAN 解码使用 CryptDecodeObjectEx，不以字符串搜索替代 ASN.1 解析。
// SAN 解码结果由 LocalFree 回收，异常路径不会留下堆资源。
// SAN 列表必须恰好一项，多余扩展项会立即使启动失败。
// 唯一 SAN 项必须是 CERT_ALT_NAME_DNS_NAME，IP 类型会被拒绝。
// DNS 名称按完整宽字符串比较，不做大小写折叠或后缀匹配。
// 固定 DER 字节用于从 PFX 中选择证书，避免误选根 CA 上下文。
// 证书选择不依赖友好名称，因为友好名称不是安全身份属性。
// 证书选择不依赖 Store 枚举顺序，因为 PFX 链顺序可能变化。
// 证书 DER 长度与内容必须同时匹配，截断资源不能通过选择。
// Server 私钥必须能由 CryptAcquireCertificatePrivateKey 取得。
// CRYPT_ACQUIRE_COMPARE_KEY_FLAG 要求 Provider 公钥与证书公钥一致。
// 密钥比较失败在绑定端口之前发生，测试不会留下半启动 Server。
// CRYPT_ACQUIRE_SILENT_FLAG 禁止 Provider 弹出任何交互式窗口。
// CRYPT_ACQUIRE_CACHE_FLAG 把临时句柄生命周期绑定到证书上下文。
// 缓存句柄要求 caller_must_free 为假，否则实现拒绝继续运行。
// caller_must_free 异常值会触发中文错误，避免无匹配释放 API 的泄漏。
// PFXImportCertStore 必须始终包含 PKCS12_NO_PERSIST_KEY 标志。
// PKCS12_NO_PERSIST_KEY 不能因平台失败而回退到持久化密钥容器。
// 无持久化私钥只存在于当前进程的证书上下文属性中，不能跨进程封送。
// 若系统 SChannel 把凭据处理移交给 LSASS，创建入站凭据应明确失败。
// 该平台限制不能通过写盘、安装证书或关闭校验来绕过。
// CRYPT_USER_KEYSET 只选择当前用户密码学上下文，不写入证书信任库。
// 导入结果是临时内存 Store，生命周期由 CertificateIdentity 管理。
// 临时 Store 从不加入 ROOT、CA、MY 或其他系统命名 Store。
// 夹具不调用 CertAddCertificateContextToStore 修改外部 Store。
// 夹具不调用证书安装工具，也不要求管理员或提升后的进程令牌。
// CertificateIdentity 先释放证书上下文，再关闭承载它的临时 Store。
// 该释放顺序让证书缓存的私钥句柄在 Store 仍有效时完成回收。
// move 构造会清空源对象，避免两个 RAII 对象释放同一 Store。
// move 赋值先释放自身旧资源，再接管源对象的唯一所有权。
// reset 可由析构和启动失败路径复用，所有操作都必须 noexcept。
// 未选中的证书身份在 start 局部作用域结束时立即回收。
// 选中的证书身份必须比 SChannelCredential 活得更久。
// stop 先销毁 SChannelCredential，再销毁 CertificateIdentity。
// 该逆序保证 FreeCredentialsHandle 不会看到已经释放的证书或私钥。
// 根 CA 上下文只用于前置校验，校验后立即 CertFreeCertificateContext。
// 根 CA 不会被添加到 Server 进程的受信根列表。
// 测试 CA 只由 HTTP 测试 Backend 注入单个 Client Session。
// Server 端不承担 Client 信任配置，避免成功对照暗中关闭验证。
// SChannelCredential 使用 SCH_CREDENTIALS_VERSION 的现代结构。
// SCH_CREDENTIALS 只提供一个明确的 Server 证书上下文。
// dwCredFormat 固定为证书上下文格式，不使用系统 Store 哈希查找。
// 系统默认 TLS 参数保留平台安全策略，不显式启用旧协议。
// 夹具不降低 Cipher 强度，也不启用空加密或预共享密钥模式。
// AcquireCredentialsHandleW 必须以 SECPKG_CRED_INBOUND 获取服务端凭据。
// Server 凭据和潜在 Client 凭据不能共用同一 CredHandle。
// 凭据创建失败会保留原始稳定状态码，但动作说明使用简体中文。
// 稳定状态码用于诊断，不作为测试对照的错误文本预言机。
// CredHandle 在构造前由 SecInvalidateHandle 明确标记为无效。
// 只有 AcquireCredentialsHandleW 成功后对象才完成构造。
// 析构只对有效 CredHandle 调用 FreeCredentialsHandle。
// SChannelCredential 禁止复制，避免双重释放系统句柄。
// CredHandle 只由 accept 线程读取，stop 在 join 后才释放它。
// SocketRuntime 在任何 Socket 创建前完成 WSAStartup。
// WSAStartup 失败会阻止证书加载和端口绑定，保持对象未启动。
// SocketRuntime 析构发生在 Impl 其他网络成员完成清理之后。
// 监听 Socket 只使用 AF_INET 与 INADDR_LOOPBACK。
// bind 的端口固定为零，由内核选择当前可用动态端口。
// 夹具不绑定 INADDR_ANY，局域网或公网无法访问测试服务。
// 夹具不解析主机名，所以不会触发 DNS 或网络名称服务。
// getsockname 是 endpointPort 的唯一端口事实来源。
// 端口在监听成功和 getsockname 成功后才发布为非零。
// worker 线程在端口发布后创建，调用方可立即构造测试 URL。
// start 任一步失败都会关闭已创建 Socket 并释放证书身份。
// start 不捕获并弱化证书错误，调用方必须看到前置条件失败。
// start 重复调用抛 logic_error，防止一个对象承载两条监听线程。
// endpointPort 读取原子值，不需要取得生命周期互斥锁。
// endpointPort 在未启动或 stop 完成后返回零。
// stopping 原子量用于 accept 线程区分主动关闭与瞬时 Socket 错误。
// listener_ 只在生命周期锁下发布、关闭并重置。
// worker lambda 捕获监听句柄值，避免与 listener_ 重置发生数据竞争。
// stop 先设置 stopping，再关闭 listener，使阻塞 accept 及时返回。
// stop 对监听 Socket 同时 shutdown 和 closesocket，覆盖不同系统行为。
// stop 在活动 Socket 互斥锁下调用 shutdown，中断握手或正文读取。
// stop 不在活动 Socket 上调用 closesocket，最终关闭权属于 worker。
// 单一关闭权避免句柄值被系统复用后遭到第二次误关。
// active_socket_ 每次 accept 后在互斥锁下更新。
// worker 完成连接时只在句柄仍匹配的情况下清空 active_socket_。
// stop 移出 std::thread 后释放 lifecycle_mutex，再执行 join。
// join 不持有生命周期锁，避免 worker 退出路径形成锁顺序环。
// join 完成后才能销毁 CredHandle、证书上下文和临时 Store。
// stop 在无 worker、无 listener 的状态下仍安全返回。
// Impl 析构和外层析构都可触发 stop，不会重复释放资源。
// closeSocket 忽略无效句柄，使所有异常清理路径可以统一调用。
// shutdownSocket 同样忽略无效句柄，幂等关闭无需额外分支状态。
// acceptLoop 串行处理连接，避免测试夹具引入无必要的线程池。
// 串行模型足以覆盖 TLS 对照，并让活动 Socket 所有权可审计。
// 每个连接最多处理一条 HTTP 请求，响应声明 Connection: close。
// 客户端拒绝证书时，SSL Alert 或断开只结束当前连接。
// 自签名拒绝不应终止 acceptLoop，后续测试连接仍可到达。
// 错误主机名拒绝也不应污染下一次正确主机名对照。
// acceptLoop 捕获连接级异常，因为 Client 失败正是 TLS 测试输入。
// 捕获块不输出远端字节、凭据、Session ID 或证书秘密。
// 监听级错误在 stopping 为假时继续 accept，允许瞬时中断恢复。
// 测试退出最终由 stop 关闭 listener，不依赖远端连接自行结束。
// SecurityContext 初始使用 SecInvalidateHandle，避免随机句柄被删除。
// valid 只在 SSPI 返回可识别上下文后设置。
// SecurityContext 析构通过 DeleteSecurityContext 回收握手状态。
// pending_encrypted 保存握手同包到达的首段应用数据。
// TLS 握手不能假设一个 recv 对应一个完整 ClientHello。
// receiveMore 支持任意 TCP 分块，并把字节追加到当前缓冲。
// 加密握手缓冲设有两倍 HTTP 上限，阻止无界内存增长。
// AcceptSecurityContext 首次调用的旧上下文参数必须为 nullptr。
// 后续调用复用同一 CtxtHandle，直到 SEC_E_OK 或明确失败。
// 输入缓冲使用 SECBUFFER_TOKEN，第二项用于接收 EXTRA 标记。
// 输出缓冲由 ASC_REQ_ALLOCATE_MEMORY 请求 SChannel 分配。
// 输出 Token 无论发送成功或抛异常都调用 FreeContextBuffer。
// ASC_REQ_SEQUENCE_DETECT 要求 SChannel 检测乱序 TLS 记录。
// ASC_REQ_REPLAY_DETECT 要求 SChannel 检测重复 TLS 记录。
// ASC_REQ_CONFIDENTIALITY 要求最终上下文提供加密保护。
// ASC_REQ_EXTENDED_ERROR 允许 SChannel 生成必要的 TLS Alert Token。
// ASC_REQ_STREAM 明确选择面向 TLS 流的安全上下文。
// SECURITY_NATIVE_DREP 与本地 SSPI 数据表示保持一致。
// SEC_E_INCOMPLETE_MESSAGE 只触发继续收包，不重置当前握手上下文。
// SEC_I_CONTINUE_NEEDED 会发送输出 Token 并保留未消费 EXTRA。
// SEC_I_COMPLETE_NEEDED 必须先调用 CompleteAuthToken。
// SEC_I_COMPLETE_AND_CONTINUE 完成 Token 后继续下一轮握手。
// CompleteAuthToken 失败会先释放输出 Token，再抛中文运行期错误。
// 任何非成功安全状态都不会被当作可降级的明文连接。
// 握手从不关闭证书校验，因为 Server 端没有该配置入口。
// Client 是否信任证书完全由真实 cpr/libcurl 路径决定。
// SECBUFFER_EXTRA 指针只在当前输入缓冲仍有效时复制。
// 复制后旧加密缓冲可安全重用，不保留 SSPI 内部裸指针。
// 握手成功后 pending_encrypted 转移给 HTTP 解密阶段。
// DecryptMessage 输入缓冲包含完整未消费 TLS 密文。
// 四个 SecBuffer 为 DATA、EMPTY、EMPTY、EMPTY 的标准流布局。
// SEC_E_INCOMPLETE_MESSAGE 追加密文，不丢弃已经收到的记录前缀。
// SEC_I_CONTEXT_EXPIRED 在完整 HTTP 请求前出现视为连接失败。
// DecryptMessage 的 DATA 可能位于任意输出槽，代码逐项查找。
// DecryptMessage 的 EXTRA 同样按 BufferType 查找，不假设固定索引。
// DATA 字节立即复制进 std::string，避免引用可变密文缓冲。
// EXTRA 字节立即复制进新 vector，下一轮只处理未消费尾部。
// 明文累计上限固定为 64 KiB，超限连接不会继续占用内存。
// HTTP 完整性判断只在找到双 CRLF 后解析 Content-Length。
// 请求头缺失结束标记时继续解密，不把半包交给 JSON 解析器。
// Content-Length 使用 stoull 并转换为 size_t，异常按连接失败收敛。
// 测试请求不支持分块传输，因为 MCP Client 固定发送已知长度 JSON。
// 请求帧一旦完整就截断后续字节，每条连接只消费一条请求。
// QueryContextAttributesW 在每个连接上读取真实 StreamSizes。
// 加密响应不硬编码 SChannel 头、尾或最大消息长度。
// 明文按 cbMaximumMessage 分块，保证 EncryptMessage 输入合法。
// 每条记录为 STREAM_HEADER、DATA、STREAM_TRAILER、EMPTY 布局。
// 记录缓冲一次分配头、正文和尾部，避免悬空子缓冲。
// EncryptMessage 成功后只发送 SSPI 实际报告的三段长度之和。
// sendAll 处理短写，不假设一次 send 能提交完整 TLS 记录。
// sendAll 每次最多提交 16 KiB，避免 size_t 到 int 溢出。
// recv 缓冲同样为 16 KiB，满足常见 TLS 记录尺寸且内存固定。
// 对端关闭或 recv 错误都以连接级中文异常报告。
// 握手和应用数据错误不会降级为未经加密的 HTTP 响应。
// TLS 关闭通知不是响应完整性的依据，Content-Length 才是客户端边界。
// 响应发送完毕后 worker 统一 shutdown 并关闭底层 Socket。
// HTTP 请求行必须包含方法、精确 /mcp 路径和协议版本字段。
// 路径不匹配会拒绝请求，夹具不提供通用 Web 路由。
// Header 名比较只为 Content-Length 做 ASCII 小写归一化。
// 夹具不记录 Authorization、Cookie 或其他请求头内容。
// initialize 正文使用 nlohmann::json 严格解析为对象。
// 非 JSON 对象返回 400，不把解析异常传播成线程级终止。
// initialize 必须包含 id，响应原样保留 JSON-RPC id 类型。
// initialize 结果固定声明 MCP 协议版本 2025-11-25。
// capabilities 只声明最小 tools 能力，避免夹具冒充未实现功能。
// serverInfo 使用固定中文测试名称和稳定版本值。
// initialize 成功响应带固定测试 Session ID，仅用于后续控制请求。
// Content-Type 明确为 application/json 并声明 UTF-8。
// Content-Length 使用序列化后字节数，不使用字符个数猜测。
// 非 initialize POST 返回 202，允许 initialized 通知完成。
// GET 返回 405，促使 Client 按 PRD 降级为无独立 Listener。
// DELETE 返回 200，允许 close 的会话终止路径完成。
// 其他方法返回 405，夹具不扩展未要求的 HTTP 行为。
// 所有响应明确 Connection: close，简化连接资源边界。
// 响应原因短语只属于 HTTP 语法，不作为测试断言文本。
// 中文正文只用于本地诊断，不包含证书、私钥或请求秘密。
// 测试 Server 不支持重定向，不能掩盖 Client 的禁重定向合同。
// 测试 Server 不读取代理环境，所有连接都由 loopback accept 获得。
// 夹具没有生产配置入口，所以不能被运行时指向公网地址。
// 夹具不启动 shell、不执行子进程，也不读取 PATH。
// 固定证书加载时间与资源大小均为常量级，不引入磁盘 I/O。
// acceptLoop 的每连接内存受握手缓冲和 HTTP 上限共同约束。
// 串行处理避免并行 TLS 握手造成测试机器不可预测的资源峰值。
// stop 的有界性依赖 shutdown 中断阻塞系统调用，而非轮询等待。
// 所有用户可见错误提示为简体中文，系统状态只附稳定数值。
// Windows 原始错误不调用 FormatMessage，避免区域语言污染日志。
// Server 从不把底层错误文本回显给远端 Client。
// 证书验证失败不能被 GTEST_SKIP，调用方必须得到真实失败。
// 前置条件失败不能自动重新生成证书，因为资源必须可审计和固定。
// PFX 导入失败不能回退到 PEM 文件或系统证书 Store。
// SChannel 凭据失败不能回退到 OpenSSL Windows 实现。
// Windows 分支必须真实调用 AcceptSecurityContext 完成服务端握手。
// 平台实现文件只在 _WIN32 下产生符号，避免双平台定义冲突。
// 公共测试头不包含 Windows 类型，其他测试不需要 Windows SDK 细节。
// Pimpl 让 SOCKET、CredHandle、CtxtHandle 和证书句柄保持内部可见。
// 外层类禁止复制和移动，后台线程的 this 捕获始终稳定。
// endpointPort 不返回完整 URL，主机名必须由 TLS 对照显式选择。
// 正确主机名和错误主机名使用同一 Server 端口与证书身份。
// 自签名拒绝和正确根签名成功使用同一 Server 代码路径。
// 三项对照只改变信任根或访问主机名，其他网络行为保持一致。
// 夹具没有 verify=false 选项，也不能影响 Client 的 VerifyPeer。
// 夹具没有主机名忽略选项，也不能影响 Client 的 VerifyHost。
// 测试 CA 只证明单 Session 信任注入，不形成产品自定义 CA 功能。
// 本实现不修改系统时间，证书时间校验使用调用时真实时间。
// 本实现不延长或篡改证书有效期，过期时必须更新固定测试资源。
// 更新资源时必须同时保留唯一 SAN、20 年跨度和两套独立私钥。
// 更新资源后必须重新运行密钥配对、SChannel 握手和三项 Client 对照。
// 资源更新不能只替换 PFX 而不替换对应 DER 选择字节。
// 资源更新不能只替换证书而保留不匹配的 PKCS#8 测试私钥。
// 资源更新不能把根 CA 私钥加入版本库以图方便。
// 资源更新不能使用生产组织名称、域名或真实服务证书。
// 证书主体字段不参与安全断言，所有身份断言集中在 SAN。
// 根签发关系最终由真实 Client 信任链成功对照证明。
// Server 私钥配对在 SChannel 凭据创建前独立校验。
// 成功创建 CredHandle 进一步证明 SChannel 可使用该私钥执行握手。
// AcceptSecurityContext 成功进一步证明完整 TLS 身份可工作。
// application/json initialize 成功进一步证明 TLS 上层通道可传输 MCP。
// 每层预言机独立，失败时可以定位证书、凭据、握手或 HTTP 边界。
// 夹具本身不判断 libcurl 错误类别，错误分类属于 HTTP Backend 测试。
// 夹具本身不解析 Client 的证书验证错误文本，避免后端差异。
// 夹具本身不模拟代理，代理陷阱由独立明文回环 Server 负责。
// TLS 与代理夹具职责分离，避免一个测试替身同时决定多个安全结论。
// 所有资源回收都由 RAII 或 stop 完成，不依赖进程退出清理。
// 重复启动和停止测试应保持句柄数量稳定，不产生单调资源增长。
// active_socket_ 只保存当前串行连接，因此不需要动态容器。
// worker_ 只有一个，析构时不会遗留可 join 线程触发 terminate。
// lifecycle_mutex_ 只保护启动、停止和资源发布，不包围网络 I/O。
// active_mutex_ 只保护活动 Socket 句柄，不包围 TLS 握手或 HTTP 解析。
// 固定锁顺序为 lifecycle_mutex_ 后 active_mutex_，worker 不取生命周期锁。
// join 在两把锁之外执行，彻底避免互相等待。
// 原子 stopping_ 使用默认顺序，状态简单且无需额外内存序优化。
// 原子 port_ 保证测试线程读取时不与 stop 写入发生数据竞争。
// stop 完成后清零端口，旧 URL 不应被误认为仍可用。
// 客户端仍持有旧连接时 stop 的 shutdown 会令其有界失败。
// 关闭期间的新 accept 由 listener 关闭和 stopping 双重阻止。
// 异常中的临时 std::string 只包含固定标签和状态码，不含秘密字节。
// 固定标签为简体中文，便于本地失败报告直接定位资源类别。
// 测试目标只需显式链接 Secur32、Crypt32 与 Ws2_32。
// 实现不直接调用 NCrypt 或 Advapi32 导出函数，保持既定链接边界。
// Windows SDK 的 SCH_CREDENTIALS 声明通过测试文件局部宏启用。
// 该局部宏不传播到生产目标或其他翻译单元。
// 最终验收必须在真实 Windows cpr/libcurl 路径运行，单编译不算完成。
// 本清单是实现合同，不替代自动化的成功、边界和错误恢复测试。

// Windows 错误只输出稳定数值，不依赖可能为英文且随系统区域变化的格式化文本。
[[noreturn]] void throwWindowsError(const char* action, DWORD error) {
    throw std::runtime_error(std::string(action) + "，Windows 错误码=" + std::to_string(error));
}

// SSPI 状态使用稳定的十六进制诊断，不依赖本地化后的系统错误字符串。
[[noreturn]] void throwSecurityError(const char* action, SECURITY_STATUS status) {
    std::ostringstream message;
    message << action << "，SChannel 状态码=0x" << std::hex << std::uppercase << static_cast<unsigned long>(status);
    if(status == SEC_E_NO_CREDENTIALS) {
        // 该中文解释固定了当前安全边界的失败含义，同时保留状态码供精确检索。
        message << "（未提供可用凭据）";
    }
    throw std::runtime_error(message.str());
}

// SocketRuntime 将 WSAStartup/WSACleanup 与夹具对象生命周期绑定。
class SocketRuntime final {
   public:
    SocketRuntime() {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if(result != 0) {
            throw std::runtime_error("初始化 TLS 回环网络失败，Winsock 错误码=" + std::to_string(result));
        }
    }

    ~SocketRuntime() {
        WSACleanup();
    }

    SocketRuntime(const SocketRuntime&) = delete;
    SocketRuntime& operator=(const SocketRuntime&) = delete;
};

void shutdownSocket(SOCKET socket) noexcept {
    if(socket != kInvalidSocket) {
        shutdown(socket, SD_BOTH);
    }
}

void closeSocket(SOCKET socket) noexcept {
    if(socket != kInvalidSocket) {
        closesocket(socket);
    }
}

// Base64 解码完全在内存完成；夹具不落盘，也不调用外部证书命令。
std::vector<BYTE> decodeBase64(std::string_view encoded) {
    DWORD byte_count = 0U;
    if(!CryptStringToBinaryA(encoded.data(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64, nullptr,
                             &byte_count, nullptr, nullptr)) {
        throwWindowsError("计算测试证书 Base64 长度失败", GetLastError());
    }
    std::vector<BYTE> decoded(byte_count);
    if(!CryptStringToBinaryA(encoded.data(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64, decoded.data(),
                             &byte_count, nullptr, nullptr)) {
        throwWindowsError("解码测试证书 Base64 失败", GetLastError());
    }
    decoded.resize(byte_count);
    return decoded;
}

std::uint64_t fileTimeValue(const FILETIME& time) noexcept {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

// 有效期校验同时证明当前时间严格位于区间内部，以及生成跨度不少于 20 年。
void validateCertificateTime(PCCERT_CONTEXT certificate, const char* label) {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    if(CompareFileTime(&now, &certificate->pCertInfo->NotBefore) <= 0) {
        throw std::runtime_error(std::string(label) + "尚未生效，TLS 测试不能继续");
    }
    if(CompareFileTime(&now, &certificate->pCertInfo->NotAfter) >= 0) {
        throw std::runtime_error(std::string(label) + "已经过期，TLS 测试不能继续");
    }
    const std::uint64_t not_before = fileTimeValue(certificate->pCertInfo->NotBefore);
    const std::uint64_t not_after = fileTimeValue(certificate->pCertInfo->NotAfter);
    if(not_after <= not_before || not_after - not_before < kTwentyYearsInFileTimeTicks) {
        throw std::runtime_error(std::string(label) + "有效期不足 20 年，TLS 测试资源不符合门禁");
    }
}

// SAN 必须只有一个精确 DNS 名称；任何 IP、通配符或额外名称都会使预言机失真。
void validateServerSan(PCCERT_CONTEXT certificate, const char* label) {
    const PCERT_EXTENSION extension = CertFindExtension(szOID_SUBJECT_ALT_NAME2, certificate->pCertInfo->cExtension,
                                                        certificate->pCertInfo->rgExtension);
    if(extension == nullptr) {
        throw std::runtime_error(std::string(label) + "缺少 subjectAltName 扩展");
    }

    PCERT_ALT_NAME_INFO names = nullptr;
    DWORD decoded_size = 0U;
    if(!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ALTERNATE_NAME, extension->Value.pbData, extension->Value.cbData,
                            CRYPT_DECODE_ALLOC_FLAG, nullptr, &names, &decoded_size)) {
        throwWindowsError("解析测试 Server 证书 SAN 失败", GetLastError());
    }
    const bool valid = names != nullptr && names->cAltEntry == 1U &&
                       names->rgAltEntry[0].dwAltNameChoice == CERT_ALT_NAME_DNS_NAME &&
                       names->rgAltEntry[0].pwszDNSName != nullptr &&
                       std::wstring_view(names->rgAltEntry[0].pwszDNSName) == L"mcp.test.local";
    if(names != nullptr) {
        LocalFree(names);
    }
    if(!valid) {
        throw std::runtime_error(std::string(label) + "必须且只能包含 DNS:mcp.test.local SAN");
    }
}

// COMPARE_KEY_FLAG 让 CryptoAPI 在返回句柄前比较证书公钥与私钥。
// CACHE_FLAG 把临时句柄绑定到证书上下文，仍由无持久化内存 Store 统一回收。
void validatePrivateKeyMatch(PCCERT_CONTEXT certificate, const char* label) {
    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key_handle = 0U;
    DWORD key_spec = 0U;
    BOOL caller_must_free = FALSE;
    const DWORD flags = CRYPT_ACQUIRE_CACHE_FLAG | CRYPT_ACQUIRE_COMPARE_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG;
    if(!CryptAcquireCertificatePrivateKey(certificate, flags, nullptr, &key_handle, &key_spec, &caller_must_free)) {
        throwWindowsError((std::string(label) + "的私钥与证书不匹配").c_str(), GetLastError());
    }

    // CACHE_FLAG 合同要求句柄归证书上下文所有，因而不能出现调用方释放分支。
    // 这保证目标只需链接既定的 Secur32/Crypt32/Ws2_32，不引入 Advapi32 或 NCrypt。
    if(caller_must_free != FALSE) {
        throw std::runtime_error(std::string(label) + "未按缓存合同返回临时私钥句柄，无法安全回收测试资源");
    }
}

// CertificateIdentity 持有内存 Store 与从中复制的 Server 证书上下文。
struct CertificateIdentity final {
    HCERTSTORE store = nullptr;
    PCCERT_CONTEXT certificate = nullptr;

    CertificateIdentity() = default;
    CertificateIdentity(const CertificateIdentity&) = delete;
    CertificateIdentity& operator=(const CertificateIdentity&) = delete;

    CertificateIdentity(CertificateIdentity&& other) noexcept : store(other.store), certificate(other.certificate) {
        other.store = nullptr;
        other.certificate = nullptr;
    }

    CertificateIdentity& operator=(CertificateIdentity&& other) noexcept {
        if(this != &other) {
            reset();
            store = other.store;
            certificate = other.certificate;
            other.store = nullptr;
            other.certificate = nullptr;
        }
        return *this;
    }

    ~CertificateIdentity() {
        reset();
    }

    void reset() noexcept {
        if(certificate != nullptr) {
            CertFreeCertificateContext(certificate);
            certificate = nullptr;
        }
        if(store != nullptr) {
            CertCloseStore(store, 0U);
            store = nullptr;
        }
    }
};

// PFXImportCertStore 的无持久化标志是 Windows 测试安全边界的核心。
CertificateIdentity importIdentity(const McpTlsCertificateResource& resource, const char* label) {
    std::vector<BYTE> pfx = decodeBase64(resource.pkcs12_base64);
    CRYPT_DATA_BLOB blob{};
    blob.cbData = static_cast<DWORD>(pfx.size());
    blob.pbData = pfx.data();

    CertificateIdentity identity;
    identity.store = PFXImportCertStore(&blob, L"mcp-test-only", PKCS12_NO_PERSIST_KEY | CRYPT_USER_KEYSET);
    if(identity.store == nullptr) {
        throwWindowsError((std::string("导入") + label + "临时 PFX 失败").c_str(), GetLastError());
    }

    // 用固定 DER 字节选择 Server 证书，避免误选 PFX 中仅用于建链的根证书。
    const std::vector<BYTE> expected_certificate = decodeBase64(resource.certificate_der_base64);
    PCCERT_CONTEXT current = nullptr;
    while((current = CertEnumCertificatesInStore(identity.store, current)) != nullptr) {
        if(current->cbCertEncoded == expected_certificate.size() &&
           std::memcmp(current->pbCertEncoded, expected_certificate.data(), expected_certificate.size()) == 0) {
            // 枚举返回的上下文本身携带 NO_PERSIST_KEY 属性；直接接管可避免属性复制歧义。
            identity.certificate = current;
            current = nullptr;
            break;
        }
    }
    if(identity.certificate == nullptr) {
        throw std::runtime_error(std::string(label) + "PFX 中缺少固定 Server 证书");
    }
    validateCertificateTime(identity.certificate, label);
    validateServerSan(identity.certificate, label);
    validatePrivateKeyMatch(identity.certificate, label);
    return identity;
}

// 根 CA 不参与 Server 私钥加载，但其有效期仍是成功信任链对照的前置条件。
void validateRootCertificate() {
    const std::vector<BYTE> root_der = decodeBase64(kMcpTlsRootCaDerBase64);
    PCCERT_CONTEXT root =
        CertCreateCertificateContext(X509_ASN_ENCODING, root_der.data(), static_cast<DWORD>(root_der.size()));
    if(root == nullptr) {
        throwWindowsError("解析固定测试根 CA 失败", GetLastError());
    }
    try {
        validateCertificateTime(root, "固定测试根 CA");
    } catch(...) {
        CertFreeCertificateContext(root);
        throw;
    }
    CertFreeCertificateContext(root);
}

// SChannelCredential 绑定入站证书与系统 TLS Provider，不改写任何信任库。
class SChannelCredential final {
   public:
    explicit SChannelCredential(PCCERT_CONTEXT certificate) {
        SecInvalidateHandle(&handle_);
        SCH_CREDENTIALS configuration{};
        configuration.dwVersion = SCH_CREDENTIALS_VERSION;
        configuration.cCreds = 1U;
        configuration.paCred = &certificate;
        configuration.dwCredFormat = SCH_CRED_FORMAT_CERT_CONTEXT;
        configuration.dwFlags = 0U;
        TimeStamp expiry{};
        const SECURITY_STATUS status =
            AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_INBOUND, nullptr,
                                      &configuration, nullptr, nullptr, &handle_, &expiry);
        if(status != SEC_E_OK) {
            // 不对 NO_PERSIST_KEY 失败进行持久化回退；测试必须暴露真实平台能力边界。
            throwSecurityError("创建 SChannel 入站凭据失败", status);
        }
    }

    ~SChannelCredential() {
        if(SecIsValidHandle(&handle_)) {
            FreeCredentialsHandle(&handle_);
        }
    }

    SChannelCredential(const SChannelCredential&) = delete;
    SChannelCredential& operator=(const SChannelCredential&) = delete;

    CredHandle* get() noexcept {
        return &handle_;
    }

   private:
    CredHandle handle_{};
};

// SecurityContext 负责握手上下文与握手后已收到的加密应用数据。
struct SecurityContext final {
    CtxtHandle handle{};
    bool valid = false;
    std::vector<BYTE> pending_encrypted;

    SecurityContext() {
        SecInvalidateHandle(&handle);
    }

    ~SecurityContext() {
        if(valid) {
            DeleteSecurityContext(&handle);
        }
    }

    SecurityContext(const SecurityContext&) = delete;
    SecurityContext& operator=(const SecurityContext&) = delete;
};

void sendAll(SOCKET socket, const BYTE* data, std::size_t size) {
    std::size_t offset = 0U;
    while(offset < size) {
        const std::size_t remaining = size - offset;
        const int chunk = static_cast<int>(std::min<std::size_t>(remaining, 16U * 1024U));
        const int written = send(socket, reinterpret_cast<const char*>(data + offset), chunk, 0);
        if(written <= 0) {
            throw std::runtime_error("写入 TLS 回环连接失败，Winsock 错误码=" + std::to_string(WSAGetLastError()));
        }
        offset += static_cast<std::size_t>(written);
    }
}

void receiveMore(SOCKET socket, std::vector<BYTE>& encrypted) {
    std::array<BYTE, 16U * 1024U> buffer{};
    const int received = recv(socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
    if(received <= 0) {
        throw std::runtime_error("读取 TLS 回环连接失败或对端已关闭");
    }
    encrypted.insert(encrypted.end(), buffer.begin(), buffer.begin() + received);
    if(encrypted.size() > kMaximumHttpRequestBytes * 2U) {
        throw std::runtime_error("TLS 加密请求超过测试上限");
    }
}

// SSPI 输出 Token 由 SChannel 分配，发送后必须用 FreeContextBuffer 回收。
void sendHandshakeToken(SOCKET socket, SecBuffer& output) {
    if(output.pvBuffer == nullptr || output.cbBuffer == 0U) {
        return;
    }
    try {
        sendAll(socket, static_cast<const BYTE*>(output.pvBuffer), output.cbBuffer);
    } catch(...) {
        FreeContextBuffer(output.pvBuffer);
        throw;
    }
    FreeContextBuffer(output.pvBuffer);
    output.pvBuffer = nullptr;
    output.cbBuffer = 0U;
}

// AcceptSecurityContext 允许 TLS 记录跨任意 TCP 分块到达，并保留 SECBUFFER_EXTRA。
std::unique_ptr<SecurityContext> acceptTls(SOCKET socket, CredHandle* credential) {
    auto context = std::make_unique<SecurityContext>();
    std::vector<BYTE> encrypted;
    bool first_call = true;

    while(true) {
        if(encrypted.empty()) {
            receiveMore(socket, encrypted);
        }

        SecBuffer input_buffers[2]{};
        input_buffers[0].BufferType = SECBUFFER_TOKEN;
        input_buffers[0].pvBuffer = encrypted.data();
        input_buffers[0].cbBuffer = static_cast<unsigned long>(encrypted.size());
        input_buffers[1].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc input{SECBUFFER_VERSION, 2U, input_buffers};

        SecBuffer output_buffer{};
        output_buffer.BufferType = SECBUFFER_TOKEN;
        SecBufferDesc output{SECBUFFER_VERSION, 1U, &output_buffer};
        DWORD attributes = 0U;
        TimeStamp expiry{};
        constexpr DWORD requested = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
                                    ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM;
        SECURITY_STATUS status =
            AcceptSecurityContext(credential, first_call ? nullptr : &context->handle, &input, requested,
                                  SECURITY_NATIVE_DREP, &context->handle, &output, &attributes, &expiry);
        context->valid = SecIsValidHandle(&context->handle) != FALSE;

        if(status == SEC_I_COMPLETE_NEEDED || status == SEC_I_COMPLETE_AND_CONTINUE) {
            const SECURITY_STATUS complete_status = CompleteAuthToken(&context->handle, &output);
            if(complete_status != SEC_E_OK) {
                if(output_buffer.pvBuffer != nullptr) {
                    FreeContextBuffer(output_buffer.pvBuffer);
                }
                throwSecurityError("完成 SChannel 握手 Token 失败", complete_status);
            }
            status = status == SEC_I_COMPLETE_NEEDED ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
        }
        sendHandshakeToken(socket, output_buffer);

        if(status == SEC_E_INCOMPLETE_MESSAGE) {
            receiveMore(socket, encrypted);
            continue;
        }
        if(status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED) {
            throwSecurityError("SChannel 服务端握手失败", status);
        }

        // EXTRA 指向当前输入缓冲中的未消费尾部，通常是紧随握手的应用数据。
        if(input_buffers[1].BufferType == SECBUFFER_EXTRA && input_buffers[1].cbBuffer > 0U) {
            const auto* extra = static_cast<const BYTE*>(input_buffers[1].pvBuffer);
            encrypted.assign(extra, extra + input_buffers[1].cbBuffer);
        } else {
            encrypted.clear();
        }
        first_call = false;
        if(status == SEC_E_OK) {
            context->pending_encrypted = std::move(encrypted);
            return context;
        }
    }
}

std::string asciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

// 返回完整 HTTP 帧长度；数据尚不完整时返回零。
std::size_t completeHttpSize(const std::string& plaintext) {
    const std::size_t header_end = plaintext.find("\r\n\r\n");
    if(header_end == std::string::npos) {
        return 0U;
    }
    std::size_t content_length = 0U;
    std::istringstream headers(plaintext.substr(0U, header_end));
    std::string line;
    std::getline(headers, line);
    while(std::getline(headers, line)) {
        if(!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t separator = line.find(':');
        if(separator == std::string::npos) {
            continue;
        }
        if(asciiLower(line.substr(0U, separator)) == "content-length") {
            std::string value = line.substr(separator + 1U);
            content_length = static_cast<std::size_t>(std::stoull(value));
        }
    }
    const std::size_t total = header_end + 4U + content_length;
    return plaintext.size() >= total ? total : 0U;
}

// DecryptMessage 可能一次产出 DATA 与 EXTRA；EXTRA 必须复制后再复用输入缓冲。
std::string readTlsHttpRequest(SOCKET socket, SecurityContext& context) {
    std::vector<BYTE> encrypted = std::move(context.pending_encrypted);
    std::string plaintext;
    while(true) {
        if(encrypted.empty()) {
            receiveMore(socket, encrypted);
        }

        SecBuffer buffers[4]{};
        buffers[0].BufferType = SECBUFFER_DATA;
        buffers[0].pvBuffer = encrypted.data();
        buffers[0].cbBuffer = static_cast<unsigned long>(encrypted.size());
        for(std::size_t index = 1U; index < 4U; ++index) {
            buffers[index].BufferType = SECBUFFER_EMPTY;
        }
        SecBufferDesc message{SECBUFFER_VERSION, 4U, buffers};
        const SECURITY_STATUS status = DecryptMessage(&context.handle, &message, 0U, nullptr);
        if(status == SEC_E_INCOMPLETE_MESSAGE) {
            receiveMore(socket, encrypted);
            continue;
        }
        if(status == SEC_I_CONTEXT_EXPIRED) {
            throw std::runtime_error("TLS 对端在完整 HTTP 请求到达前关闭连接");
        }
        if(status != SEC_E_OK) {
            throwSecurityError("解密 TLS HTTP 请求失败", status);
        }

        std::vector<BYTE> extra;
        for(const SecBuffer& buffer : buffers) {
            if(buffer.BufferType == SECBUFFER_DATA && buffer.cbBuffer > 0U) {
                plaintext.append(static_cast<const char*>(buffer.pvBuffer), buffer.cbBuffer);
            } else if(buffer.BufferType == SECBUFFER_EXTRA && buffer.cbBuffer > 0U) {
                const auto* bytes = static_cast<const BYTE*>(buffer.pvBuffer);
                extra.assign(bytes, bytes + buffer.cbBuffer);
            }
        }
        encrypted = std::move(extra);
        if(plaintext.size() > kMaximumHttpRequestBytes) {
            throw std::runtime_error("解密后的 HTTP 请求超过测试上限");
        }
        const std::size_t complete_size = completeHttpSize(plaintext);
        if(complete_size != 0U) {
            plaintext.resize(complete_size);
            return plaintext;
        }
    }
}

// EncryptMessage 按 SChannel 报告的最大正文分块，避免假设 TLS 记录容量。
void sendTlsPlaintext(SOCKET socket, SecurityContext& context, const std::string& plaintext) {
    SecPkgContext_StreamSizes sizes{};
    const SECURITY_STATUS size_status = QueryContextAttributesW(&context.handle, SECPKG_ATTR_STREAM_SIZES, &sizes);
    if(size_status != SEC_E_OK) {
        throwSecurityError("读取 SChannel 流尺寸失败", size_status);
    }

    std::size_t offset = 0U;
    while(offset < plaintext.size()) {
        const std::size_t chunk_size = std::min<std::size_t>(plaintext.size() - offset, sizes.cbMaximumMessage);
        std::vector<BYTE> record(static_cast<std::size_t>(sizes.cbHeader) + chunk_size +
                                 static_cast<std::size_t>(sizes.cbTrailer));
        std::memcpy(record.data() + sizes.cbHeader, plaintext.data() + offset, chunk_size);

        SecBuffer buffers[4]{};
        buffers[0] = {sizes.cbHeader, SECBUFFER_STREAM_HEADER, record.data()};
        buffers[1] = {static_cast<unsigned long>(chunk_size), SECBUFFER_DATA, record.data() + sizes.cbHeader};
        buffers[2] = {sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, record.data() + sizes.cbHeader + chunk_size};
        buffers[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc message{SECBUFFER_VERSION, 4U, buffers};
        const SECURITY_STATUS status = EncryptMessage(&context.handle, 0U, &message, 0U);
        if(status != SEC_E_OK) {
            throwSecurityError("加密 TLS HTTP 响应失败", status);
        }
        const std::size_t encrypted_size = static_cast<std::size_t>(buffers[0].cbBuffer) +
                                           static_cast<std::size_t>(buffers[1].cbBuffer) +
                                           static_cast<std::size_t>(buffers[2].cbBuffer);
        sendAll(socket, record.data(), encrypted_size);
        offset += chunk_size;
    }
}

struct ParsedHttpRequest final {
    std::string method;
    std::string body;
};

ParsedHttpRequest parseHttpRequest(const std::string& request) {
    const std::size_t header_end = request.find("\r\n\r\n");
    std::istringstream request_line(request.substr(0U, request.find("\r\n")));
    ParsedHttpRequest parsed;
    std::string target;
    std::string version;
    request_line >> parsed.method >> target >> version;
    if(parsed.method.empty() || target != "/mcp" || header_end == std::string::npos) {
        throw std::runtime_error("TLS 回环夹具收到非法 HTTP 请求");
    }
    parsed.body = request.substr(header_end + 4U);
    return parsed;
}

std::string makeHttpResponse(int status, const char* reason, const char* content_type, const std::string& body,
                             const std::string& extra_headers = {}) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << ' ' << reason << "\r\n";
    if(content_type[0] != '\0') {
        response << "Content-Type: " << content_type << "\r\n";
    }
    response << extra_headers;
    response << "Content-Length: " << body.size() << "\r\nConnection: close\r\n\r\n" << body;
    return response.str();
}

// 最小 Server 仅对 initialize 返回 MCP JSON；控制通知与 Listener 降级保持可完成。
std::string buildMcpResponse(const ParsedHttpRequest& request) {
    if(request.method == "GET") {
        return makeHttpResponse(405, "Method Not Allowed", "text/plain; charset=utf-8", "不支持独立 Listener");
    }
    if(request.method == "DELETE") {
        return makeHttpResponse(200, "OK", "", "");
    }
    if(request.method != "POST") {
        return makeHttpResponse(405, "Method Not Allowed", "text/plain; charset=utf-8", "不支持的请求方法");
    }

    const nlohmann::json message = nlohmann::json::parse(request.body, nullptr, false);
    if(!message.is_object()) {
        return makeHttpResponse(400, "Bad Request", "text/plain; charset=utf-8", "请求正文不是 JSON 对象");
    }
    if(message.value("method", std::string{}) != "initialize") {
        return makeHttpResponse(202, "Accepted", "", "");
    }
    if(!message.contains("id")) {
        return makeHttpResponse(400, "Bad Request", "text/plain; charset=utf-8", "initialize 请求缺少 id");
    }

    const nlohmann::json result = {
        {"jsonrpc", "2.0"                                                        },
        {"id",      message.at("id")                                             },
        {"result",
         {{"protocolVersion", "2025-11-25"},
          {"capabilities", {{"tools", {{"listChanged", false}}}}},
          {"serverInfo", {{"name", "TLS 回环测试夹具"}, {"version", "1"}}}}}
    };
    return makeHttpResponse(200, "OK", "application/json; charset=utf-8", result.dump(),
                            "MCP-Session-Id: tls-test-session\r\n");
}

}  // namespace

struct McpTlsTestServer::Impl final {
    explicit Impl(McpTlsCertificateMode certificate_mode) : certificate_mode_(certificate_mode) {}

    ~Impl() {
        stop();
    }

    void start() {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if(worker_.joinable() || port_.load() != 0U) {
            throw std::logic_error("TLS 回环夹具已经启动");
        }

        // 每次启动都校验根 CA 和两套 Server 身份，避免未选中的固定资源悄然失效。
        validateRootCertificate();
        CertificateIdentity root_signed =
            importIdentity(mcpTlsCertificateResource(McpTlsCertificateMode::RootSigned), "根签名 Server 证书");
        CertificateIdentity self_signed =
            importIdentity(mcpTlsCertificateResource(McpTlsCertificateMode::SelfSigned), "自签名 Server 证书");
        identity_ = std::make_unique<CertificateIdentity>(
            certificate_mode_ == McpTlsCertificateMode::RootSigned ? std::move(root_signed) : std::move(self_signed));
        credential_ = std::make_unique<SChannelCredential>(identity_->certificate);

        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if(listener == kInvalidSocket) {
            cleanupIdentity();
            throw std::runtime_error("创建 TLS 回环监听 Socket 失败，Winsock 错误码=" +
                                     std::to_string(WSAGetLastError()));
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0U;
        if(bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
           listen(listener, 8) != 0) {
            const int error = WSAGetLastError();
            closeSocket(listener);
            cleanupIdentity();
            throw std::runtime_error("绑定 TLS 动态回环端口失败，Winsock 错误码=" + std::to_string(error));
        }

        sockaddr_in bound{};
        int bound_size = sizeof(bound);
        if(getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
            const int error = WSAGetLastError();
            closeSocket(listener);
            cleanupIdentity();
            throw std::runtime_error("读取 TLS 动态回环端口失败，Winsock 错误码=" + std::to_string(error));
        }

        listener_ = listener;
        stopping_.store(false);
        port_.store(ntohs(bound.sin_port));
        worker_ = std::thread([this, listener] { acceptLoop(listener); });
    }

    std::uint16_t endpointPort() const noexcept {
        return port_.load();
    }

    void stop() noexcept {
        std::thread worker;
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            stopping_.store(true);
            if(listener_ != kInvalidSocket) {
                shutdownSocket(listener_);
                closeSocket(listener_);
                listener_ = kInvalidSocket;
            }
            {
                std::lock_guard<std::mutex> active_lock(active_mutex_);
                shutdownSocket(active_socket_);
            }
            worker = std::move(worker_);
        }
        if(worker.joinable()) {
            worker.join();
        }
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        port_.store(0U);
        cleanupIdentity();
    }

   private:
    void cleanupIdentity() noexcept {
        credential_.reset();
        identity_.reset();
    }

    void acceptLoop(SOCKET listener) noexcept {
        while(!stopping_.load()) {
            sockaddr_in peer{};
            int peer_size = sizeof(peer);
            const SOCKET client = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
            if(client == kInvalidSocket) {
                if(stopping_.load()) {
                    return;
                }
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(active_mutex_);
                active_socket_ = client;
            }
            try {
                handleConnection(client);
            } catch(...) {
                // 自签名拒绝和错误主机名拒绝会主动中断握手，属于预期测试路径。
            }
            {
                std::lock_guard<std::mutex> lock(active_mutex_);
                if(active_socket_ == client) {
                    active_socket_ = kInvalidSocket;
                }
            }
            shutdownSocket(client);
            closeSocket(client);
        }
    }

    void handleConnection(SOCKET client) {
        // credential_ 在 stop 等待 worker 退出后才销毁，因此此处无需跨线程共享所有权。
        std::unique_ptr<SecurityContext> context = acceptTls(client, credential_->get());
        const ParsedHttpRequest request = parseHttpRequest(readTlsHttpRequest(client, *context));
        sendTlsPlaintext(client, *context, buildMcpResponse(request));
    }

    SocketRuntime socket_runtime_;
    McpTlsCertificateMode certificate_mode_;
    std::unique_ptr<CertificateIdentity> identity_;
    std::unique_ptr<SChannelCredential> credential_;
    std::atomic<bool> stopping_{true};
    std::atomic<std::uint16_t> port_{0U};
    SOCKET listener_ = kInvalidSocket;
    SOCKET active_socket_ = kInvalidSocket;
    std::thread worker_;
    mutable std::mutex lifecycle_mutex_;
    std::mutex active_mutex_;
};

McpTlsTestServer::McpTlsTestServer(McpTlsCertificateMode certificate_mode)
    : impl_(std::make_unique<Impl>(certificate_mode)) {}

McpTlsTestServer::~McpTlsTestServer() = default;

void McpTlsTestServer::start() {
    impl_->start();
}

std::uint16_t McpTlsTestServer::endpointPort() const noexcept {
    return impl_->endpointPort();
}

void McpTlsTestServer::stop() noexcept {
    impl_->stop();
}

}  // namespace aiSDK::test

#endif
