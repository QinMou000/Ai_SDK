#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "http/SSEParser.h"

namespace {

TEST(SSEParserTest, IgnoresNullContentChunksAndParsesDoneEvent) {
    const std::string chunk =
        "data: "
        "{\"choices\":[{\"delta\":{\"content\":null,\"role\":\"assistant\"},\"finish_reason\":null,\"index\":0}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"你好\"},\"finish_reason\":null,\"index\":0}]}\n\n"
        "data: [DONE]\n\n";

    aiSDK::SSEParser parser;
    const std::vector<aiSDK::StreamEvent> events = parser.parseChunk(chunk);

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].type, aiSDK::StreamEventType::Delta);
    EXPECT_EQ(events[0].delta, "你好");
    EXPECT_EQ(events[1].type, aiSDK::StreamEventType::Done);
}

// 工具调用以多个 SSE 事件抵达时，解析器必须保留每个调用的索引和字段分片。
// 上层只依赖这一公共结构，因此不必了解 OpenAI-compatible 的嵌套 JSON 形态。
TEST(SSEParserTest, ParsesStructuredInterleavedToolCallDeltas) {
    const std::string chunk =
        R"json(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call-first","function":{"name":"add","arguments":"{\"a\":1"}},{"index":1,"id":"call-second","function":{"name":"add","arguments":"{\"a\":2"}}]}}]})json"
        "\n\n"
        R"json(data: {"choices":[{"delta":{"tool_calls":[{"index":1,"function":{"arguments":",\"b\":4}"}},{"index":0,"function":{"arguments":",\"b\":3}"}}]}}]})json"
        "\n\n";

    aiSDK::SSEParser parser;
    const std::vector<aiSDK::StreamEvent> events = parser.parseChunk(chunk);

    ASSERT_EQ(events.size(), 2U);
    ASSERT_EQ(events[0].type, aiSDK::StreamEventType::ToolCallDelta);
    ASSERT_EQ(events[0].tool_call_deltas.size(), 2U);
    EXPECT_EQ(events[0].tool_call_deltas[0].index, 0U);
    ASSERT_TRUE(events[0].tool_call_deltas[0].id.has_value());
    EXPECT_EQ(*events[0].tool_call_deltas[0].id, "call-first");
    ASSERT_TRUE(events[0].tool_call_deltas[0].name.has_value());
    EXPECT_EQ(*events[0].tool_call_deltas[0].name, "add");
    EXPECT_EQ(events[0].tool_call_deltas[0].arguments_delta, "{\"a\":1");
    EXPECT_EQ(events[0].tool_call_deltas[1].index, 1U);
    EXPECT_EQ(events[0].tool_call_deltas[1].arguments_delta, "{\"a\":2");

    ASSERT_EQ(events[1].type, aiSDK::StreamEventType::ToolCallDelta);
    ASSERT_EQ(events[1].tool_call_deltas.size(), 2U);
    EXPECT_EQ(events[1].tool_call_deltas[0].index, 1U);
    EXPECT_EQ(events[1].tool_call_deltas[0].arguments_delta, ",\"b\":4}");
    EXPECT_EQ(events[1].tool_call_deltas[1].index, 0U);
    EXPECT_EQ(events[1].tool_call_deltas[1].arguments_delta, ",\"b\":3}");
}

// 非法索引在协议边界转化为 Error，不能把负数隐式转换为巨大的容器下标。
TEST(SSEParserTest, RejectsMalformedToolCallDelta) {
    const std::string chunk =
        R"json(data: {"choices":[{"delta":{"tool_calls":[{"index":-1,"function":{"name":"echo","arguments":"{}"}}]}}]})json"
        "\n\n";

    aiSDK::SSEParser parser;
    const std::vector<aiSDK::StreamEvent> events = parser.parseChunk(chunk);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, aiSDK::StreamEventType::Error);
    EXPECT_EQ(events.front().error_message, "流式工具调用增量格式无效");
}

}  // namespace
