#include "core/Message.h"

#include <stdexcept>

namespace aiSDK {
namespace {

// ToolCall 的 JSON 转换先收敛在 Message 模块内部，
// 因为当前消息协议是 ToolCall 的主要承载者。
nlohmann::json toolCallToJson(const ToolCall& call) {
    return nlohmann::json{
        {"id",            call.id           },
        {"name",          call.name         },
        {"arguments",     call.arguments    },
        {"raw_arguments", call.raw_arguments},
    };
}

ToolCall toolCallFromJson(const nlohmann::json& json) {
    ToolCall call;
    call.id = json.value("id", "");
    call.name = json.value("name", "");
    call.raw_arguments = json.value("raw_arguments", "");
    if(json.contains("arguments")) {
        call.arguments = json.at("arguments");
    }
    return call;
}

}  // namespace

std::string roleToString(Role role) {
    // 这里返回的字符串是后续请求协议和测试断言的标准值。
    switch(role) {
        case Role::System:
            return "system";
        case Role::User:
            return "user";
        case Role::Assistant:
            return "assistant";
        case Role::Tool:
            return "tool";
    }
    throw std::invalid_argument("unsupported role");
}

Role roleFromString(const std::string& role) {
    if(role == "system") {
        return Role::System;
    }
    if(role == "user") {
        return Role::User;
    }
    if(role == "assistant") {
        return Role::Assistant;
    }
    if(role == "tool") {
        return Role::Tool;
    }
    throw std::invalid_argument("unsupported role string");
}

nlohmann::json messageToJson(const Message& message) {
    nlohmann::json json{
        {"role",    roleToString(message.role)},
        {"content", message.content           },
    };

    if(message.name.has_value()) {
        json["name"] = *message.name;
    }
    if(message.tool_call_id.has_value()) {
        json["tool_call_id"] = *message.tool_call_id;
    }
    if(!message.tool_calls.empty()) {
        json["tool_calls"] = nlohmann::json::array();
        for(const auto& call : message.tool_calls) {
            json["tool_calls"].push_back(toolCallToJson(call));
        }
    }

    return json;
}

Message messageFromJson(const nlohmann::json& json) {
    Message message;
    message.role = roleFromString(json.at("role").get<std::string>());
    message.content = json.value("content", "");
    if(json.contains("name")) {
        message.name = json.at("name").get<std::string>();
    }
    if(json.contains("tool_call_id")) {
        message.tool_call_id = json.at("tool_call_id").get<std::string>();
    }
    if(json.contains("tool_calls")) {
        // assistant 可能携带多个工具调用，这里按顺序完整恢复。
        for(const auto& call_json : json.at("tool_calls")) {
            message.tool_calls.push_back(toolCallFromJson(call_json));
        }
    }
    return message;
}

}  // namespace aiSDK
