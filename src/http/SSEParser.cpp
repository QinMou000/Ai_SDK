#include "http/SSEParser.h"

#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
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

// readOptionalString 仅接受字符串、null 或字段缺失，拒绝协议中不应出现的其他类型。
// 工具调用分片会跨多个事件到达，必须保留“本次没有该字段”和“字段格式错误”的区别。
bool readOptionalString(const nlohmann::json& json, const char* key, std::optional<std::string>& value) {
    if(!json.contains(key) || json.at(key).is_null()) {
        return true;
    }

    if(!json.at(key).is_string()) {
        return false;
    }

    value = json.at(key).get<std::string>();
    return true;
}

// readToolCallIndex 校验供应商协议中的数组位置，避免负数或浮点数被静默转换为容器下标。
// Agent 后续按该下标聚合分片，因此这里必须在协议边界拒绝不确定的索引。
bool readToolCallIndex(const nlohmann::json& json, std::size_t& index) {
    if(!json.contains("index")) {
        return false;
    }

    const nlohmann::json& raw_index = json.at("index");
    if(raw_index.is_number_unsigned()) {
        const auto value = raw_index.get<nlohmann::json::number_unsigned_t>();
        if(value > static_cast<nlohmann::json::number_unsigned_t>(std::numeric_limits<std::size_t>::max())) {
            return false;
        }
        index = static_cast<std::size_t>(value);
        return true;
    }

    if(raw_index.is_number_integer()) {
        const auto value = raw_index.get<nlohmann::json::number_integer_t>();
        if(value < 0 || static_cast<nlohmann::json::number_unsigned_t>(value) >
                            static_cast<nlohmann::json::number_unsigned_t>(std::numeric_limits<std::size_t>::max())) {
            return false;
        }
        index = static_cast<std::size_t>(value);
        return true;
    }

    return false;
}

// parseToolCallDeltas 将供应商的嵌套 JSON 收敛成 SDK 公共的结构化分片。
// 之后的 Agent 只消费 ToolCallDelta，不依赖某个 Provider 的 JSON 字段布局。
bool parseToolCallDeltas(const nlohmann::json& tool_calls, std::vector<ToolCallDelta>& deltas) {
    if(!tool_calls.is_array()) {
        return false;
    }

    for(const nlohmann::json& tool_call : tool_calls) {
        if(!tool_call.is_object()) {
            return false;
        }

        ToolCallDelta delta;
        if(!readToolCallIndex(tool_call, delta.index) || !readOptionalString(tool_call, "id", delta.id)) {
            return false;
        }

        if(!tool_call.contains("function") || tool_call.at("function").is_null()) {
            deltas.push_back(std::move(delta));
            continue;
        }

        const nlohmann::json& function = tool_call.at("function");
        std::optional<std::string> arguments_delta;
        if(!function.is_object() || !readOptionalString(function, "name", delta.name) ||
           !readOptionalString(function, "arguments", arguments_delta)) {
            return false;
        }
        if(arguments_delta.has_value()) {
            delta.arguments_delta = std::move(*arguments_delta);
        }
        deltas.push_back(std::move(delta));
    }

    return true;
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

        if(delta.contains("tool_calls") && !delta.at("tool_calls").is_null()) {
            std::vector<ToolCallDelta> tool_call_deltas;
            if(!parseToolCallDeltas(delta.at("tool_calls"), tool_call_deltas)) {
                return {errorEvent("流式工具调用增量格式无效")};
            }
            if(!tool_call_deltas.empty()) {
                StreamEvent event;
                event.type = StreamEventType::ToolCallDelta;
                event.tool_call_deltas = std::move(tool_call_deltas);
                events.push_back(std::move(event));
            }
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
