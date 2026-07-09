#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tool/ToolCall.h"

namespace aiSDK {

enum class Role { System, User, Assistant, Tool };

// Message 是 SDK 内部统一消息结构。
// 后续 Provider 只负责把它映射到各自 API 需要的 JSON 格式。
struct Message {
    // role 决定消息在模型对话中的语义位置。
    Role role = Role::User;
    // content 保存当前消息的文本主体。
    std::string content;
    // name 主要用于 system/assistant/tool 的附加身份标识。
    std::optional<std::string> name;
    // tool_call_id 用于把工具结果消息绑定回对应的工具调用。
    std::optional<std::string> tool_call_id;
    // tool_calls 只在 assistant 发起工具调用时有值。
    std::vector<ToolCall> tool_calls;
};

// roleToString 和 roleFromString 统一角色的字符串协议，
// 这样不同 Provider 和测试都不需要各自维护一套映射。
std::string roleToString(Role role);
Role roleFromString(const std::string& role);

// messageToJson / messageFromJson 是 Message 的稳定序列化边界。
// 后续 Provider 适配层会直接基于这里的 JSON 结果做协议转换。
nlohmann::json messageToJson(const Message& message);
Message messageFromJson(const nlohmann::json& json);

// 这些工厂函数让测试和业务层可以显式表达消息意图，
// 同时避免在各处手写 Role 和可选字段的初始化细节。
inline Message SystemMessage(std::string content) {
    return Message{Role::System, std::move(content), std::nullopt, std::nullopt, {}};
}

inline Message UserMessage(std::string content) {
    return Message{Role::User, std::move(content), std::nullopt, std::nullopt, {}};
}

inline Message AssistantMessage(std::string content) {
    return Message{Role::Assistant, std::move(content), std::nullopt, std::nullopt, {}};
}

inline Message ToolMessage(std::string content, std::string tool_call_id) {
    Message message{Role::Tool, std::move(content), std::nullopt, std::move(tool_call_id), {}};
    return message;
}

}  // namespace aiSDK
