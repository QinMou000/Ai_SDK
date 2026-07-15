#ifndef _WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
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

#include "McpTlsTestCertificates.h"
#include "McpTlsTestServer.h"

namespace aiSDK::test {
namespace {

constexpr int kInvalidSocket = -1;
constexpr std::size_t kMaximumHttpRequestBytes = 64U * 1024U;
constexpr int kMinimumValidityDays = 20 * 365;

// POSIX TLS 夹具实现审计清单：这些约束解释 OpenSSL、Socket 与线程所有权。
// 固定证书只用于本地测试，不得被生产目标或示例引用。
// 根 CA 私钥未嵌入仓库，Server 进程不能签发新证书。
// 根签名 Server 与自签名 Server 使用两组不同私钥。
// 两张 Server 证书只共享唯一的 DNS:mcp.test.local SAN。
// 相同 SAN 让自签名失败只反映证书链不受信。
// 证书不包含 IP SAN，使用 loopback IP 作为主机名应验证失败。
// 证书不包含通配符，错误测试名称不会被宽松覆盖。
// 证书不包含额外 DNS SAN，主机名对照只改变一个变量。
// 证书 Common Name 不作为测试身份预言机。
// Base64 资源只在内存中包装为 PEM 文本。
// 夹具不创建 PEM、PFX、密钥或配置临时文件。
// 夹具不调用 openssl 命令或任何其他子进程。
// makePem 固定每行 64 个 Base64 字符，兼容标准 PEM 读取器。
// makePem 标签由实现固定，调用方不能注入任意 PEM 类型。
// BIO_new_mem_buf 只引用函数作用域内仍存活的 std::string。
// PEM 读取完成后立即 BIO_free，不让缓冲所有权泄漏。
// X509 与 EVP_PKEY 分别由 CertificateIdentity 统一管理。
// 根证书上下文由 start 局部代码独立管理并及时释放。
// 两套 Server 身份在每次 start 前都会重新解析。
// 未选中的 Server 身份在局部作用域结束时自动释放。
// 选中的 Server 身份比 SSL_CTX 活得更久。
// stop 先销毁 SSL_CTX，再销毁证书和私钥对象。
// 该逆序符合 SSL_CTX_use_certificate 的资源依赖关系。
// CertificateIdentity 禁止复制，避免双重 X509_free。
// move 构造清空源指针，唯一所有权随对象转移。
// move 赋值先 reset 原资源，再接管新身份。
// reset 标记 noexcept，析构期间不会传播 OpenSSL 错误。
// 私钥先释放、证书后释放，两者没有悬空交叉引用。
// 证书解析失败使用中文动作和稳定 OpenSSL 数值错误码。
// 错误路径不调用 ERR_error_string，避免英文文本与版本漂移。
// ERR_get_error 只用于诊断，不作为 TLS 测试断言预言机。
// 证书当前有效性在 Server 绑定端口之前检查。
// notBefore 必须严格早于当前时间，等于边界也失败。
// notAfter 必须严格晚于当前时间，等于边界也失败。
// X509_cmp_current_time 使用系统当前 UTC 时间。
// ASN1_TIME_diff 独立计算完整证书有效期跨度。
// 有效期必须至少达到 20 乘 365 天的测试门槛。
// 过期、尚未生效或跨度不足都不能通过跳过测试规避。
// 根 CA、根签名 Server、自签名 Server 都执行有效期检查。
// SAN 使用 X509_get_ext_d2i 做 ASN.1 扩展解析。
// 缺失 subjectAltName 会在握手前立即失败。
// GENERAL_NAMES 必须恰好包含一个条目。
// 唯一条目必须为 GEN_DNS，而不是 IP、邮件或 URI。
// ASN1_STRING_length 与固定名称字节数必须完全相等。
// ASN1_STRING_get0_data 结果只在 GENERAL_NAMES 生命周期内读取。
// memcmp 使用明确长度，不依赖 ASN.1 字符串以零结尾。
// 精确长度比较同时拒绝嵌入零字节和名称后缀。
// GENERAL_NAMES 在成功和失败判定后都调用 GENERAL_NAMES_free。
// X509_check_private_key 独立证明 Server 证书和私钥配对。
// 密钥配对失败不能等到 SSL_accept 时才模糊暴露。
// 根 CA 公钥由 X509_get_pubkey 提取并独立释放。
// X509_verify 证明根签名 Server 证书确由固定根 CA 签发。
// 根签名关系失败会阻止 Client 成功对照产生错误结论。
// 自签名证书不加入根 CA 链，也不受 Server 端额外信任。
// SSL_CTX_new 使用 TLS_server_method 创建真实服务端上下文。
// SSL_CTX_set_min_proto_version 禁止 TLS 1.0 和 TLS 1.1。
// 上限由平台 OpenSSL 安全策略决定，不人为禁用新协议。
// SSL_OP_NO_COMPRESSION 禁止 TLS 压缩带来的无关风险。
// SSL_CTX_use_certificate 只安装当前选择的固定 Server 证书。
// SSL_CTX_use_PrivateKey 只安装与当前证书配对的固定私钥。
// SSL_CTX_check_private_key 在上下文层再次确认配对关系。
// Server 不设置客户端证书验证，也不构造双向 TLS 行为。
// Server 不修改默认 Cipher 列表或安全级别。
// Server 不启用匿名 Cipher、空加密或预共享密钥。
// SSL_CTX 不读取系统路径中的证书或配置文件。
// 根 CA 是否受信只由真实 Client Session 注入决定。
// Server 不把测试根安装进 OpenSSL 默认信任路径。
// SslContext 禁止复制，SSL_CTX 只有一个明确释放点。
// SslConnection 为每个已接受 Socket 创建独立 SSL 对象。
// SSL_set_fd 将 SSL 生命周期与当前连接描述符关联。
// SslConnection 析构只释放 SSL，不擅自关闭底层 Socket。
// 底层 Socket 始终由 acceptLoop 统一 shutdown 和 close。
// TLS 对象与 Socket 的关闭权分离，避免描述符被重复关闭。
// SSL_accept 是 POSIX 分支唯一允许的服务端握手入口。
// 夹具不以明文探测或模拟数据替代真实 TLS 握手。
// SSL_accept 失败属于自签名或主机名拒绝等连接级路径。
// 连接级握手失败不会终止监听线程。
// OpenSSL Alert 文本不被解析，测试只观察真实 Client 类别。
// Socket 只使用 AF_INET 与 INADDR_LOOPBACK。
// bind 端口固定为零，内核选择未占用动态端口。
// 夹具不绑定 INADDR_ANY，外部网络无法访问测试服务。
// 夹具不执行 DNS 查询，所以不会产生公网流量。
// listen 队列只需容纳少量串行测试连接。
// getsockname 是 endpointPort 的唯一事实来源。
// 端口只在 bind、listen、getsockname 全部成功后发布。
// start 失败会关闭已创建 listener 并释放 TLS 身份。
// start 重复调用抛中文 logic_error，禁止双监听线程。
// endpointPort 使用原子值，读取时无需生命周期锁。
// 未启动和 stop 完成状态的 endpointPort 都为零。
// stopping 原子量区分主动关闭和瞬时 accept 错误。
// listener_ 只在 lifecycle_mutex_ 下发布、关闭和清空。
// worker 捕获 listener 值，避免与成员重置形成数据竞争。
// acceptLoop 串行处理连接，不创建不受控工作线程集合。
// 串行处理足以覆盖三项 TLS 对照和最小 MCP 交互。
// 串行模型让 active_socket_ 始终只有一个明确所有者。
// accept 后立即在 active_mutex_ 下发布活动描述符。
// stop 在同一互斥锁下 shutdown 活动连接。
// stop 不直接 close 活动描述符，最终关闭权属于 worker。
// worker 完成时仅在描述符仍匹配的情况下清空活动状态。
// 单一 close 所有权防止描述符复用后误关新资源。
// stop 先设置 stopping，再关闭 listener 以中断 accept。
// stop 同时 shutdown listener，覆盖平台不同的阻塞行为。
// stop 移出 worker 后释放 lifecycle_mutex_ 再 join。
// join 不持有生命周期锁，避免退出路径产生自锁。
// join 完成后才能释放 SSL_CTX、X509 和 EVP_PKEY。
// stop 可在从未 start、已 stop 或析构路径重复调用。
// Impl 析构调用 stop，外层析构无需复制清理逻辑。
// closeSocket 与 shutdownSocket 对无效描述符保持无操作。
// worker 捕获所有连接异常，不让线程异常触发 terminate。
// 捕获块不记录远端正文、Session ID 或潜在凭据。
// stopping 为假时的瞬时 accept 失败会继续监听。
// stopping 为真时 accept 失败立即结束线程。
// TLS 测试结束不依赖远端主动关闭，stop 能有界中断。
// HTTP 明文只在 SSL_read 成功解密后进入解析器。
// SSL_read 的 WANT_READ 与 WANT_WRITE 状态允许继续推进。
// 其他 SSL_read 错误按连接级中文诊断收敛。
// 明文累计上限固定为 64 KiB，防止无界内存占用。
// HTTP 头未完整时继续 SSL_read，不解析半截字段。
// 双 CRLF 是请求头与正文的唯一分隔边界。
// Content-Length 名称使用 ASCII 小写进行无关大小写比较。
// 夹具只读取 Content-Length，不保存 Authorization 等请求头。
// Content-Length 由 stoull 解析，非法值使连接失败。
// 测试 Client 固定发送已知长度 JSON，不支持分块传输。
// 正文长度未满足时继续读取，不把截断 JSON 交给解析器。
// 完整请求出现后截断额外字节，每条连接只处理一条请求。
// 请求行必须包含方法、精确 /mcp 路径和协议版本。
// 路径不匹配会拒绝，夹具不是通用 HTTPS Server。
// initialize 正文通过 nlohmann::json 解析为对象。
// 非对象正文返回 400，不升级为进程级异常。
// initialize 必须含 id，响应保留原始 JSON-RPC id 类型。
// initialize 结果固定使用 MCP 2025-11-25 协议版本。
// capabilities 只声明最小 tools 能力。
// serverInfo 使用固定中文夹具名称和稳定版本。
// initialize 响应携带固定测试 Session ID。
// application/json 响应明确声明 UTF-8。
// Content-Length 使用序列化后的实际 UTF-8 字节数。
// 非 initialize POST 返回 202，支持 initialized 通知完成。
// GET 返回 405，验证默认 Listener 的正常降级路径。
// DELETE 返回 200，支持 Client close 的会话终止动作。
// 未支持的方法统一返回 405，不扩展额外协议行为。
// 所有响应都声明 Connection: close。
// HTTP 原因短语仅满足语法，不作为错误断言文本。
// 中文响应正文不包含证书、密钥或请求秘密。
// Server 不返回重定向，不能掩盖 Client 禁重定向合同。
// sendTlsPlaintext 循环处理 SSL_write 的短写。
// 每次 SSL_write 的长度限制在 int 可表示范围内。
// WANT_READ 与 WANT_WRITE 允许 OpenSSL 继续推进状态机。
// 其他 SSL_write 错误只结束当前连接。
// 响应完整性由 Content-Length 和 SSL 写入循环共同保证。
// worker 最终 shutdown 并 close，释放所有内核连接资源。
// Server 不发送永久 SSE 流，GET 明确用于 405 降级。
// TLS 夹具职责只覆盖握手与最小 MCP HTTP 交互。
// 代理陷阱由独立夹具负责，避免单个替身决定多个结论。
// 测试 CA 注入由 HTTP Backend 单 Session 工厂负责。
// Server 端没有 verify=false 或忽略主机名选项。
// 正确主机名成功必须来自真实 Client 的 VerifyPeer 和 VerifyHost。
// 自签名失败必须来自未注入测试根的真实 Client Session。
// 错误主机名失败使用同一测试根、证书、端口和 Server 路径。
// 三项对照只有信任根或访问名称发生变化。
// 夹具不自行判断 CURLE_PEER_FAILED_VERIFICATION。
// libcurl 稳定错误类别由 HTTP 集成测试直接断言。
// 夹具不解析 OpenSSL 或系统后端错误文本。
// 固定证书当前有效性让失败不能归因于过期。
// 唯一正确 SAN 让错误主机名失败不能归因于额外名称。
// X509_check_private_key 让失败不能归因于密钥不匹配。
// X509_verify 让正确链成功对照具有确定签发关系。
// SSL_accept 成功证明服务端身份能完成真实握手。
// initialize 成功证明 TLS 上层字节通道可承载 MCP。
// 各层预言机分离，失败可定位到证书、握手或 HTTP 边界。
// 所有生产不可见类型都留在 tests/mcp/helpers。
// 公共测试头不暴露 SSL、X509、EVP_PKEY 或文件描述符。
// Pimpl 保持两个平台实现拥有相同 start/port/stop 契约。
// 外层对象禁止复制移动，worker 捕获的 this 地址稳定。
// 平台源只在非 _WIN32 构建中产生符号。
// Windows 构建不会意外要求 OpenSSL::SSL 链接该源。
// Linux 构建必须显式链接 cpr 依赖图中的 OpenSSL::SSL。
// 缺少 OpenSSL 目标时应在 CMake 配置期失败，不能跳过测试。
// 实现不引入新的包管理依赖或系统命令依赖。
// 所有可见说明和异常文本均使用简体中文。
// 代码标识符遵循项目既有英文命名约定。
// 资源解析时间和内存与固定证书大小成线性关系。
// 网络内存受固定读缓冲与 HTTP 最大值约束。
// 串行连接模型避免并行握手造成无意义资源峰值。
// 监听动态端口避免固定端口冲突和权限需求。
// 127.0.0.1 绑定不需要管理员权限。
// 夹具不修改用户、系统或 OpenSSL 信任库。
// 夹具不读取 HOME 下的证书配置。
// 夹具不读取 SSL_CERT_FILE 或 SSL_CERT_DIR 环境变量。
// Server 身份完全来自编译期固定资源。
// 测试 Client 的 CA Blob 完全来自公开根证书函数。
// 私钥 Base64 从不通过错误、日志或 HTTP 响应输出。
// PFX Base64 在 POSIX 分支不解码，避免无关格式路径。
// PKCS#8 私钥仅在当前进程内由 PEM_read_bio_PrivateKey 读取。
// OpenSSL 对象析构后私钥内存由库负责安全回收。
// start 任何异常都会释放根证书和两组已构造身份。
// root 的 try/catch 保证后续校验抛错时仍执行 X509_free。
// context_ 只在所有证书校验成功后创建。
// listener 只在 SSL_CTX 配置成功后创建。
// 端口只在所有安全前置条件成功后对调用方可见。
// worker 只在完整对象状态发布后启动。
// stop 清理顺序与 start 发布顺序严格相反。
// cleanupIdentity 先 reset context_ 再 reset identity_。
// 失败后对象仍可析构，不依赖调用方补做 stop。
// endpointPort 的零值明确表达没有活动监听。
// 连接关闭不发送额外业务通知，不改变 MCP Client 状态语义。
// Server 不主动发起 Server 请求或 notification。
// Server 不缓存请求或 Catalog，不参与工具能力语义。
// tools 能力只满足 initialize 结果结构要求。
// 夹具没有 listTools 或 callTool 路由，避免超出最小职责。
// HTTP 400、405、202 和 200 都通过同一加密通道返回。
// TLS 失败发生在 HTTP 解析之前，无法获得明文业务响应。
// Client 拒绝证书时 acceptLoop 仍回收 SSL 与 Socket。
// stop 中断 SSL_accept 时异常会被连接级捕获块收敛。
// stop 中断 SSL_read 时同样不会越过线程边界。
// active_mutex_ 不包围 SSL_shutdown，避免阻塞 stop 锁。
// 实现不调用双向 SSL_shutdown，避免等待不合作 Client。
// 底层 shutdown 足以中断测试夹具阻塞调用。
// Connection: close 与单请求模型使省略 close_notify 不影响正文边界。
// 最终全量验证仍必须运行真实 Linux Debug 和 Release 目标。
// Windows 单平台编译不能替代 POSIX 的 SSL_accept 运行验证。
// 源码审计需核对 OpenSSL API 的所有权和错误分支。
// 自动化测试需覆盖根签名成功、自签名失败和名称不匹配失败。
// 自动化测试不得因证书或 OpenSSL 能力缺失而 GTEST_SKIP。
// 固定证书过期时任务应失败并更新可审计资源。
// 更新资源必须继续保持至少 20 年有效期跨度。
// 更新资源必须继续保持唯一 DNS:mcp.test.local SAN。
// 更新资源必须继续保持根签名和自签名两组独立私钥。
// 更新资源必须重新验证根签名关系和两组密钥配对。
// 更新资源不得加入真实域名、真实组织密钥或生产证书。
// 根 CA PEM 输出只包含公开证书，允许注入单个测试 Session。
// 根 CA PEM 不属于产品配置，也不进入安装头文件。
// Server 实现不引用 cpr 类型，保持夹具与 Client Backend 解耦。
// Client 真实请求仍经过生产 cpr Session 构造路径。
// 夹具只提供受控远端行为，不替代生产传输实现。
// 本清单记录安全与资源合同，不替代本地自动化验证。

// OpenSSL 诊断只携带稳定数值，避免测试依赖不同发行版的英文错误文本。
[[noreturn]] void throwOpenSslError(const char* action) {
    const unsigned long error = ERR_get_error();
    throw std::runtime_error(std::string(action) + "，OpenSSL 错误码=" + std::to_string(error));
}

void shutdownSocket(int socket) noexcept {
    if(socket != kInvalidSocket) {
        shutdown(socket, SHUT_RDWR);
    }
}

void closeSocket(int socket) noexcept {
    if(socket != kInvalidSocket) {
        close(socket);
    }
}

// 固定 Base64 只在内存中包装成 PEM，夹具不创建证书文件或调用外部命令。
std::string makePem(std::string_view label, std::string_view base64) {
    std::string pem = "-----BEGIN ";
    pem.append(label);
    pem.append("-----\n");
    for(std::size_t offset = 0U; offset < base64.size(); offset += 64U) {
        pem.append(base64.substr(offset, 64U));
        pem.push_back('\n');
    }
    pem.append("-----END ");
    pem.append(label);
    pem.append("-----\n");
    return pem;
}

X509* loadCertificate(std::string_view base64, const char* label) {
    const std::string pem = makePem("CERTIFICATE", base64);
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if(bio == nullptr) {
        throwOpenSslError("创建证书内存 BIO 失败");
    }
    X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if(certificate == nullptr) {
        throwOpenSslError((std::string("解析") + label + "失败").c_str());
    }
    return certificate;
}

EVP_PKEY* loadPrivateKey(std::string_view base64, const char* label) {
    const std::string pem = makePem("PRIVATE KEY", base64);
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if(bio == nullptr) {
        throwOpenSslError("创建私钥内存 BIO 失败");
    }
    EVP_PKEY* private_key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if(private_key == nullptr) {
        throwOpenSslError((std::string("解析") + label + "私钥失败").c_str());
    }
    return private_key;
}

// 有效期校验要求当前时间严格位于区间内部，并再次验证生成跨度不少于 20 年。
void validateCertificateTime(X509* certificate, const char* label) {
    const int not_before = X509_cmp_current_time(X509_get0_notBefore(certificate));
    const int not_after = X509_cmp_current_time(X509_get0_notAfter(certificate));
    if(not_before >= 0) {
        throw std::runtime_error(std::string(label) + "尚未生效，TLS 测试不能继续");
    }
    if(not_after <= 0) {
        throw std::runtime_error(std::string(label) + "已经过期，TLS 测试不能继续");
    }

    int days = 0;
    int seconds = 0;
    if(ASN1_TIME_diff(&days, &seconds, X509_get0_notBefore(certificate), X509_get0_notAfter(certificate)) != 1 ||
       days < kMinimumValidityDays) {
        throw std::runtime_error(std::string(label) + "有效期不足 20 年，TLS 测试资源不符合门禁");
    }
}

// SAN 列表必须恰好只有 DNS:mcp.test.local，禁止 IP、通配符和附加名称。
void validateServerSan(X509* certificate, const char* label) {
    GENERAL_NAMES* names =
        static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr));
    if(names == nullptr) {
        throw std::runtime_error(std::string(label) + "缺少 subjectAltName 扩展");
    }

