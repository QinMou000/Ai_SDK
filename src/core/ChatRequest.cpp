#include "core/ChatRequest.h"

namespace aiSDK {
namespace {

// toolToJson 当前生成 OpenAI-compatible 的 tools 结构，
// 这样后续多 Provider 复用时不需要每家再拼一次基础字段。
nlohmann::json toolToJson(const Tool& tool) {
    return nlohmann::json{
        {"type",     "function"},
        {"function",
         {
             {"name", tool.name},
             {"description", tool.description},
             {"parameters", tool.parameters},
         }                     },
    };
}

}  // namespace

nlohmann::json chatRequestToJson(const ChatRequest& request) {
    nlohmann::json json{
        {"model",       request.model          },
        {"messages",    nlohmann::json::array()},
        {"stream",      request.stream         },
        {"temperature", request.temperature    },
        {"max_tokens",  request.max_tokens     },
    };

    // messages 的顺序必须保留，不能在序列化阶段做任何重排。
    for(const auto& message : request.messages) {
        json["messages"].push_back(messageToJson(message));
    }

    if(!request.tools.empty()) {
        // tools 为空时不输出该字段，避免给不需要工具的请求附带噪音。
        json["tools"] = nlohmann::json::array();
        for(const auto& tool : request.tools) {
            json["tools"].push_back(toolToJson(tool));
        }
    }

    return json;
}

}  // namespace aiSDK
