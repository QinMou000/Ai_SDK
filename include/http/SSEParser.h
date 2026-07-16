#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aiSDK {

enum class StreamEventType { Delta, ToolCallDelta, Done, Error };

// ToolCallDelta 描述同一工具调用在流式响应中的一个不完整片段。
// index 是当前模型响应内的稳定位置；id、name 通常只在首片段出现，
// arguments_delta 则按接收顺序拼接为完整 JSON 参数文本。
struct ToolCallDelta {
    std::size_t index = 0U;
    std::optional<std::string> id;
    std::optional<std::string> name;
    std::string arguments_delta;
};

// StreamEvent 描述流式回调中的单个逻辑事件。
// 文本与错误保留字符串字段；工具调用使用供应商无关的结构化片段，
// 上层无需依赖 OpenAI-compatible SSE JSON 的具体嵌套结构。
struct StreamEvent {
    StreamEventType type = StreamEventType::Done;
    std::string delta;
    std::string error_message;
    std::vector<ToolCallDelta> tool_call_deltas;
};

using StreamCallback = std::function<void(const StreamEvent&)>;

// SSEParser 负责把 DeepSeek 返回的 SSE 文本块转换为统一事件。
// 调用方只需要保证传入的是一个或多个完整事件块即可。
class SSEParser {
   public:
    std::vector<StreamEvent> parseChunk(const std::string& chunk) const;
};

}  // namespace aiSDK