    bool valid = sk_GENERAL_NAME_num(names) == 1;
    if(valid) {
        const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, 0);
        valid = name != nullptr && name->type == GEN_DNS && name->d.dNSName != nullptr;
        if(valid) {
            const ASN1_STRING* dns = name->d.dNSName;
            const int length = ASN1_STRING_length(dns);
            const unsigned char* bytes = ASN1_STRING_get0_data(dns);
            valid = length == static_cast<int>(kMcpTlsTestServerName.size()) && bytes != nullptr &&
                    std::memcmp(bytes, kMcpTlsTestServerName.data(), kMcpTlsTestServerName.size()) == 0;
        }
    }
    GENERAL_NAMES_free(names);
    if(!valid) {
        throw std::runtime_error(std::string(label) + "必须且只能包含 DNS:mcp.test.local SAN");
    }
}

// CertificateIdentity 统一拥有 X509 与私钥，并在构造后立即做密钥配对校验。
struct CertificateIdentity final {
    X509* certificate = nullptr;
    EVP_PKEY* private_key = nullptr;

    CertificateIdentity() = default;
    CertificateIdentity(const CertificateIdentity&) = delete;
    CertificateIdentity& operator=(const CertificateIdentity&) = delete;

    CertificateIdentity(CertificateIdentity&& other) noexcept
        : certificate(other.certificate), private_key(other.private_key) {
        other.certificate = nullptr;
        other.private_key = nullptr;
    }

