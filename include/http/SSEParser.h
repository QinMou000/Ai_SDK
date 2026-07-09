#pragma once

#include <functional>
#include <string>
#include <vector>

namespace aiSDK {

enum class StreamEventType {
    Delta,
    ToolCallDelta,
    Done,
    Error
};

// StreamEvent 描述流式回调中的单个逻辑事件。
// 目前统一承载文本增量、工具调用增量、结束事件和错误事件。
struct StreamEvent {
    StreamEventType type = StreamEventType::Done;
    std::string delta;
    std::string error_message;
};

using StreamCallback = std::function<void(const StreamEvent&)>;

// SSEParser 负责把 DeepSeek 返回的 SSE 文本块转换为统一事件。
// 调用方只需要保证传入的是一个或多个完整事件块即可。
class SSEParser {
   public:
    std::vector<StreamEvent> parseChunk(const std::string& chunk) const;
};

}  // namespace aiSDK
