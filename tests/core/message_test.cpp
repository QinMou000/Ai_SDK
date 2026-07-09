#include <gtest/gtest.h>

#include "core/Message.h"

namespace {

TEST(MessageTest, ConvertsRolesToAndFromString) {
    EXPECT_EQ(aiSDK::roleToString(aiSDK::Role::Assistant), "assistant");
    EXPECT_EQ(aiSDK::roleFromString("tool"), aiSDK::Role::Tool);
}

TEST(MessageTest, SerializesAndDeserializesMessageWithToolCalls) {
    aiSDK::Message message = aiSDK::AssistantMessage("需要调用工具");
    message.name = "planner";
    message.tool_calls.push_back(aiSDK::ToolCall{
        "call_001",
        "get_current_time",
        nlohmann::json{{"timezone", "Asia/Tokyo"}},
        R"({"timezone":"Asia/Tokyo"})",
    });

    const nlohmann::json json = aiSDK::messageToJson(message);
    const aiSDK::Message restored = aiSDK::messageFromJson(json);

    EXPECT_EQ(restored.role, aiSDK::Role::Assistant);
    ASSERT_TRUE(restored.name.has_value());
    EXPECT_EQ(*restored.name, "planner");
    ASSERT_EQ(restored.tool_calls.size(), 1U);
    EXPECT_EQ(restored.tool_calls.front().name, "get_current_time");
    EXPECT_EQ(restored.tool_calls.front().arguments.at("timezone"), "Asia/Tokyo");
}

TEST(MessageTest, BuildsToolMessageWithCallId) {
    const aiSDK::Message message = aiSDK::ToolMessage("done", "call_123");

    EXPECT_EQ(message.role, aiSDK::Role::Tool);
    ASSERT_TRUE(message.tool_call_id.has_value());
    EXPECT_EQ(*message.tool_call_id, "call_123");
}

}  // namespace
