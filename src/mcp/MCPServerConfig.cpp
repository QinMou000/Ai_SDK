#include "mcp/MCPServerConfig.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace aiSDK {
namespace {

// 本文件只执行静态配置校验，不创建进程、解析 DNS 或发出网络请求。
// 构造 MCPClient 时即可发现的错误必须在任何外部副作用之前稳定失败。
// 校验函数按值读取配置，不修改 Provider、路径、环境或调用方容器。
// 所有失败使用 std::invalid_argument，运行期传输错误由其他层表达。
// 诊断文本不包含凭据值，也不回显完整参数、环境或 Endpoint 查询内容。
// server_id 是本地非敏感标识，仍拒绝会破坏日志边界的控制字符。
// 公共 timeout 使用严格正值，避免零值在不同后端产生不同特殊语义。
// 公共容量使用严格非零值，不能通过配置关闭资源保护。
// absolute_request_timeout 是重试总界，不得依赖单次 timeout 无限续期。
// close_timeout 为 Provider、Worker 与句柄回收提供统一有界预算。
// max_message_bytes 限制单条解析正文，不代表可以无限排队多条消息。
// max_pending_messages 独立约束队列深度，与单消息上限共同保护内存。
// max_error_text_bytes 限制公开诊断，避免远端正文进入模型上下文。
// max_pages 和 max_tools 共同限制 tools/list 的分页时间与目录体积。
// stdio executable 必须是绝对路径，禁止依赖启动进程的当前目录搜索。
// 静态层不要求 executable 已存在，存在性与可执行性属于 connect 阶段。
// executable 中的 NUL 会截断平台 API，必须在创建管道前拒绝。
// executable 中的 CR/LF 会破坏日志与命令边界，同样静态拒绝。
// .cmd 与 .bat 需要 shell 解释，首版要求调用方显式指定真实解释器。
// 扩展名按 ASCII 不区分大小写比较，Windows 常见大小写均被覆盖。
// stdio 参数按 argv 元素传递，不拼接 shell 命令，也不做二次解析。
// 参数可包含普通空格和 Unicode，但不能包含 NUL、CR 或 LF 边界字节。
// working_directory 为空表示明确沿用应用当前目录，不做父目录扫描。
// 非空 working_directory 必须是绝对路径，避免运行环境隐式改变含义。
// working_directory 的 NUL 不能到达 CreateProcessW、chdir 等平台 API。
// working_directory 的 CR/LF 即使文件系统允许也会破坏安全诊断边界。
// 静态层不探测 working_directory 是否存在，实际进程后端在启动前复核。
// 环境变量名称不能为空，否则平台环境块无法形成稳定键值映射。
// 环境变量名称不能含等号，等号是 Windows 与 POSIX 环境条目的分隔符。
// 环境变量名称和值都拒绝 NUL，避免后缀被静默截断。
// 环境变量名称和值都拒绝 CR/LF，避免日志和环境块边界注入。
// inherit_parent_environment 只控制继承策略，不绕过显式环境项校验。
// startup_timeout 限制进程握手等待，不能使用零或负值表达无限等待。
// shutdown_timeout 限制 EOF、终止与回收流程，close 不能永久阻塞。
// stderr 尾部上限必须非零，既保留诊断又防止 Server 输出耗尽内存。
// HTTP Endpoint 只允许明确的 http 或 https Scheme，不接受相对 URL。
// Scheme 采用 ASCII 小写规范化，不修改路径、查询或主机之外的正文。
// Endpoint 不能为空，也不能含 NUL、DEL、ASCII 空白或其他控制字符。
// Fragment 不会发送给 Server，允许它会造成配置表象与实际请求不一致。
// URL 用户信息可能泄漏静态秘密，因此 authority 中出现 at 符号即拒绝。
// 首版拒绝 authority 百分号转义，避免安全判断与后端解码结果分叉。
// 主机不能为空，不能依赖 HTTP 库用默认来源补齐缺失 authority。
// IPv6 字面量必须使用方括号，避免冒号同时被解释为端口分隔符。
// IPv6 Zone ID 与网卡环境相关，首版通过百分号禁令整体拒绝。
// 非方括号 authority 出现多个冒号时按畸形 IPv6 处理。
// 端口必须是十进制数字，不接受正负号、十六进制或服务名称。
// 端口逐位累积并先检查溢出，异常长输入不会触发无符号回绕。
// 端口零无可用服务语义，合法范围固定为一到六万五千五百三十五。
// HTTPS 默认允许远程主机，但后端仍必须启用系统信任和主机名校验。
// 明文 HTTP 默认关闭，只有显式开关与字面量 loopback 同时满足才允许。
// localhost 不按回环处理，避免 DNS、hosts 或搜索域改变安全结论。
// IPv4 回环只接受标准四段十进制，并覆盖完整 127/8 地址范围。
// 简写 IPv4、整数 IPv4 与十六进制 IPv4 都不会通过保守解析器。
// IPv4 每段必须一到三位且不大于 255，段数必须恰好为四。
// IPv6 明文回环只接受规范 authority 中去括号后的 ::1。
// HTTP connect_timeout 限制建立连接与 TLS 握手的前置阶段。
// stream_idle_timeout 限制 SSE 无数据空闲，不等同于整个 Listener 寿命。
// credential_timeout 限制应用 Provider，Provider 不能拖延 close 超过预算。
// credential_timeout 不得大于 close_timeout，保证关闭可等待 Provider 退出。
// max_sse_retry_delay 限制 Server retry 建议，避免异常值制造长时间失联。
// max_sse_event_bytes 不能超过完整消息上限，事件层不能绕过正文保护。
// max_event_id_bytes 限制 Last-Event-ID 状态，避免重连 Header 无界增长。
// max_session_id_bytes 限制 Server 分配的会话 Header，避免会话状态放大。
// max_reconnect_attempts 必须非零，Listener 恢复不能退化为无保护热循环。
// 凭据 variant 只允许匿名、Bearer 或固定自定义 Header 三种明确模式。
// Anonymous 不读取 Provider，也不会自动采用环境中的用户名或令牌。
// Bearer 必须持有 Provider，秘密只在具体请求准备阶段按需获取。
// 固定 Header 同样必须持有 Provider，配置对象不直接保存秘密正文。
// 配置校验绝不调用 Provider，避免构造 Client 就产生外部副作用。
// Header 名按 RFC token 字符集校验，空格、冒号与非 ASCII 均拒绝。
// Header 名比较采用 ASCII 不区分大小写，与 HTTP 字段语义一致。
// Authorization 由 Bearer 模式控制，固定 Header 不能覆盖它。
// Host、Content-Type 与 Content-Length 由传输控制，应用不能篡改协议边界。
// Accept、Connection 与 Transfer-Encoding 由传输控制，避免响应协商分叉。
// MCP-Protocol-Version 与 MCP-Session-Id 是 MCP 状态字段，禁止凭据覆盖。
// Last-Event-ID 属于 SSE 恢复状态，禁止凭据 Provider 注入任意游标。
// Cookie 与 Set-Cookie 不开放，首版不会隐式引入浏览器会话语义。
// Proxy-Authorization 及整个 proxy- 前缀保留，防止代理秘密混入源站凭据。
// Origin、User-Agent 与 Upgrade 等连接字段也由 SDK 保持单一来源。
// Provider 指针只验证非空，不推断线程安全或返回值有效性。
// Provider 返回空值、超时或异常属于运行期 CredentialUnavailable 语义。
// 固定 Header 的值仍在请求阶段检查控制字符，静态层无法提前读取。
// URLParts 只保存校验需要的 scheme 与 host，不形成第二套 HTTP 解析器。
// Endpoint 最终仍由传输后端解析，但必须先通过这里的保守安全子集。
// 静态校验不规范化路径和查询，避免签名、路由或服务器语义被改写。
// 静态校验不跟随重定向，生产传输也应保持重定向关闭。
// 静态校验不读取系统代理变量，生产传输必须显式关闭环境代理继承。
// 静态校验不读取证书文件，生产 TLS 只使用系统信任库。
// 测试专用 CA 或解析覆盖不能通过公共配置结构进入生产路径。
// 所有字符串扫描按字节识别 ASCII 边界，Unicode 正文保持原样。
// std::filesystem::path 转 UTF-8 仅用于控制字符检查，不改变原生路径对象。
// Windows 宽字符路径在进程后端继续保留，配置层不会提前窄化启动参数。
// validateMCPServerConfig 先校验公共字段，再按 variant 校验唯一传输分支。
// variant 类型保证 stdio 与 HTTP 配置互斥，不需要运行期字符串分派。
// 校验顺序只影响首个诊断，不能被调用方当作字段优先级协议。
// 应用修改配置后应重新构造 Client，不支持绕过校验热更新内部传输。
// 这些规则优先选择确定性和可审计性，不追求接受所有宽松 URL 写法。
// requirePositive 使用毫秒计数判断，负值和零值共享同一配置错误类别。
// requireNonZero 只负责通用零值合同，字段之间的关系由对应传输校验器处理。
// asciiLower 只传入 unsigned char，避免高位字节触发区域相关未定义行为。
// Header token 校验显式列出 tchar，结果不受当前系统 locale 影响。
// parsePort 不调用 stoi，避免异常类型和有符号范围依赖标准库实现。
// loopback IPv4 解析不做 DNS I/O，因此静态校验可确定且可离线测试。
// 公共入口没有缓存，重复校验同一不可变配置会得到相同结果。

// URLParts 只保存静态安全校验所需字段，避免公共配置层依赖具体 HTTP 后端。
// Endpoint 的完整解析仍由传输库执行，但任何网络 I/O 前必须先通过这里的保守检查。
struct URLParts {
    std::string scheme;
    std::string host;
};

std::string asciiLower(std::string value) {
    // HTTP Scheme、Header 名和 Windows 扩展名都采用 ASCII 不区分大小写语义。
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool containsForbiddenTextByte(std::string_view value) {
    // NUL 会截断平台 API，CR/LF 会制造 Header 或命令字段边界，配置层统一拒绝。
    return value.find('\0') != std::string_view::npos || value.find('\r') != std::string_view::npos ||
           value.find('\n') != std::string_view::npos;
}

void requirePositive(std::chrono::milliseconds value, const char* field_name) {
    if(value.count() <= 0) {
        throw std::invalid_argument(std::string(field_name) + " 必须为正值");
    }
}

void requireNonZero(std::size_t value, const char* field_name) {
    if(value == 0U) {
        throw std::invalid_argument(std::string(field_name) + " 必须大于零");
    }
}

bool isHeaderTokenCharacter(unsigned char character) {
    // RFC 9110 field-name 使用 token；显式列出 tchar，拒绝空格、冒号和非 ASCII。
    if((character >= static_cast<unsigned char>('A') && character <= static_cast<unsigned char>('Z')) ||
       (character >= static_cast<unsigned char>('a') && character <= static_cast<unsigned char>('z')) ||
       (character >= static_cast<unsigned char>('0') && character <= static_cast<unsigned char>('9'))) {
        return true;
    }
    constexpr std::string_view symbols = "!#$%&'*+-.^_`|~";
    return symbols.find(static_cast<char>(character)) != std::string_view::npos;
}

void validateCredential(const MCPHttpCredential& credential) {
    if(const auto* bearer = std::get_if<MCPBearerCredential>(&credential)) {
        // Provider 是每次请求取得秘密值的唯一入口；空指针会把失败推迟到 I/O 阶段。
        if(!bearer->provider) {
            throw std::invalid_argument("Bearer 凭据 Provider 不能为空");
        }
        return;
    }

    const auto* fixed_header = std::get_if<MCPFixedHeaderCredential>(&credential);
    if(fixed_header == nullptr) {
        // Anonymous 不携带任何额外状态，因此无需进一步校验。
        return;
    }
    if(!fixed_header->provider) {
        throw std::invalid_argument("固定 Header 凭据 Provider 不能为空");
    }
    if(fixed_header->header_name.empty() ||
       !std::all_of(fixed_header->header_name.begin(), fixed_header->header_name.end(),
                    [](unsigned char character) { return isHeaderTokenCharacter(character); })) {
        throw std::invalid_argument("固定凭据 Header 名称不是合法 HTTP token");
    }

    // 这些字段由 HTTP/MCP 传输自身控制。允许凭据覆盖会破坏来源、会话、正文或连接边界。
    static const std::unordered_set<std::string> reserved_headers = {"accept",
                                                                     "accept-encoding",
                                                                     "authorization",
                                                                     "connection",
                                                                     "content-length",
                                                                     "content-type",
                                                                     "cookie",
                                                                     "cookie2",
                                                                     "host",
                                                                     "keep-alive",
                                                                     "last-event-id",
                                                                     "mcp-protocol-version",
                                                                     "mcp-session-id",
                                                                     "origin",
                                                                     "proxy-authenticate",
                                                                     "proxy-authorization",
                                                                     "set-cookie",
                                                                     "te",
                                                                     "trailer",
                                                                     "transfer-encoding",
                                                                     "upgrade",
                                                                     "user-agent"};
    const std::string lower_name = asciiLower(fixed_header->header_name);
    if(reserved_headers.find(lower_name) != reserved_headers.end() || lower_name.rfind("proxy-", 0U) == 0U) {
        throw std::invalid_argument("固定凭据 Header 名称属于 SDK 保留字段");
    }
}

unsigned int parsePort(std::string_view port_text) {
    if(port_text.empty()) {
        throw std::invalid_argument("MCP Endpoint 端口不能为空");
    }

    unsigned int port = 0U;
    for(const unsigned char character : port_text) {
        if(std::isdigit(character) == 0) {
            throw std::invalid_argument("MCP Endpoint 端口必须为十进制数字");
        }
        // 逐位检查避免无符号溢出，也把非法端口挡在网络解析之前。
        const unsigned int digit = static_cast<unsigned int>(character - static_cast<unsigned char>('0'));
        if(port > (std::numeric_limits<unsigned int>::max() - digit) / 10U) {
            throw std::invalid_argument("MCP Endpoint 端口超出范围");
        }
        port = port * 10U + digit;
    }
    if(port == 0U || port > 65535U) {
        throw std::invalid_argument("MCP Endpoint 端口必须位于 1 到 65535");
    }
    return port;
}

URLParts parseEndpoint(const std::string& endpoint) {
    if(endpoint.empty() || containsForbiddenTextByte(endpoint)) {
        throw std::invalid_argument("MCP Endpoint 不能为空且不能包含 NUL、回车或换行");
    }
    if(std::any_of(endpoint.begin(), endpoint.end(),
                   [](unsigned char character) { return character <= 0x20U || character == 0x7FU; })) {
        throw std::invalid_argument("MCP Endpoint 不能包含空白或 ASCII 控制字符");
    }
    if(endpoint.find('#') != std::string::npos) {
        throw std::invalid_argument("MCP Endpoint 不能包含 URL Fragment");
    }

    const std::size_t scheme_end = endpoint.find("://");
    if(scheme_end == std::string::npos || scheme_end == 0U) {
        throw std::invalid_argument("MCP Endpoint 必须包含明确的 http 或 https Scheme");
    }
    URLParts result;
    result.scheme = asciiLower(endpoint.substr(0U, scheme_end));
    if(result.scheme != "https" && result.scheme != "http") {
        throw std::invalid_argument("MCP Endpoint 只支持 https 或受限 loopback http");
    }

    const std::size_t authority_start = scheme_end + 3U;
    const std::size_t authority_end = endpoint.find_first_of("/?", authority_start);
    const std::string authority = endpoint.substr(authority_start, authority_end - authority_start);
    if(authority.empty() || authority.find('@') != std::string::npos || authority.find('%') != std::string::npos) {
        throw std::invalid_argument("MCP Endpoint 必须包含不带用户信息和转义的明确主机");
    }

    if(authority.front() == '[') {
        // IPv6 字面量必须使用方括号；首版拒绝 Zone ID，防止来源判断依赖网卡环境。
        const std::size_t bracket_end = authority.find(']');
        if(bracket_end == std::string::npos || bracket_end == 1U) {
            throw std::invalid_argument("MCP Endpoint 的 IPv6 主机格式非法");
        }
        result.host = asciiLower(authority.substr(1U, bracket_end - 1U));
        const std::string_view suffix(authority.data() + bracket_end + 1U, authority.size() - bracket_end - 1U);
        if(!suffix.empty()) {
            if(suffix.front() != ':') {
                throw std::invalid_argument("MCP Endpoint 的 IPv6 主机后存在非法内容");
            }
            static_cast<void>(parsePort(suffix.substr(1U)));
        }
    } else {
        if(std::count(authority.begin(), authority.end(), ':') > 1) {
            throw std::invalid_argument("MCP Endpoint 的 IPv6 主机必须使用方括号");
        }
        const std::size_t port_separator = authority.find(':');
        result.host = asciiLower(authority.substr(0U, port_separator));
        if(port_separator != std::string::npos) {
            static_cast<void>(parsePort(std::string_view(authority).substr(port_separator + 1U)));
        }
    }
    if(result.host.empty() || result.host.find_first_of(" \t") != std::string::npos) {
        throw std::invalid_argument("MCP Endpoint 主机不能为空或包含空白");
    }
    return result;
}

bool isLiteralIpv4Loopback(const std::string& host) {
    // 只接受标准四段十进制写法，拒绝 127.1、整数或十六进制等解析器相关等价形式。
    std::array<unsigned int, 4U> octets{};
    std::size_t segment_start = 0U;
    for(std::size_t index = 0U; index < octets.size(); ++index) {
        const std::size_t segment_end = host.find('.', segment_start);
        if((segment_end == std::string::npos) != (index == octets.size() - 1U)) {
            return false;
        }
        const std::size_t length = (segment_end == std::string::npos ? host.size() : segment_end) - segment_start;
        if(length == 0U || length > 3U) {
            return false;
        }
        unsigned int value = 0U;
        for(std::size_t offset = 0U; offset < length; ++offset) {
            const unsigned char character = static_cast<unsigned char>(host[segment_start + offset]);
            if(std::isdigit(character) == 0) {
                return false;
            }
            value = value * 10U + static_cast<unsigned int>(character - static_cast<unsigned char>('0'));
        }
        if(value > 255U) {
            return false;
        }
        octets[index] = value;
        segment_start = segment_end == std::string::npos ? host.size() : segment_end + 1U;
    }
    return octets[0] == 127U;
}

void validateCommonLimits(const MCPCommonLimits& limits) {
    requirePositive(limits.request_timeout, "request_timeout");
    requirePositive(limits.absolute_request_timeout, "absolute_request_timeout");
    requirePositive(limits.close_timeout, "close_timeout");
    requireNonZero(limits.max_message_bytes, "max_message_bytes");
    requireNonZero(limits.max_pending_messages, "max_pending_messages");
    requireNonZero(limits.max_error_text_bytes, "max_error_text_bytes");
    requireNonZero(limits.max_pages, "max_pages");
    requireNonZero(limits.max_tools, "max_tools");
}

void validateStdioConfig(const MCPStdioServerConfig& config) {
    if(config.executable.empty() || !config.executable.is_absolute()) {
        throw std::invalid_argument("stdio executable 必须是绝对路径");
    }
    const std::string executable_text = config.executable.generic_u8string();
    if(containsForbiddenTextByte(executable_text)) {
        throw std::invalid_argument("stdio executable 不能包含 NUL、回车或换行");
    }
    const std::string extension = asciiLower(config.executable.extension().u8string());
    if(extension == ".cmd" || extension == ".bat") {
        throw std::invalid_argument("stdio 首版不能直接执行 .cmd 或 .bat，请显式指定真实解释器");
    }
    if(!config.working_directory.empty()) {
        const std::string working_directory_text = config.working_directory.generic_u8string();
        if(containsForbiddenTextByte(working_directory_text)) {
            throw std::invalid_argument("stdio working_directory 不能包含 NUL、回车或换行");
        }
        if(!config.working_directory.is_absolute()) {
            throw std::invalid_argument("stdio working_directory 必须是绝对路径");
        }
    }

    // 参数和环境最终进入平台进程 API；NUL 会造成静默截断，必须在创建任何管道前拒绝。
    for(const auto& argument : config.arguments) {
        if(containsForbiddenTextByte(argument)) {
            throw std::invalid_argument("stdio 参数不能包含 NUL、回车或换行");
        }
    }
    for(const auto& entry : config.environment) {
        if(entry.first.empty() || entry.first.find('=') != std::string::npos ||
           containsForbiddenTextByte(entry.first)) {
            throw std::invalid_argument("stdio 环境变量名称非法");
        }
        if(containsForbiddenTextByte(entry.second)) {
            throw std::invalid_argument("stdio 环境变量值不能包含 NUL、回车或换行");
        }
    }

    requirePositive(config.startup_timeout, "startup_timeout");
    requirePositive(config.shutdown_timeout, "shutdown_timeout");
    requireNonZero(config.max_stderr_tail_bytes, "max_stderr_tail_bytes");
}

void validateHttpConfig(const MCPStreamableHttpConfig& config, const MCPCommonLimits& limits) {
    const URLParts endpoint = parseEndpoint(config.endpoint);
    if(endpoint.scheme == "http") {
        const bool is_literal_loopback = endpoint.host == "::1" || isLiteralIpv4Loopback(endpoint.host);
        if(!config.allow_loopback_http || !is_literal_loopback) {
            throw std::invalid_argument("明文 HTTP 只允许显式开启的字面量 loopback Endpoint");
        }
    }

    requirePositive(config.connect_timeout, "connect_timeout");
    requirePositive(config.stream_idle_timeout, "stream_idle_timeout");
    requirePositive(config.credential_timeout, "credential_timeout");
    requirePositive(config.max_sse_retry_delay, "max_sse_retry_delay");
    if(config.credential_timeout > limits.close_timeout) {
        throw std::invalid_argument("credential_timeout 不能大于 close_timeout");
    }
    requireNonZero(config.max_sse_event_bytes, "max_sse_event_bytes");
    requireNonZero(config.max_event_id_bytes, "max_event_id_bytes");
    requireNonZero(config.max_session_id_bytes, "max_session_id_bytes");
    requireNonZero(config.max_reconnect_attempts, "max_reconnect_attempts");
    if(config.max_sse_event_bytes > limits.max_message_bytes) {
        throw std::invalid_argument("max_sse_event_bytes 不能大于 max_message_bytes");
    }
    validateCredential(config.credential);
}

}  // namespace

bool MCPHttpCredentialRequestContext::isCancellationRequested() const {
    // Provider 可以直接调用该辅助函数，不必为匿名空回调写额外分支。
    return cancellation_requested && cancellation_requested();
}

void validateMCPServerConfig(const MCPServerConfig& config) {
    // server_id 会进入本地命名空间和脱敏错误定位，控制字符会破坏日志与名称边界。
    if(config.server_id.empty() || containsForbiddenTextByte(config.server_id)) {
        throw std::invalid_argument("MCP server_id 不能为空且不能包含控制边界字符");
    }
    validateCommonLimits(config.limits);

    // variant 在类型层保证两种传输互斥；这里只验证当前分支，且整个过程不执行 I/O。
    if(const auto* stdio = std::get_if<MCPStdioServerConfig>(&config.transport)) {
        validateStdioConfig(*stdio);
        return;
    }
    validateHttpConfig(std::get<MCPStreamableHttpConfig>(config.transport), config.limits);
}

}  // namespace aiSDK
