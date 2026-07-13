#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <stdexcept>

#include "AIClient.h"

namespace {

TEST(AIClientSmokeTest, SupportsDeepSeekAndRejectsUnsupportedProvider) {
    aiSDK::Config config;
    config.default_provider = "deepseek";

    aiSDK::AIClient client(config);
    EXPECT_NO_THROW(client.setProvider("deepseek"));
    EXPECT_THROW(client.setProvider("minimax"), std::invalid_argument);
}

TEST(AIClientSmokeTest, RejectsEmptyProviderName) {
    aiSDK::Config config;
    config.default_provider = "deepseek";

    aiSDK::AIClient client(config);
    EXPECT_THROW(client.setProvider(""), std::invalid_argument);
}

TEST(AIClientSmokeTest, ExecutesRegisteredToolCallsWithoutStartingAnotherChat) {
    // 构造客户端只会创建 Provider 对象，真正请求前才要求 API Key；
    // 因此这个冒烟用例可以完全离线验证 AIClient 的工具门面。
    aiSDK::Config config;
    config.default_provider = "deepseek";
    aiSDK::AIClient client(config);

    // echo 工具原样返回参数，便于证明注册表和 AIClient 之间没有额外转换。
    client.tools().registerTool(
        aiSDK::Tool{
            "echo",
            "原样返回参数",
            nlohmann::json{{"type", "object"}},
            aiSDK::ToolRiskLevel::Low,
        },
        [](const nlohmann::json& arguments) { return aiSDK::ToolResult::successResult(arguments); });

    // 该入口仅执行调用方传入的一批本地 Tool Call，
    // 不需要 API Key、不会发起网络请求，也不会管理任何会话历史。
    const auto results = client.executeToolCalls({
        {"call_echo", "echo", nlohmann::json{{"text", "你好"}}, R"({"text":"你好"})"},
    });

    // 结果仍携带处理函数返回的结构化 JSON，调用方可按需转换为 ToolMessage。
    ASSERT_EQ(results.size(), 1U);
    EXPECT_TRUE(results.front().result.success);
    EXPECT_EQ(results.front().result.data.at("text"), "你好");
}

}  // namespace
