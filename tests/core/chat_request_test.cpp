#include <gtest/gtest.h>

#include "core/ChatRequest.h"

namespace {

TEST(ChatRequestTest, SerializesMessagesAndToolsToOpenAICompatibleJson) {
    aiSDK::ChatRequest request;
    request.model = "deepseek-chat";
    request.stream = true;
    request.messages.push_back(aiSDK::SystemMessage("你是一个助手"));
    request.messages.push_back(aiSDK::UserMessage("现在东京几点"));
    request.tools.push_back(aiSDK::Tool{
        "get_current_time",
        "获取当前时间",
        nlohmann::json{
            {"type", "object"},
            {"properties", {{"timezone", {{"type", "string"}}}}},
        },
        aiSDK::ToolRiskLevel::Low,
    });

    const nlohmann::json json = aiSDK::chatRequestToJson(request);

    EXPECT_EQ(json.at("model"), "deepseek-chat");
    EXPECT_TRUE(json.at("stream").get<bool>());
    ASSERT_EQ(json.at("messages").size(), 2U);
    ASSERT_EQ(json.at("tools").size(), 1U);
    EXPECT_EQ(json.at("tools").front().at("function").at("name"), "get_current_time");
}

}  // namespace
