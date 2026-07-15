#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace aiSDK {

// MCPHttpRequestKind 只向凭据 Provider 暴露脱敏的请求用途。
// Provider 不能通过该值改变 Endpoint、Header 名称或协议正文。
enum class MCPHttpRequestKind {
    ForegroundPost,
    ListenerGet,
    RecoveryGet,
    ServerResponsePost,
    CancellationPost,
    DeleteSession
};

// MCPHttpCredentialRequestContext 给 Provider 提供截止时间和协作取消视图。
// 该对象只在 credentialValue 同步调用期间有效，跨调用保存没有意义。
struct MCPHttpCredentialRequestContext {
    MCPHttpRequestKind kind = MCPHttpRequestKind::ForegroundPost;
    std::chrono::steady_clock::time_point deadline;
    // cancellation_requested 返回 true 时，Provider 应尽快终止外部交互。
    std::function<bool()> cancellation_requested;

    // isCancellationRequested 对空回调返回 false，便于简单 Provider 使用。
    bool isCancellationRequested() const;
};

// IMCPHttpCredentialProvider 由应用实现并负责 Token 的获取、刷新与保存。
// SDK 每次请求只获取当前秘密值，不把 Provider 异常文本带入公开错误。
class IMCPHttpCredentialProvider {
   public:
    virtual ~IMCPHttpCredentialProvider() = default;

    // credentialValue 必须在上下文截止时间内协作返回。
    virtual std::string credentialValue(const MCPHttpCredentialRequestContext& context) = 0;
};

// MCPAnonymousCredential 表示请求不附加应用凭据。
struct MCPAnonymousCredential {};

// MCPBearerCredential 固定写入 Authorization: Bearer <value>。
struct MCPBearerCredential {
    std::shared_ptr<IMCPHttpCredentialProvider> provider;
};

// MCPFixedHeaderCredential 的 Header 名称由受信应用构造期固定。
// Provider 只能返回值，不能覆盖 MCP 保留头或动态选择名称。
struct MCPFixedHeaderCredential {
    std::string header_name;
    std::shared_ptr<IMCPHttpCredentialProvider> provider;
};

// MCPHttpCredential 是互斥的闭合鉴权模式，默认匿名。
using MCPHttpCredential = std::variant<MCPAnonymousCredential, MCPBearerCredential, MCPFixedHeaderCredential>;

// MCPCommonLimits 为两个传输共享资源和绝对操作上限。
struct MCPCommonLimits {
    // request_timeout 是单次活动等待上限，absolute_request_timeout 限制整个公开操作。
    std::chrono::milliseconds request_timeout{30000};
    std::chrono::milliseconds absolute_request_timeout{120000};
    std::chrono::milliseconds close_timeout{5000};
    // 字节和队列上限在解析或入队之前检查，阻止不受信任 Server 放大内存。
    std::size_t max_message_bytes = 8U * 1024U * 1024U;
    std::size_t max_pending_messages = 64U;
    std::size_t max_error_text_bytes = 4096U;
    // 分页上限同时约束请求次数与最终工具数量，任一超限都终止列举。
    std::size_t max_pages = 100U;
    std::size_t max_tools = 4096U;
};

// MCPStdioServerConfig 只接受真实可执行文件及参数数组。
// 所有字段在启动前校验，传输不得经过 shell 或命令字符串拼接。
struct MCPStdioServerConfig {
    // executable 必须是绝对真实文件；arguments 中每项保持独立字节序列。
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    // environment 是显式子进程环境；是否合并父环境由下一字段单独决定。
    std::unordered_map<std::string, std::string> environment;
    bool inherit_parent_environment = false;
    std::chrono::milliseconds startup_timeout{10000};
    // shutdown_timeout 到期后 Transport 必须终止整个进程树，不能无限等待 EOF。
    std::chrono::milliseconds shutdown_timeout{2000};
    std::size_t max_stderr_tail_bytes = 64U * 1024U;
    // stderr_callback 在内部消费线程按原始字节分块调用，必须非阻塞并自行脱敏。
    // string_view 仅在本次回调期间有效；回调异常会被隔离，不改变协议结果。
    std::function<void(std::string_view)> stderr_callback;
};

// MCPStreamableHttpConfig 保存构造后不可变的远程 Endpoint 和网络上限。
// 生产只允许 HTTPS；显式开发开关也只允许字面量 loopback HTTP。
struct MCPStreamableHttpConfig {
    // endpoint 在构造期完成 Scheme、主机、片段和用户信息校验，后续不可重定向。
    std::string endpoint;
    bool allow_loopback_http = false;
    MCPHttpCredential credential = MCPAnonymousCredential{};
    // connect_timeout 只约束建连，公开操作仍受公共绝对截止时间约束。
    std::chrono::milliseconds connect_timeout{10000};
    std::chrono::milliseconds stream_idle_timeout{30000};
    std::chrono::milliseconds credential_timeout{2000};
    // SSE、Event ID 与 Session ID 分别设置上限，不能互相借用更宽松的消息上限。
    std::size_t max_sse_event_bytes = 8U * 1024U * 1024U;
    std::size_t max_event_id_bytes = 4096U;
    std::size_t max_session_id_bytes = 1024U;
    std::size_t max_reconnect_attempts = 3U;
    // Server retry 提示仍受本地上限裁剪，避免远端令关闭或恢复无限挂起。
    std::chrono::milliseconds max_sse_retry_delay{30000};
};

// MCPServerConfig 绑定一个 Server 标识、公共限制和且仅一个传输变体。
// MCPClient 构造时完成静态校验，但绝不在构造函数中执行 I/O。
struct MCPServerConfig {
    std::string server_id;
    MCPCommonLimits limits;
    std::variant<MCPStdioServerConfig, MCPStreamableHttpConfig> transport;
};

// validateMCPServerConfig 只做确定性静态校验，失败抛 std::invalid_argument。
// 该函数不检查远端可达性，也不启动进程或发出网络请求。
void validateMCPServerConfig(const MCPServerConfig& config);

}  // namespace aiSDK
