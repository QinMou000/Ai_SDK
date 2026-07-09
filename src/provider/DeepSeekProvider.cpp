#include "provider/DeepSeekProvider.h"

#include <sstream>
#include <stdexcept>
#include <utility>

#include "core/ChatResponse.h"
#include "core/Message.h"
#include "tool/Tool.h"

namespace aiSDK {
namespace {

// jsonStringOrEmpty 在字段缺失、为 null 或类型不匹配时统一返回空字符串。
// 这样 Provider 解析层可以兼容 tool_call 和普通回答中的可空文本字段。
std::string jsonStringOrEmpty(const nlohmann::json& json, const char* key) {
    if(!json.contains(key) || json.at(key).is_null()) {
        return "";
    }

    if(!json.at(key).is_string()) {
        return "";
    }

    return json.at(key).get<std::string>();
}

// kDeepSeekProviderName 统一维护运行时 Provider 标识。
constexpr const char* kDeepSeekProviderName = "deepseek";
// kDeepSeekDefaultBaseUrl 对齐 DeepSeek 官方 OpenAI-compatible 基础地址。
constexpr const char* kDeepSeekDefaultBaseUrl = "https://api.deepseek.com";
// kDeepSeekDefaultModel 默认使用当前官方长期模型名，避免继续依赖即将废弃的别名。
constexpr const char* kDeepSeekDefaultModel = "deepseek-v4-flash";

// trimTrailingSlash 负责去掉末尾斜杠，避免拼接路径时出现双斜杠。
std::string trimTrailingSlash(std::string text) {
    while(!text.empty() && text.back() == '/') {
        text.pop_back();
    }
    return text;
}

// resolveBaseUrl 在未显式配置时回退到官方默认地址。
std::string resolveBaseUrl(const ProviderConfig& config) {
    if(config.base_url.empty()) {
        return kDeepSeekDefaultBaseUrl;
    }
    return trimTrailingSlash(config.base_url);
}

// resolveModelName 在请求模型、配置默认模型和库默认模型之间做兜底选择。
std::string resolveModelName(const ProviderConfig& config, const ChatRequest& request) {
    if(!request.model.empty()) {
        return request.model;
    }
    if(!config.default_model.empty()) {
        return config.default_model;
    }
    return kDeepSeekDefaultModel;
}

// requireApiKey 保证真实调用前已经注入密钥。
std::string requireApiKey(const ProviderConfig& config) {
    if(config.api_key.empty()) {
        throw std::invalid_argument("DeepSeekProvider 缺少 api_key，请通过配置或环境变量提供");
    }
    return config.api_key;
}

// buildChatUrl 统一构造 Chat Completions 终端地址。
std::string buildChatUrl(const ProviderConfig& config) {
    return resolveBaseUrl(config) + "/chat/completions";
}

// toolCallToProviderJson 把内部 ToolCall 映射为 OpenAI-compatible assistant tool_calls 结构。
nlohmann::json toolCallToProviderJson(const ToolCall& call) {
    const std::string arguments = call.raw_arguments.empty() ? call.arguments.dump() : call.raw_arguments;

    return nlohmann::json{
        {"id",       call.id   },
        {"type",     "function"},
        {"function",
         {
             {"name", call.name},
             {"arguments", arguments},
         }                     },
    };
}

// messageToProviderJson 把统一消息结构转换为 DeepSeek 请求消息。
nlohmann::json messageToProviderJson(const Message& message) {
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
            json["tool_calls"].push_back(toolCallToProviderJson(call));
        }
    }

    return json;
}