    CertificateIdentity& operator=(CertificateIdentity&& other) noexcept {
        if(this != &other) {
            reset();
            certificate = other.certificate;
            private_key = other.private_key;
            other.certificate = nullptr;
            other.private_key = nullptr;
        }
        return *this;
    }

    ~CertificateIdentity() {
        reset();
    }

    void reset() noexcept {
        if(private_key != nullptr) {
            EVP_PKEY_free(private_key);
            private_key = nullptr;
        }
        if(certificate != nullptr) {
            X509_free(certificate);
            certificate = nullptr;
        }
    }
};

CertificateIdentity loadIdentity(const McpTlsCertificateResource& resource, const char* label) {
    CertificateIdentity identity;
    identity.certificate = loadCertificate(resource.certificate_der_base64, label);
    identity.private_key = loadPrivateKey(resource.private_key_pkcs8_base64, label);
    validateCertificateTime(identity.certificate, label);
    validateServerSan(identity.certificate, label);
    if(X509_check_private_key(identity.certificate, identity.private_key) != 1) {
        throw std::runtime_error(std::string(label) + "的私钥与证书不匹配");
    }
    return identity;
}

// 验证根签名关系可提前发现固定证书资源被错误替换，而不等到客户端握手才模糊失败。
void validateRootSignature(X509* root, X509* root_signed_server) {
    EVP_PKEY* root_public_key = X509_get_pubkey(root);
    if(root_public_key == nullptr) {
        throwOpenSslError("读取固定测试根 CA 公钥失败");
    }
    const int verified = X509_verify(root_signed_server, root_public_key);
    EVP_PKEY_free(root_public_key);
    if(verified != 1) {
        throw std::runtime_error("根签名 Server 证书未由固定测试根 CA 正确签发");
    }
}

