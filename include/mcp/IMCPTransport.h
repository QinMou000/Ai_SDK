#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "mcp/MCPTypes.h"

namespace aiSDK {

// MCPTransportRequestKind 让传输选择独立的前台、控制或监听执行路径。
// 它不包含工具参数、凭据或其他模型可见正文。
enum class MCPTransportRequestKind {
    // 前五项对应用户公开操作及初始化握手。
    Initialize,
    InitializedNotification,
    Ping,
    ListTools,
    CallTool,
    // 中间三项是 Client 代表协议端发送的后台控制消息。
    CancellationNotification,
    ServerResponse,
    // 最后三项是 HTTP Listener、恢复和会话终止专用路径。
    ListenerGet,
    RecoveryGet,
    DeleteSession
};

// MCPTransportEventType 归一化发送完成、Listener 与终命传输事件。
// JSON-RPC 消息通过独立回调投递，避免事件结构混入协议正文。
enum class MCPTransportEventType {
    // SendCompleted 与 SendFailed 必须携带原 dispatch_id 供 Client 关联。
    SendCompleted,
    SendFailed,
    // POST SSE 响应头已通过校验；Client 可把前台等待从请求段切换到绝对上限。
    ResponseStreamEstablished,
    // Listener 事件只更新旁路状态，不占用前台操作槽。
    ListenerStarted,
    ListenerUnsupported,
    ListenerRecovering,
    ListenerUnavailable,
    ListenerStopped,
    // 其余事件会影响主会话或反映进程、网络终态。
    SessionExpired,
    ServerExited,
    TransportFault
};

// MCPTransportRequestContext 同时携带准备阶段的操作上限与提交后的请求段截止时间。
// dispatch_id 是 Client 生成的内部关联号，不等同于 JSON-RPC request id。
struct MCPTransportRequestContext {
    MCPTransportRequestKind kind = MCPTransportRequestKind::Ping;
    std::uint64_t dispatch_id = 0;
    // deadline 在 prepareMessage 时等于 operation_deadline；commit 后改为请求段截止时间。
    std::chrono::steady_clock::time_point deadline;
    // operation_deadline 不因分页、凭据、SSE 事件或恢复尝试重置。
    std::chrono::steady_clock::time_point operation_deadline = std::chrono::steady_clock::time_point::max();
};

// MCPTransportEvent 只携带闭合错误、HTTP 状态和 Listener 状态。
// error_code 不含动态文本，避免 Server 正文或秘密穿透传输边界。
struct MCPTransportEvent {
    MCPTransportEventType type = MCPTransportEventType::TransportFault;
    std::uint64_t dispatch_id = 0;
    MCPErrorCode error_code = MCPErrorCode::TransportFailure;
    int http_status = 0;
    MCPListenerState listener_state = MCPListenerState::NotApplicable;
};

// MCPTransportCallbacks 由 MCPClient 在 open 时一次性安装。
// 具体传输必须在自身锁外调用，且 commitPrepared 内不得同步回调。
struct MCPTransportCallbacks {
    // on_message 接收单个完整 JSON-RPC 对象，分帧和大小限制由 Transport 完成。
    std::function<void(nlohmann::json)> on_message;
    // on_event 只报告传输事实，不在回调线程解释 Client 生命周期策略。
    std::function<void(MCPTransportEvent)> on_event;
};

// IMCPPreparedMessage 是只允许对应 Transport 解释的一次性准备对象。
// 析构必须释放本次凭据副本和序列化正文，不能留下可重用秘密。
class IMCPPreparedMessage {
   public:
    virtual ~IMCPPreparedMessage() = default;
};

// IMCPTransport 是 MCPClient 与 stdio / Streamable HTTP 的窄边界。
// 接口不解释 Tools 业务语义，也不暴露平台句柄、cpr 或 libcurl 类型。
class IMCPTransport {
   public:
    virtual ~IMCPTransport() = default;

    // open 在 Client 生成的单一初始化截止时间内建立传输资源并安装回调。
    // 具体传输不得从配置重新生成自己的启动预算，以免子阶段串行叠加超时。
    virtual void open(MCPTransportCallbacks callbacks, std::chrono::steady_clock::time_point absolute_deadline) = 0;
    // prepareMessage 在 Client 状态锁外序列化并按需获取 HTTP 凭据。
    virtual std::unique_ptr<IMCPPreparedMessage> prepareMessage(const nlohmann::json& message,
                                                                const MCPTransportRequestContext& context) = 0;
    // commitPrepared 在提交点写入请求段截止时间，只做有界原子入队。
    // 成功返回即建立 Submitted 线性化点；不得调用用户代码或执行阻塞 I/O。
    virtual void commitPrepared(std::unique_ptr<IMCPPreparedMessage> prepared,
                                std::chrono::steady_clock::time_point request_deadline) = 0;
    // completeInitialization 固定后续 HTTP 请求使用的协商协议版本。
    virtual void completeInitialization(const std::string& protocol_version) = 0;
    // startListener 仅对 Streamable HTTP 有效；stdio 实现直接报告 NotApplicable。
    virtual void startListener(std::chrono::steady_clock::time_point deadline) = 0;
    // close 共用 Client 首次进入 Closing 时生成的绝对截止时间。
    // 具体传输不得重新启动 close_timeout；调用返回后不得再调用 Client 回调。
    virtual void close(std::chrono::steady_clock::time_point absolute_deadline) noexcept = 0;
};

}  // namespace aiSDK
