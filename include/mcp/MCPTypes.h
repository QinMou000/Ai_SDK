#pragma once

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace aiSDK {

class MCPToolCatalogAccess;

// MCPClientState 描述一个 Client 实例的完整主生命周期。
// Faulted 和 Closed 都是终态；调用方需要创建新实例才能重新连接。
enum class MCPClientState {
    // 前四项描述从零资源到可执行公开操作的单向建立过程。
    Disconnected,
    Connecting,
    Initializing,
    Ready,
    // Closing 只用于收敛并发关闭；Closed 与 Faulted 均不可重新 connect。
    Closing,
    Closed,
    Faulted
};

// MCPListenerState 独立描述 Streamable HTTP 的后台 GET SSE 状态。
// stdio 使用 NotApplicable，HTTP 405 使用 Unsupported，不与主状态混用。
enum class MCPListenerState {
    // NotApplicable 专用于 stdio，不能解释为 HTTP Listener 成功或失败。
    NotApplicable,
    Starting,
    Listening,
    Unsupported,
    Recovering,
    // Unavailable 可由恢复耗尽产生；Stopped 表示传输已终止监听生命周期。
    Unavailable,
    Stopped
};

// MCPErrorCode 是公开、闭合且不携带秘密的错误分类。
// 调用方应依据枚举分支，不能解析异常文本决定是否重试。
enum class MCPErrorCode {
    // 协商和 JSON-RPC 语义错误位于枚举前段。
    VersionMismatch,
    CapabilityMissing,
    ProtocolViolation,
    RemoteProtocolError,
    TransportFailure,
    RequestTimeout,
    ServerExited,
    HttpStatusError,
    AuthenticationRequired,
    CredentialUnavailable,
    // 生命周期、并发和目录错误保证请求尚未提交或已被明确取消。
    ClientBusy,
    OperationCancelled,
    ToolCatalogStale,
    SessionExpired,
    // OutcomeUnknown 是唯一允许 mayHaveExecuted 为 true 的顶层错误。
    OutcomeUnknown,
    MessageLimitExceeded,
    MessageQueueOverflow,
    PaginationLimitExceeded
};

// mcpErrorCodeName 返回稳定机器名称，只用于诊断和适配层固定映射。
// 返回值是静态字符串，不包含 Server 正文、路径、凭据或会话信息。
const char* mcpErrorCodeName(MCPErrorCode code) noexcept;

// MCPException 承载协议运行期错误及工具副作用不确定性。
// 错误文本使用简体中文；结构化字段才是调用方可靠判断依据。
class MCPException : public std::runtime_error {
   public:
    // 普通错误没有 cause；OutcomeUnknown 必须提供 cause 且 may_have_executed 为 true。
    MCPException(MCPErrorCode code, MCPClientState client_state, std::string message,
                 std::optional<MCPErrorCode> cause = std::nullopt, bool may_have_executed = false);

    // code 返回顶层错误类别。
    MCPErrorCode code() const noexcept;
    // causeCode 仅在 OutcomeUnknown 时保存导致结果未知的原始类别。
    const std::optional<MCPErrorCode>& causeCode() const noexcept;
    // mayHaveExecuted 表示远端工具可能已经产生副作用。
    bool mayHaveExecuted() const noexcept;
    // clientStateAtFailure 保存错误完成线性化点上的状态快照。
    MCPClientState clientStateAtFailure() const noexcept;

   private:
    MCPErrorCode code_;
    std::optional<MCPErrorCode> cause_code_;
    bool may_have_executed_ = false;
    MCPClientState client_state_at_failure_ = MCPClientState::Disconnected;
};

// MCPTool 同时提供常用字段和完整原始对象。
// 未知扩展字段只保存在 raw 中，不被解释为本地执行指令。
struct MCPTool {
    // name 是远端协议名称，实际调用必须原样使用。
    std::string name;
    // title 与 description 是可选展示元数据，缺失时为空。
    std::string title;
    std::string description;
    // input_schema 必须是对象；Client 只做任务所需的顶层校验。
    nlohmann::json input_schema = nlohmann::json::object();
    // output_schema 缺失时为 null，不伪造 Schema。
    nlohmann::json output_schema;
    // annotations、icons 与 execution 按不受信任元数据保存。
    nlohmann::json annotations;
    nlohmann::json icons;
    nlohmann::json execution;
    // raw 无损保存 Server 返回的单个工具对象及未知扩展。
    nlohmann::json raw = nlohmann::json::object();
};

// MCPToolCatalog 是由 MCPClient 签发的目录快照。
// 默认构造对象没有签发令牌，不能绕过 Client 的代次校验。
class MCPToolCatalog {
   public:
    MCPToolCatalog() = default;

    // tools 返回目录值对象；目录本身可安全复制。
    const std::vector<MCPTool>& tools() const noexcept;
    // serverId 标识签发目录的非敏感 Server。
    const std::string& serverId() const noexcept;
    // revision 是签发时的单调目录代次。
    std::uint64_t revision() const noexcept;
    // valid 只表示目录由某个 Client 签发，不表示当前仍未过期。
    bool valid() const noexcept;

   private:
    friend class MCPClient;
    friend class MCPToolAdapter;
    friend class MCPToolCatalogAccess;

    // 私有构造函数阻止调用方伪造有效的签发令牌。
    MCPToolCatalog(std::string server_id, std::uint64_t revision, std::vector<MCPTool> tools,
                   std::shared_ptr<const void> issuer_token);

    std::string server_id_;
    std::uint64_t revision_ = 0;
    std::vector<MCPTool> tools_;
    std::shared_ptr<const void> issuer_token_;
};

// MCPToolCallResult 无损保存 tools/call 的合法结果。
// Adapter 可以生成有损 ToolResult，但不得反向修改这里的协议数据。
struct MCPToolCallResult {
    // content 必须是协议返回的完整内容块数组。
    nlohmann::json content = nlohmann::json::array();
    // structured_content 缺失时为 null；对象值可被首版 Adapter 接受。
    nlohmann::json structured_content;
    // is_error 区分工具业务失败与 JSON-RPC error。
    bool is_error = false;
    // meta 保存可选 _meta，不执行其中的任何内容。
    nlohmann::json meta;
    // raw_result 无损保存完整 result 对象及未知扩展字段。
    nlohmann::json raw_result = nlohmann::json::object();
};

}  // namespace aiSDK
