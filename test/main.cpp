#include <gtest/gtest.h>
#include <spdlog/common.h>

#include <memory>
#include <vector>

#include "../sdk/include/ChatGPTProvider.h"
#include "../sdk/include/DeepSeekProvider.h"
#include "../sdk/include/GeminiProvider.h"
#include "../sdk/include/LLMManager.h"
#include "../sdk/include/LocalLLMProvider.h"
#include "../sdk/include/util/Log.h"

// #define LLMMANAGER
// #define DeepSeek
#define ChatGPT
// #define LocalLLM

int main(int argc, char** argv) {
    // 初始化 spdlog
    Util::Logger::initLogger("test", "stdout", spdlog::level::debug);
    // 初始化 gtest库
    testing::InitGoogleTest(&argc, argv);
    // 执行所有测试用例
    return RUN_ALL_TESTS();
}

#ifdef DeepSeek
TEST(DeepSeekProviderTest, sendMessage) {
    auto provider = std::make_shared<Ai_Chat_SDK::DeepSeekProvider>();
    ASSERT_TRUE(provider);

    std::map<std::string, std::string> ModelConfig = {{"api_key", "your api key"}, {"end_point", "https://api.deepseek.com"}};
    ASSERT_TRUE(provider->initModel(ModelConfig));

    std::map<std::string, std::string> requestParam{{"temperature", "0.7"}, {"max_tokens", "2048"}};
    std::vector<Ai_Chat_SDK::Message> messages;
    messages.push_back({"user", "你好,0.9和0.11哪个大?"});

    auto callback = [](const std::string& content, bool isFinish) {
        INFO("content : {}", content);
        if(isFinish) INFO("[DONE]");
    };

    std::string response = provider->sendMessageStream(messages, requestParam, callback);
    INFO("content : {}", response);
    ASSERT_FALSE(response.empty());
}
#endif
#ifdef ChatGPT
TEST(ChatGPTProviderTest, sendMessage) {
    auto provider = std::make_shared<Ai_Chat_SDK::ChatGPTProvider>();
    ASSERT_TRUE(provider);

    std::map<std::string, std::string> ModelConfig = {{"api_key", "sk-3f74f6a7ca32e25f584a4dea139ec60100f2eda580616c1625217725d8dbb851"},
                                                      {"end_point", "https://echocode.pro"}};
    ASSERT_TRUE(provider->initModel(ModelConfig));

    std::map<std::string, std::string> requestParam{{"temperature", "0.7"}, {"max_tokens", "2048"}};
    std::vector<Ai_Chat_SDK::Message> messages;
    messages.push_back({"user", "你好,0.9和0.11哪个大?"});

    auto callback = [](const std::string& content, bool isFinish) {
        INFO("content : {}", content);
        if(isFinish) INFO("[DONE]");
    };

    std::string response = provider->sendMessageStream(messages, requestParam, callback);
    INFO("content : {}", response);
    ASSERT_FALSE(response.empty());
}
#endif

#ifdef LocalLLM
TEST(LocalLLMProviderTest, sendMessage) {
    auto provider = std::make_shared<Ai_Chat_SDK::LocalLLMProvider>();
    ASSERT_TRUE(provider);

    std::map<std::string, std::string> ModelConfig = {{"api_key", ""}, {"end_point", "http://192.168.0.96:1234"}};
    ASSERT_TRUE(provider->initModel(ModelConfig));

    std::map<std::string, std::string> requestParam{{"temperature", "0.1"}, {"max_tokens", "4096"}};
    std::vector<Ai_Chat_SDK::Message> messages;
    messages.push_back({"user", "你给我讲个笑话吧"});

    auto callback = [](const std::string& content, bool isFinish) {
        INFO("content : {}", content);
        if(isFinish) INFO("[DONE]");
    };

    std::string response = provider->sendMessageStream(messages, requestParam, callback);
    INFO("content : {}", response);
    ASSERT_FALSE(response.empty());
}
#endif
#ifdef LLMMANAGER
TEST(LLMManagerTest, sendMessage) {
    std::unique_ptr<Ai_Chat_SDK::LLMProvider> provider = std::make_unique<Ai_Chat_SDK::LocalLLMProvider>();
    ASSERT_TRUE(provider);
    std::string modelName = provider->getModelName();

    auto manager = std::make_shared<Ai_Chat_SDK::LLMManager>();
    manager->registerModel(modelName, std::move(provider));

    std::map<std::string, std::string> ModelConfig = {{"api_key", ""}, {"end_point", "http://192.168.0.96:1234"}};
    manager->initModel(modelName, ModelConfig);

    std::map<std::string, std::string> requestParam{{"temperature", "0.1"}, {"max_tokens", "4096"}};
    std::vector<Ai_Chat_SDK::Message> messages;
    messages.push_back({"user", "你给我讲个笑话吧"});
    std::string response = manager->sendMessage(modelName, messages, requestParam);
    INFO("content : {}", response);
    ASSERT_FALSE(response.empty());
}
#endif