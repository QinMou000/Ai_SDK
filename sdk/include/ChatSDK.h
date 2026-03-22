#pragma once
#include <functional>

#include "LLMManager.h"
#include "SessionManager.h"

namespace Ai_Chat_SDK {
class ChatSDK {
    // 链接LLManager和SessionManager
    // 为用户提供统一接口
   public:
    ChatSDK(const std::string& dbPath = "chat.db");
    ~ChatSDK() = default;

    // 初始化 SDK (保留接口，目前可在构造中直接初始化或由调用方控制)
    bool init();
    // 注册模型
    bool registerModel(const std::string& modelName, std::unique_ptr<LLMProvider> provider);
    // 初始化并配置模型
    bool initModel(const std::string& modelName, const std::map<std::string, std::string>& modelParam, const Config& config);
    // 获取所有可用模型
    std::vector<LLMInfo> getAvailableModels() const;
    // 创建新会话
    std::string createSession(const std::string& modelName);
    // 获取会话信息
    std::shared_ptr<Session> getSession(const std::string& sessionId);
    // 获取所有会话列表
    std::vector<std::string> getSessionLists() const;
    // 删除会话
    bool deleteSession(const std::string& sessionId);
    // 清空所有会话
    void clearAllSessions();
    // 发送消息 (全量响应)
    std::string sendMessage(const std::string& sessionId, const std::string& message);
    // 发送消息 (流式响应)
    std::string sendMessageStream(const std::string& sessionId, const std::string& message, std::function<void(const std::string&, bool)> callback);

   private:
    bool _isInitialized = false;
    std::unordered_map<std::string, Config> _modelConfigs;
    SessionManager _sessionManager;
    LLMManager _llmManager;
};
}  // namespace Ai_Chat_SDK