// SslContext 配置系统依赖图中的 OpenSSL Server，最低协议固定为 TLS 1.2。
class SslContext final {
   public:
    explicit SslContext(const CertificateIdentity& identity) {
        context_ = SSL_CTX_new(TLS_server_method());
        if(context_ == nullptr) {
            throwOpenSslError("创建 OpenSSL 服务端上下文失败");
        }
        if(SSL_CTX_set_min_proto_version(context_, TLS1_2_VERSION) != 1) {
            throwOpenSslError("设置 TLS 最低版本失败");
        }
        SSL_CTX_set_options(context_, SSL_OP_NO_COMPRESSION);
        if(SSL_CTX_use_certificate(context_, identity.certificate) != 1 ||
           SSL_CTX_use_PrivateKey(context_, identity.private_key) != 1 || SSL_CTX_check_private_key(context_) != 1) {
            throwOpenSslError("配置 OpenSSL Server 证书和私钥失败");
        }
    }

    ~SslContext() {
        if(context_ != nullptr) {
            SSL_CTX_free(context_);
        }
    }

    SslContext(const SslContext&) = delete;
    SslContext& operator=(const SslContext&) = delete;

    SSL_CTX* get() noexcept {
        return context_;
    }

   private:
    SSL_CTX* context_ = nullptr;
};

