#pragma once

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "mcp/MCPTypes.h"

namespace aiSDK::detail {

// 本头仅暴露 MCPClient 与协议实现之间的内部纯值合同。
// 所有输入 JSON 都按不可信消息处理，调用方不得跳过对应解析器。
// 构造入口不执行 I/O，Transport 负责序列化、framing 与投递。
// 解析入口不保留输入引用，返回值可脱离传输缓冲独立存活。
// expected_id 只用于 Client 主动请求的响应关联，不代表内部 dispatch_id。
// remaining_tool_limit 必须由持有分页上下文的 Client 逐页重新计算。
// Client 状态参数只用于异常快照，纯解析过程不会主动改变状态机。
// Server 请求和通知先经分类，再由 Client 决定支持的方法与响应。
// 该接口位于 src 私有 include 路径，不属于 ai_sdk_mcp 的安装公共面。

// 首版只协商一个明确协议版本，避免不同版本能力语义被静默混用。
inline constexpr const char* kMCPProtocolVersion = "2025-11-25";

// MCPInitializeResult 保存 Client 状态机真正需要的协商结果。
// 原始初始化正文不进入公开对象，避免 Server 自报信息被误当作可信配置。
struct MCPInitializeResult {
    std::string protocol_version;
    bool tools_list_changed = false;
};

// MCPToolsPage 是一次 tools/list 的已校验临时页。
// 单页数量在复制工具前受剩余额度保护；跨页重名和重复游标由 Client 处理。
struct MCPToolsPage {
    std::vector<MCPTool> tools;
    std::optional<std::string> next_cursor;
};

// MCPIncomingMessageKind 区分响应、Server 请求和通知，供单一分发队列使用。
enum class MCPIncomingMessageKind { Response, Request, Notification };

// MCPIncomingMessage 保留关联所需的 ID、方法与参数，不解释未声明能力。
struct MCPIncomingMessage {
    MCPIncomingMessageKind kind = MCPIncomingMessageKind::Notification;
    nlohmann::json id;
    std::string method;
    nlohmann::json params;
    nlohmann::json raw;
};

// 以下构造函数只生成一条紧凑 JSON-RPC 值，不执行序列化或 I/O。
nlohmann::json makeInitializeRequest(std::uint64_t request_id);
nlohmann::json makeInitializedNotification();
nlohmann::json makePingRequest(std::uint64_t request_id);
nlohmann::json makeToolsListRequest(std::uint64_t request_id, const std::optional<std::string>& cursor);
nlohmann::json makeToolsCallRequest(std::uint64_t request_id, const std::string& name, const nlohmann::json& arguments);
nlohmann::json makeCancelledNotification(const nlohmann::json& request_id);
nlohmann::json makeServerSuccessResponse(const nlohmann::json& request_id);
nlohmann::json makeServerMethodNotFoundResponse(const nlohmann::json& request_id);

// 解析函数严格校验 JSON-RPC 信封和当前响应 ID，失败抛闭合 MCPException。
MCPInitializeResult parseInitializeResponse(const nlohmann::json& message, std::uint64_t expected_id,
                                            MCPClientState client_state);
void parseEmptyResponse(const nlohmann::json& message, std::uint64_t expected_id, MCPClientState client_state);
MCPToolsPage parseToolsListResponse(const nlohmann::json& message, std::uint64_t expected_id,
                                    MCPClientState client_state, std::size_t remaining_tool_limit);
MCPToolCallResult parseToolsCallResponse(const nlohmann::json& message, std::uint64_t expected_id,
                                         MCPClientState client_state);

// classifyIncomingMessage 只做单条消息结构分类；响应的具体 result 由上面的操作解析器处理。
MCPIncomingMessage classifyIncomingMessage(const nlohmann::json& message, MCPClientState client_state);

}  // namespace aiSDK::detail
