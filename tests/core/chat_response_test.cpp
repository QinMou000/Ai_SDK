#include <gtest/gtest.h>

#include "core/ChatResponse.h"

namespace {

TEST(ChatResponseTest, BuildsAssistantTextResponse) {
    const aiSDK::ChatResponse response = aiSDK::assistantTextResponse("你好");

    EXPECT_EQ(response.message.role, aiSDK::Role::Assistant);
    EXPECT_EQ(response.message.content, "你好");
    EXPECT_EQ(response.content, "你好");
    EXPECT_FALSE(response.hasToolCalls());
}

TEST(ChatResponseTest, SerializesUsageAndToolCalls) {
    aiSDK::ChatResponse response = aiSDK::assistantTextResponse("调用完成");
    response.tool_calls.push_back(aiSDK::ToolCall{
        "call_1",
        "calculator",
        nlohmann::json{{"expression", "1+2"}},
        R"({"expression":"1+2"})",
    });
    response.usage = aiSDK::Usage{10, 20, 30};
    response.raw_response = "{\"id\":\"resp_1\"}";

    const nlohmann::json json = aiSDK::chatResponseToJson(response);

    EXPECT_EQ(json.at("usage").at("total_tokens"), 30);
    ASSERT_EQ(json.at("tool_calls").size(), 1U);
    EXPECT_EQ(json.at("tool_calls").front().at("name"), "calculator");
    EXPECT_EQ(json.at("raw_response"), "{\"id\":\"resp_1\"}");
}

}  // namespace