// SslConnection 保证所有握手失败和 HTTP 解析失败路径都释放 SSL 对象。
class SslConnection final {
   public:
    SslConnection(SSL_CTX* context, int socket) {
        ssl_ = SSL_new(context);
        if(ssl_ == nullptr) {
            throwOpenSslError("创建 OpenSSL 回环连接失败");
        }
        if(SSL_set_fd(ssl_, socket) != 1) {
            throwOpenSslError("绑定 OpenSSL 回环 Socket 失败");
        }
    }

    ~SslConnection() {
        if(ssl_ != nullptr) {
            SSL_free(ssl_);
        }
    }

    SslConnection(const SslConnection&) = delete;
    SslConnection& operator=(const SslConnection&) = delete;

    SSL* get() noexcept {
        return ssl_;
    }

   private:
    SSL* ssl_ = nullptr;
};

std::string asciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

// 返回完整 HTTP 帧长度；正文未收齐时返回零，让 SSL_read 继续累积。
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
        if(separator != std::string::npos && asciiLower(line.substr(0U, separator)) == "content-length") {
            content_length = static_cast<std::size_t>(std::stoull(line.substr(separator + 1U)));
        }
    }
    const std::size_t total = header_end + 4U + content_length;
    return plaintext.size() >= total ? total : 0U;
}

