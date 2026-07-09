#include <cstdlib>
#include <filesystem>
#include <optional>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "provider/DeepSeekProvider.h"

namespace {

// readEnv 统一封装环境变量读取，避免在 MSVC 下继续触发 getenv 告警。
std::string readEnv(const char* key) {
#ifdef _WIN32
    char* value = nullptr;
    size_t size = 0;
    if(_dupenv_s(&value, &size, key) != 0 || value == nullptr) {
        return "";
    }

    const std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(key);
    return value == nullptr ? "" : std::string(value);
#endif
}

std::string getOptionalEnv(const char* key, const char* fallback) {
    const std::string value = readEnv(key);
    if(value.empty()) {
        return fallback;
    }
    return value;
}

std::optional<aiSDK::ProviderConfig> loadProviderConfigFromEnv() {
    // 在线测试也先自动加载最近的 .env，保持和示例一致的本地体验。
    aiSDK::loadNearestEnvFile(std::filesystem::current_path());

    const std::string api_key = readEnv("DEEPSEEK_API_KEY");
    if(api_key.empty()) {
        return std::nullopt;
    }

    aiSDK::ProviderConfig config;
    config.api_key = api_key;
    config.base_url = getOptionalEnv("DEEPSEEK_BASE_URL", "https://api.deepseek.com");
    config.default_model = getOptionalEnv("DEEPSEEK_MODEL", "deepseek-v4-flash");
    return config;
}

TEST(DeepSeekProviderTest, CallsRealChatCompletionApi) {
    const std::optional<aiSDK::ProviderConfig> config = loadProviderConfigFromEnv();
    if(!config.has_value()) {
        GTEST_SKIP() << "缺少环境变量: DEEPSEEK_API_KEY";
    }

    aiSDK::DeepSeekProvider provider(*config);

    aiSDK::ChatRequest request;
    request.messages.push_back(aiSDK::SystemMessage("你是 SDK 集成测试助手，请简洁回答。"));
    request.messages.push_back(aiSDK::UserMessage("请用一句中文介绍你自己。"));

    const aiSDK::ChatResponse response = provider.chat(request);
    const nlohmann::json raw = nlohmann::json::parse(response.raw_response);

    EXPECT_EQ(response.message.role, aiSDK::Role::Assistant);
    EXPECT_FALSE(response.content.empty());
    EXPECT_TRUE(raw.contains("choices"));
    EXPECT_TRUE(raw.contains("usage"));
}

TEST(DeepSeekProviderTest, StreamsFromRealApi) {
    const std::optional<aiSDK::ProviderConfig> config = loadProviderConfigFromEnv();
    if(!config.has_value()) {
        GTEST_SKIP() << "缺少环境变量: DEEPSEEK_API_KEY";
    }

    aiSDK::DeepSeekProvider provider(*config);

    aiSDK::ChatRequest request;
    request.messages.push_back(aiSDK::UserMessage("请输出一句简短的中文问候语。"));

    std::vector<aiSDK::StreamEvent> events;
    provider.streamChat(request, [&](const aiSDK::StreamEvent& event) {
        events.push_back(event);
    });

    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().type, aiSDK::StreamEventType::Done);

    bool saw_delta = false;
    for(const auto& event : events) {
        if(event.type == aiSDK::StreamEventType::Delta && !event.delta.empty()) {
            saw_delta = true;
            break;
        }
    }
    EXPECT_TRUE(saw_delta);
}

}  // namespace
