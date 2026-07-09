#include "core/ChatResponse.h"

#include <utility>

namespace aiSDK {
namespace {

// ToolCall 在响应导出时保持完整结构，方便后续排查解析链路。
nlohmann::json toolCallToJson(const ToolCall& call) {
    return nlohmann::json{
        {"id",            call.id           },
        {"name",          call.name         },
        {"arguments",     call.arguments    },
        {"raw_arguments", call.raw_arguments},
    };
}

}  // namespace

nlohmann::json usageToJson(const Usage& usage) {
    return nlohmann::json{
        {"prompt_tokens",     usage.prompt_tokens    },
        {"completion_tokens", usage.completion_tokens},
        {"total_tokens",      usage.total_tokens     },
    };
}

nlohmann::json chatResponseToJson(const ChatResponse& response) {
    nlohmann::json json{
        {"message",      messageToJson(response.message)},
        {"content",      response.content               },
        {"usage",        usageToJson(response.usage)    },
        {"raw_response", response.raw_response          },
    };

    // tool_calls 即使为空也输出数组，方便测试和日志消费端保持字段稳定。
    json["tool_calls"] = nlohmann::json::array();
    for(const auto& call : response.tool_calls) {
        json["tool_calls"].push_back(toolCallToJson(call));
    }

    return json;
}

ChatResponse assistantTextResponse(std::string content) {
    ChatResponse response;
    // 统一让 message.content 和顶层 content 保持一致，
    // 避免调用方在两个字段之间再做同步。
    response.message = AssistantMessage(content);
    response.content = std::move(content);
    return response;
}

}  // namespace aiSDK
