#include "mcp/MCPTypes.h"

#include <utility>

namespace aiSDK {

// 本文件实现公开 MCP 值类型的稳定名称、结构化异常和只读目录访问。
// 它不解释网络正文，也不持有 Client、Transport 或线程资源。
// 所有访问器均为常量时间；目录工具数组的存储成本只在签发时发生一次。
// 公开文本由调用方在边界处净化，本层只维护错误字段之间的不变量。

const char* mcpErrorCodeName(MCPErrorCode code) noexcept {
    // 错误名称属于公开机器合同，必须逐项列出，避免新增枚举时意外改变旧名称。
    // default 仅防御来自 ABI 边界的非法枚举值，不承担协议分类职责。
    switch(code) {
        // 协议版本不相交，调用方必须更换兼容 Server 或 Client。
        case MCPErrorCode::VersionMismatch:
            return "VersionMismatch";
        // 初始化结果缺少首版所需 Tools 能力。
        case MCPErrorCode::CapabilityMissing:
            return "CapabilityMissing";
        // 对端消息违反 JSON-RPC 或 MCP 结构合同。
        case MCPErrorCode::ProtocolViolation:
            return "ProtocolViolation";
        // 合法 JSON-RPC error 被归一化为远端协议错误。
        case MCPErrorCode::RemoteProtocolError:
            return "RemoteProtocolError";
        // 进程、Socket 或 TLS 等底层通路失败。
        case MCPErrorCode::TransportFailure:
            return "TransportFailure";
        // 本地截止时间先于协议终局到达。
        case MCPErrorCode::RequestTimeout:
            return "RequestTimeout";
        // stdio Server 在会话仍需使用时退出。
        case MCPErrorCode::ServerExited:
            return "ServerExited";
        // HTTP 返回非协议允许状态且没有更窄分类。
        case MCPErrorCode::HttpStatusError:
            return "HttpStatusError";
        // 对端明确要求或拒绝当前认证。
        case MCPErrorCode::AuthenticationRequired:
            return "AuthenticationRequired";
        // 凭据 Provider 未能在截止时间内交付秘密值。
        case MCPErrorCode::CredentialUnavailable:
            return "CredentialUnavailable";
        // 同一 Client 已有另一个用户公开操作在途。
        case MCPErrorCode::ClientBusy:
            return "ClientBusy";
        // close 或上层取消在提交前终止操作。
        case MCPErrorCode::OperationCancelled:
            return "OperationCancelled";
        // Catalog 身份、Server 或 revision 已不再匹配。
        case MCPErrorCode::ToolCatalogStale:
            return "ToolCatalogStale";
        // 已建立的 HTTP Session 被 Server 以 404 宣告失效。
        case MCPErrorCode::SessionExpired:
            return "SessionExpired";
        // 工具请求已提交，但无法证明远端是否执行或完成。
        case MCPErrorCode::OutcomeUnknown:
            return "OutcomeUnknown";
        // 单条 JSON 或 SSE 消息超过构造期资源上限。
        case MCPErrorCode::MessageLimitExceeded:
            return "MessageLimitExceeded";
        // Transport 的有界待处理队列没有容量。
        case MCPErrorCode::MessageQueueOverflow:
            return "MessageQueueOverflow";
        // 工具分页次数或累计条目超过本地上限。
        case MCPErrorCode::PaginationLimitExceeded:
            return "PaginationLimitExceeded";
    }
    return "UnknownMCPError";
}

MCPException::MCPException(MCPErrorCode code, MCPClientState client_state, std::string message,
                           std::optional<MCPErrorCode> cause, bool may_have_executed)
    // runtime_error 只保存净化后的用户可见文本，控制流依据下列结构化字段。
    : std::runtime_error(std::move(message)),
      code_(code),
      cause_code_(cause),
      may_have_executed_(may_have_executed),
      client_state_at_failure_(client_state) {
    // OutcomeUnknown 是防止上层误重试有副作用工具的安全边界，必须同时保留根因和执行可能性。
    // 其他错误不得伪装成结果未知，否则调用方无法可靠区分未提交和已提交请求。
    if(code_ == MCPErrorCode::OutcomeUnknown) {
        // 缺少任一字段都会让上层无法执行“禁止自动重试”的安全策略。
        if(!cause_code_.has_value() || !may_have_executed_) {
            throw std::invalid_argument("OutcomeUnknown 必须包含根因并标记工具可能已执行");
        }
        if(*cause_code_ == MCPErrorCode::OutcomeUnknown) {
            // 根因必须是具体传输、超时或会话类别，禁止形成递归包装。
            throw std::invalid_argument("OutcomeUnknown 的根因不能再次为 OutcomeUnknown");
        }
    } else if(cause_code_.has_value() || may_have_executed_) {
        // 未知结果语义不能附着到普通错误，否则会模糊提交前后的边界。
        throw std::invalid_argument("只有 OutcomeUnknown 可以包含根因或执行可能性标记");
    }
}

MCPErrorCode MCPException::code() const noexcept {
    // 枚举按值返回，读取不会改变异常对象或其状态快照。
    return code_;
}

const std::optional<MCPErrorCode>& MCPException::causeCode() const noexcept {
    // 引用生命周期与异常对象一致，调用方不拥有内部 optional。
    return cause_code_;
}

bool MCPException::mayHaveExecuted() const noexcept {
    // 该标志只有 OutcomeUnknown 构造成功时可能为 true。
    return may_have_executed_;
}

MCPClientState MCPException::clientStateAtFailure() const noexcept {
    // 返回抛出点快照，不实时查询 Client，也不持有 Client 生命周期。
    return client_state_at_failure_;
}

MCPToolCatalog::MCPToolCatalog(std::string server_id, std::uint64_t revision, std::vector<MCPTool> tools,
                               std::shared_ptr<const void> issuer_token)
    // 所有可变输入一次性移入快照；之后只提供 const 读取接口。
    : server_id_(std::move(server_id)),
      revision_(revision),
      tools_(std::move(tools)),
      issuer_token_(std::move(issuer_token)) {}

const std::vector<MCPTool>& MCPToolCatalog::tools() const noexcept {
    // 引用避免复制大型 Schema；有效期与 Catalog 对象一致。
    return tools_;
}

const std::string& MCPToolCatalog::serverId() const noexcept {
    // Server ID 是非敏感逻辑标识，可以用于上层审批和审计映射。
    return server_id_;
}

std::uint64_t MCPToolCatalog::revision() const noexcept {
    // revision 仅表示签发代次，不保证调用瞬间仍是当前代次。
    return revision_;
}

bool MCPToolCatalog::valid() const noexcept {
    // 令牌只证明对象来自某个 Client；是否仍属当前代次由 Client 在调用前复核。
    return issuer_token_ != nullptr;
}

}  // namespace aiSDK
