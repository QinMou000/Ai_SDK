#include "http/SSEParser.h"

#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace aiSDK {
namespace {

// jsonStringOrEmpty 在字段缺失或显式为 null 时统一返回空字符串。
// 这样流式解析可以安全跳过暂时没有文本增量的 chunk。
std::string jsonStringOrEmpty(const nlohmann::json& json, const char* key) {
    if(!json.contains(key) || json.at(key).is_null()) {
        return "";
    }

    if(!json.at(key).is_string()) {
        return "";
    }

    return json.at(key).get<std::string>();
}

// LF：\n Unix/Linux/macOS 标准换行
// CRLF：\r\n Windows 系统换行（回车 + 换行两个字符）
// CR：\r 老式 Mac 换行，现在很少见

// normalizeNewlines 把 CRLF 统一折叠为 LF，降低后续事件边界处理复杂度。
// 把所有 \r\n、单独\r 全部替换成单纯的 \n
std::string normalizeNewlines(std::string text) {
    std::string normalized;
    normalized.reserve(text.size());

    for(size_t index = 0; index < text.size(); ++index) {
        if(text[index] == '\r') {
            if(index + 1U < text.size() && text[index + 1U] == '\n') {
                continue;
            }
            normalized.push_back('\n');
            continue;
        }

        normalized.push_back(text[index]);
    }

    return normalized;
}

// trimDataPrefix 去掉 SSE data: 后面可选的一个空格，保留真实负载内容。
std::string trimDataPrefix(std::string line) {
    constexpr const char* kPrefix = "data:";
    if(line.rfind(kPrefix, 0) != 0) {
        return "";
    }

    std::string payload = line.substr(5);
    if(!payload.empty() && payload.front() == ' ') {
        payload.erase(payload.begin());
    }
    return payload;
}

// errorEvent 统一构造流式错误事件，减少分支中的重复样板。
StreamEvent errorEvent(std::string message) {
    StreamEvent event;
    event.type = StreamEventType::Error;
    event.error_message = std::move(message);
    return event;
}

// parseEventBlock 解析单个完整 SSE 事件块。
std::vector<StreamEvent> parseEventBlock(const std::string& block) {
    std::istringstream input(block);
    std::string line;
    std::string payload;

    while(std::getline(input, line)) {
        if(line.empty()) {
            continue;
        }

        const std::string data_line = trimDataPrefix(line);
        if(data_line.empty()) {
            continue;
        }

        if(!payload.empty()) {
            payload.push_back('\n');
        }
        payload.append(data_line);
    }

    if(payload.empty()) {
        return {};
    }

    if(payload == "[DONE]") {
        return {
            StreamEvent{StreamEventType::Done, "", ""}
        };
    }

    const nlohmann::json json = nlohmann::json::parse(payload, nullptr, false);
    if(json.is_discarded()) {
        return {errorEvent("无法解析 SSE 事件 JSON")};
    }

    if(json.contains("error")) {
        const nlohmann::json& error = json.at("error");
        if(error.is_object() && error.contains("message")) {
            return {errorEvent(error.at("message").get<std::string>())};
        }
        return {errorEvent(error.dump())};
    }

    if(!json.contains("choices")) {
        return {};
    }

    std::vector<StreamEvent> events;
    for(const auto& choice : json.at("choices")) {
        if(!choice.contains("delta")) {
            continue;
        }

        const nlohmann::json& delta = choice.at("delta");
        if(delta.contains("content")) {
            const std::string content = jsonStringOrEmpty(delta, "content");
            if(!content.empty()) {
                events.push_back(StreamEvent{StreamEventType::Delta, content, ""});
            }
        }

        if(delta.contains("tool_calls") && delta.at("tool_calls").is_array() && !delta.at("tool_calls").empty()) {
            events.push_back(StreamEvent{
                StreamEventType::ToolCallDelta,
                delta.at("tool_calls").dump(),
                "",
            });
        }
    }

    return events;
}

}  // namespace

// parseChunk 把一个或多个完整 SSE 事件块转换为统一事件列表。
std::vector<StreamEvent> SSEParser::parseChunk(const std::string& chunk) const {
    const std::string normalized = normalizeNewlines(chunk);
    std::vector<StreamEvent> events;

    size_t begin = 0;
    while(begin < normalized.size()) {
        const size_t end = normalized.find("\n\n", begin);
        const std::string block = normalized.substr(begin, end == std::string::npos ? std::string::npos : end - begin);

        std::vector<StreamEvent> block_events = parseEventBlock(block);
        events.insert(events.end(), block_events.begin(), block_events.end());

        if(end == std::string::npos) {
            break;
        }

        begin = end + 2U;  // 跳过作为事件分隔符的两个换行符。
        while(begin < normalized.size() && normalized[begin] == '\n') {
            ++begin;
        }
    }

    return events;
}

}  // namespace aiSDK
