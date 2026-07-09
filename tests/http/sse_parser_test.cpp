#include <gtest/gtest.h>

#include "http/SSEParser.h"

namespace {

TEST(SSEParserTest, IgnoresNullContentChunksAndParsesDoneEvent) {
    const std::string chunk =
        "data: {\"choices\":[{\"delta\":{\"content\":null,\"role\":\"assistant\"},\"finish_reason\":null,\"index\":0}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"你好\"},\"finish_reason\":null,\"index\":0}]}\n\n"
        "data: [DONE]\n\n";

    aiSDK::SSEParser parser;
    const std::vector<aiSDK::StreamEvent> events = parser.parseChunk(chunk);

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].type, aiSDK::StreamEventType::Delta);
    EXPECT_EQ(events[0].delta, "你好");
    EXPECT_EQ(events[1].type, aiSDK::StreamEventType::Done);
}

}  // namespace