// toolToProviderJson 把统一工具定义转换为 OpenAI-compatible tools 描述。
nlohmann::json toolToProviderJson(const Tool& tool) {
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

// buildRequestJson 统一生成 DeepSeek 请求体，避免 chat / stream 两处手工维护字段。
nlohmann::json buildRequestJson(const ProviderConfig& config, const ChatRequest& request, bool stream) {
    nlohmann::json json{
        {"model", resolveModelName(config, request)},
        {"messages", nlohmann::json::array()},
        {"stream", stream},
        {"temperature", request.temperature},
        {"max_tokens", request.max_tokens},
    };

    for(const auto& message : request.messages) {
        json["messages"].push_back(messageToProviderJson(message));
    }

    if(!request.tools.empty()) {
        json["tools"] = nlohmann::json::array();
        for(const auto& tool : request.tools) {
            json["tools"].push_back(toolToProviderJson(tool));
        }
    }

    if(stream) {
        // include_usage 让最终流式阶段返回完整 token 统计，便于后续补 usage 能力。
        json["stream_options"] = nlohmann::json{
            {"include_usage", true}
        };
    }

    return json;
}

// buildHeaders 统一注入 Content-Type 与 Bearer 认证头。
HttpHeaders buildHeaders(const ProviderConfig& config) {
    return HttpHeaders{
        {"Content-Type",  "application/json"               },
        {"Authorization", "Bearer " + requireApiKey(config)},
    };
}

// parseToolCall 把 DeepSeek/OpenAI-compatible tool call 结构转换回内部对象。
ToolCall parseToolCall(const nlohmann::json& json) {
    ToolCall call;
    call.id = jsonStringOrEmpty(json, "id");

    if(json.contains("function")) {
        const nlohmann::json& function = json.at("function");
        call.name = jsonStringOrEmpty(function, "name");
        call.raw_arguments = jsonStringOrEmpty(function, "arguments");

        if(!call.raw_arguments.empty()) {
            const nlohmann::json arguments = nlohmann::json::parse(call.raw_arguments, nullptr, false);
            call.arguments = arguments.is_discarded() ? nlohmann::json::object() : arguments;
        }
    }

    return call;
}

// parseUsage 提取统一 token 统计结构；缺失时返回默认值。
Usage parseUsage(const nlohmann::json& json) {
    Usage usage;
    if(!json.is_object()) {
        return usage;
    }

    usage.prompt_tokens = json.value("prompt_tokens", 0);
    usage.completion_tokens = json.value("completion_tokens", 0);
    usage.total_tokens = json.value("total_tokens", 0);
    return usage;
}

// extractHttpError 尝试优先提取服务端结构化错误信息。
std::string extractHttpError(const HttpResponse& response) {
    const nlohmann::json json = nlohmann::json::parse(response.body, nullptr, false);
    if(!json.is_discarded() && json.contains("error")) {
        const nlohmann::json& error = json.at("error");
        if(error.is_object() && error.contains("message")) {
            return error.at("message").get<std::string>();
        }
        return error.dump();
    }

    if(!response.body.empty()) {
        return response.body;
    }

    return "空响应体";
}

// ensureSuccessStatus 把非 2xx 响应转换成明确异常，便于调用方快速定位问题。
void ensureSuccessStatus(const HttpResponse& response, const std::string& url) {
    if(response.status_code >= 200 && response.status_code < 300) {
        return;
    }

    std::ostringstream message;
    message << "DeepSeek API 请求失败: " << url << " | HTTP " << response.status_code << " | " << extractHttpError(response);
    throw std::runtime_error(message.str());
}

// parseChatResponse 解析普通 Chat Completion 响应体。
ChatResponse parseChatResponse(const std::string& raw_response) {
    const nlohmann::json json = nlohmann::json::parse(raw_response);
    const nlohmann::json& choice = json.at("choices").at(0);
    const nlohmann::json& message_json = choice.at("message");

    ChatResponse response;
    response.raw_response = raw_response;
    response.content = jsonStringOrEmpty(message_json, "content");
    response.message = AssistantMessage(response.content);
    response.usage = parseUsage(json.value("usage", nlohmann::json::object()));

    if(message_json.contains("tool_calls")) {
        for(const auto& tool_call_json : message_json.at("tool_calls")) {
            response.tool_calls.push_back(parseToolCall(tool_call_json));
        }
        response.message.tool_calls = response.tool_calls;
    }

    return response;
}

// processBufferedEvents 从累积缓冲区中提取完整 SSE 事件并回调给上层。
void processBufferedEvents(std::string& buffer, const SSEParser& parser, const StreamCallback& callback) {
    while(true) {
        size_t boundary = buffer.find("\r\n\r\n");
        size_t delimiter_size = 4;

        const size_t lf_boundary = buffer.find("\n\n");
        if(boundary == std::string::npos || (lf_boundary != std::string::npos && lf_boundary < boundary)) {
            boundary = lf_boundary;
            delimiter_size = 2;
        }

        if(boundary == std::string::npos) {
            return;
        }

        const std::string event_block = buffer.substr(0, boundary);
        buffer.erase(0, boundary + delimiter_size);

        for(const auto& event : parser.parseChunk(event_block)) {
            callback(event);
        }
    }
}

}  // namespace

DeepSeekProvider::DeepSeekProvider(ProviderConfig config, int timeout_ms) : config_(std::move(config)), timeout_ms_(timeout_ms) {}

ChatResponse DeepSeekProvider::chat(const ChatRequest& request) {
    const std::string url = buildChatUrl(config_);
    const nlohmann::json request_json = buildRequestJson(config_, request, false);

    const HttpResponse response = http_client_.postJson(url, request_json, buildHeaders(config_), timeout_ms_);
    ensureSuccessStatus(response, url);
    return parseChatResponse(response.body);
}

void DeepSeekProvider::streamChat(const ChatRequest& request, StreamCallback callback) {
    if(!callback) {
        return;
    }

    const std::string url = buildChatUrl(config_);
    const nlohmann::json request_json = buildRequestJson(config_, request, true);
    std::string pending_buffer;

    const HttpResponse response = http_client_.postJsonStream(url, request_json, buildHeaders(config_), timeout_ms_, [&](std::string_view chunk) {
        pending_buffer.append(chunk);
        processBufferedEvents(pending_buffer, sse_parser_, callback);
    });

    ensureSuccessStatus(response, url);

    // 网络结束后再冲刷一次残留缓冲，避免最后一个事件块刚好没有被及时取出。
    if(!pending_buffer.empty()) {
        for(const auto& event : sse_parser_.parseChunk(pending_buffer)) {
            callback(event);
        }
    }
}

ProviderInfo DeepSeekProvider::info() const {
    ProviderInfo provider_info;
    provider_info.name = kDeepSeekProviderName;
    provider_info.default_model = config_.default_model.empty() ? kDeepSeekDefaultModel : config_.default_model;
    provider_info.supports_stream = true;
    provider_info.supports_tool_call = true;
    return provider_info;
}

}  // namespace aiSDK
