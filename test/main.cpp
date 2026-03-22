#include <gtest/gtest.h>
#include <spdlog/common.h>

#include <memory>
#include <vector>

#include "../sdk/include/ChatGPTProvider.h"
#include "../sdk/include/DataManager.h"
#include "../sdk/include/DeepSeekProvider.h"
#include "../sdk/include/GeminiProvider.h"
#include "../sdk/include/LLMManager.h"
#include "../sdk/include/LocalLLMProvider.h"
#include "../sdk/include/SessionManager.h"
#include "../sdk/include/ChatSDK.h"
#include "../sdk/include/util/Log.h"

// #define LLMMANAGER
// #define DeepSeek
// #define ChatGPT
// #define LocalLLM
// #define SESSIONMANAGER
#define CHATSDK

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

#ifdef CHATSDK
TEST(ChatSDKTest, basicOperations) {
    // 1. 初始化 SDK
    Ai_Chat_SDK::ChatSDK sdk("test_chatsdk.db");
    ASSERT_TRUE(sdk.init());

    // 2. 清理环境，防止脏数据干扰
    sdk.clearAllSessions();

    // 3. 注册并初始化模型
    auto provider = std::make_unique<Ai_Chat_SDK::LocalLLMProvider>();
    std::string modelName = provider->getModelName();
    ASSERT_TRUE(sdk.registerModel(modelName, std::move(provider)));

    std::map<std::string, std::string> modelParam = {{"api_key", ""}, {"end_point", "http://192.168.0.96:1234"}};
    Ai_Chat_SDK::Config config;
    config._temperature = 0.1;
    config._maxTokens = 4096;
    ASSERT_TRUE(sdk.initModel(modelName, modelParam, config));

    // 4. 获取可用模型
    auto models = sdk.getAvailableModels();
    ASSERT_FALSE(models.empty());
    bool found = false;
    for (const auto& m : models) {
        if (m._modelName == modelName) {
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);

    // 5. 创建会话
    std::string sessionId = sdk.createSession(modelName);
    ASSERT_FALSE(sessionId.empty());

    // 6. 获取会话列表
    auto sessions = sdk.getSessionLists();
    ASSERT_EQ(sessions.size(), 1);
    ASSERT_EQ(sessions[0], sessionId);

    // 7. 发送消息并获取回复 (全量)
    std::string reply = sdk.sendMessage(sessionId, "你好，请回复'Hello'");
    INFO("ChatSDK Reply: {}", reply);
    ASSERT_FALSE(reply.empty());

    // 8. 验证会话消息记录
    auto session = sdk.getSession(sessionId);
    ASSERT_NE(session, nullptr);
    ASSERT_EQ(session->_messages.size(), 2);
    ASSERT_EQ(session->_messages[0]._role, "user");
    ASSERT_EQ(session->_messages[0]._content, "你好，请回复'Hello'");
    ASSERT_EQ(session->_messages[1]._role, "assistant");
    ASSERT_EQ(session->_messages[1]._content, reply);

    // 9. 删除会话
    ASSERT_TRUE(sdk.deleteSession(sessionId));
    sessions = sdk.getSessionLists();
    ASSERT_TRUE(sessions.empty());
}
#endif

#ifdef SESSIONMANAGER
TEST(SessionManagerTest, basicOperations) {
    auto manager = std::make_shared<Ai_Chat_SDK::SessionManager>("test_chat.db");

    // 1. 测试清空所有会话 (清理环境)
    manager->clearAllSessions();
    ASSERT_EQ(manager->getSessionCount(), 0);

    // 2. 测试创建会话
    std::string modelName = "test_model";
    std::string sessionId = manager->createSession(modelName);
    ASSERT_FALSE(sessionId.empty());
    ASSERT_EQ(manager->getSessionCount(), 1);

    // 3. 测试获取会话
    auto session = manager->getSession(sessionId);
    ASSERT_NE(session, nullptr);
    ASSERT_EQ(session->_modelName, modelName);
    ASSERT_EQ(session->_sessionId, sessionId);

    // 4. 测试添加消息
    Ai_Chat_SDK::Message msg1("user", "Hello!");
    ASSERT_TRUE(manager->addMessage(sessionId, msg1));

    Ai_Chat_SDK::Message msg2("assistant", "Hi there!");
    ASSERT_TRUE(manager->addMessage(sessionId, msg2));

    // 5. 测试获取历史消息
    auto history = manager->getHistroyMessages(sessionId);
    ASSERT_EQ(history.size(), 2);
    ASSERT_EQ(history[0]._role, "user");
    ASSERT_EQ(history[0]._content, "Hello!");
    ASSERT_EQ(history[1]._role, "assistant");
    ASSERT_EQ(history[1]._content, "Hi there!");

    // 6. 测试会话列表排序 (后活动的排在前面)
    std::string sessionId2 = manager->createSession("test_model_2");
    Ai_Chat_SDK::Message msg3("user", "Update session 2");
    manager->addMessage(sessionId2, msg3);

    auto sessionLists = manager->getSessionLists();
    ASSERT_EQ(sessionLists.size(), 2);
    // session2 最近有消息更新，所以应该排在第一位
    ASSERT_EQ(sessionLists[0], sessionId2);
    ASSERT_EQ(sessionLists[1], sessionId);

    // 7. 测试重启恢复 (模拟DataManager从数据库加载)
    // 销毁旧的manager，重新创建一个新的manager来模拟程序重启
    manager.reset();
    auto newManager = std::make_shared<Ai_Chat_SDK::SessionManager>("test_chat.db");

    // 这里应该是 2 个会话，因为前面的代码中并没有删除任何会话
    ASSERT_EQ(newManager->getSessionCount(), 2);
    auto recoveredSession = newManager->getSession(sessionId);
    ASSERT_NE(recoveredSession, nullptr);
    // 检查是否懒加载成功恢复了消息
    auto recoveredHistory = newManager->getHistroyMessages(sessionId);
    ASSERT_EQ(recoveredHistory.size(), 2);
    ASSERT_EQ(recoveredHistory[0]._content, "Hello!");

    // 8. 测试删除会话
    ASSERT_TRUE(newManager->deleteSession(sessionId));
    ASSERT_EQ(newManager->getSessionCount(), 1);
    ASSERT_EQ(newManager->getSession(sessionId), nullptr);
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