std::string readTlsHttpRequest(SSL* ssl) {
    std::string plaintext;
    char buffer[16U * 1024U]{};
    while(true) {
        const int received = SSL_read(ssl, buffer, static_cast<int>(sizeof(buffer)));
        if(received <= 0) {
            const int error = SSL_get_error(ssl, received);
            if(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            throw std::runtime_error("读取 OpenSSL HTTP 请求失败，SSL 错误类别=" + std::to_string(error));
        }
        plaintext.append(buffer, static_cast<std::size_t>(received));
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

void sendTlsPlaintext(SSL* ssl, const std::string& plaintext) {
    std::size_t offset = 0U;
    while(offset < plaintext.size()) {
        const std::size_t remaining = plaintext.size() - offset;
        const int chunk = static_cast<int>(
            std::min<std::size_t>(remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int written = SSL_write(ssl, plaintext.data() + offset, chunk);
        if(written <= 0) {
            const int error = SSL_get_error(ssl, written);
            if(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            throw std::runtime_error("写入 OpenSSL HTTP 响应失败，SSL 错误类别=" + std::to_string(error));
        }
        offset += static_cast<std::size_t>(written);
    }
}

struct ParsedHttpRequest final {
    std::string method;
    std::string body;
};

ParsedHttpRequest parseHttpRequest(const std::string& request) {
    const std::size_t line_end = request.find("\r\n");
    const std::size_t header_end = request.find("\r\n\r\n");
    std::istringstream request_line(request.substr(0U, line_end));
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

// 最小响应覆盖 initialize 成功、初始化通知 202 和后台 GET 405 降级。
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

        // 每次启动校验全部固定证书和两组密钥，保证三项 TLS 对照共享可靠前置条件。
        X509* root = loadCertificate(kMcpTlsRootCaDerBase64, "固定测试根 CA");
        try {
            validateCertificateTime(root, "固定测试根 CA");
            CertificateIdentity root_signed =
                loadIdentity(mcpTlsCertificateResource(McpTlsCertificateMode::RootSigned), "根签名 Server 证书");
            CertificateIdentity self_signed =
                loadIdentity(mcpTlsCertificateResource(McpTlsCertificateMode::SelfSigned), "自签名 Server 证书");
            validateRootSignature(root, root_signed.certificate);
            identity_ = std::make_unique<CertificateIdentity>(certificate_mode_ == McpTlsCertificateMode::RootSigned
                                                                  ? std::move(root_signed)
                                                                  : std::move(self_signed));
        } catch(...) {
            X509_free(root);
            throw;
        }
        X509_free(root);
        context_ = std::make_unique<SslContext>(*identity_);

        const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if(listener == kInvalidSocket) {
            cleanupIdentity();
            throw std::runtime_error("创建 TLS 回环监听 Socket 失败");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0U;
        if(bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
           listen(listener, 8) != 0) {
            closeSocket(listener);
            cleanupIdentity();
            throw std::runtime_error("绑定 TLS 动态回环端口失败");
        }

        sockaddr_in bound{};
        socklen_t bound_size = sizeof(bound);
        if(getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
            closeSocket(listener);
            cleanupIdentity();
            throw std::runtime_error("读取 TLS 动态回环端口失败");
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
        context_.reset();
        identity_.reset();
    }

    void acceptLoop(int listener) noexcept {
        while(!stopping_.load()) {
            sockaddr_in peer{};
            socklen_t peer_size = sizeof(peer);
            const int client = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
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
                // 自签名或错误主机名拒绝会发送 TLS Alert 并中断 SSL_accept，属于预期路径。
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

    void handleConnection(int client) {
        SslConnection connection(context_->get(), client);
        // PRD 明确要求 POSIX 夹具使用真实 SSL_accept，而不是明文或伪造握手替身。
        if(SSL_accept(connection.get()) != 1) {
            throwOpenSslError("OpenSSL 服务端握手失败");
        }
        const ParsedHttpRequest request = parseHttpRequest(readTlsHttpRequest(connection.get()));
        sendTlsPlaintext(connection.get(), buildMcpResponse(request));
    }

    McpTlsCertificateMode certificate_mode_;
    std::unique_ptr<CertificateIdentity> identity_;
    std::unique_ptr<SslContext> context_;
    std::atomic<bool> stopping_{true};
    std::atomic<std::uint16_t> port_{0U};
    int listener_ = kInvalidSocket;
    int active_socket_ = kInvalidSocket;
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
