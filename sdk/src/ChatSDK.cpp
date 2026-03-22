#include "../include/ChatSDK.h"

#include "../include/util/Log.h"

namespace Ai_Chat_SDK {

ChatSDK::ChatSDK(const std::string& dbPath) : _sessionManager(dbPath) {
    // 构造函数
}

bool ChatSDK::init() {
    if(_isInitialized) {
        return true;
    }
    _isInitialized = true;
    INFO("ChatSDK initialized");
    return true;
}

bool ChatSDK::registerModel(const std::string& modelName, std::unique_ptr<LLMProvider> provider) {
    if(!_isInitialized) {
        WARN("ChatSDK is not initialized. Please call init() first.");
    }
    return _llmManager.registerModel(modelName, std::move(provider));
}

bool ChatSDK::initModel(const std::string& modelName, const std::map<std::string, std::string>& modelParam, const Config& config) {
    if(!_isInitialized) {
        WARN("ChatSDK is not initialized. Please call init() first.");
    }
    bool success = _llmManager.initModel(modelName, modelParam);
    if(success) {
        _modelConfigs[modelName] = config;
        INFO("ChatSDK initModel success for {}", modelName);
    } else {
        ERR("ChatSDK initModel failed for {}", modelName);
    }
    return success;
}

std::vector<LLMInfo> ChatSDK::getAvailableModels() const { return _llmManager.getAvailableModels(); }

std::string ChatSDK::createSession(const std::string& modelName) {
    if(!_llmManager.isAvailable(modelName)) {
        ERR("ChatSDK createSession failed: model {} is not available", modelName);
        return "";
    }
    std::string sessionId = _sessionManager.createSession(modelName);
    INFO("ChatSDK created session {} for model {}", sessionId, modelName);
    return sessionId;
}

std::shared_ptr<Session> ChatSDK::getSession(const std::string& sessionId) { return _sessionManager.getSession(sessionId); }

std::vector<std::string> ChatSDK::getSessionLists() const { return _sessionManager.getSessionLists(); }

bool ChatSDK::deleteSession(const std::string& sessionId) { return _sessionManager.deleteSession(sessionId); }

void ChatSDK::clearAllSessions() { _sessionManager.clearAllSessions(); }

std::string ChatSDK::sendMessage(const std::string& sessionId, const std::string& message) {
    if(!_isInitialized) {
        ERR("ChatSDK is not initialized");
        return "";
    }

    auto session = _sessionManager.getSession(sessionId);
    if(!session) {
        ERR("ChatSDK sendMessage failed: session {} not found", sessionId);
        return "";
    }

    // 1. 添加用户消息
    Message userMsg("user", message);
    if(!_sessionManager.addMessage(sessionId, userMsg)) {
        ERR("ChatSDK sendMessage failed: cannot add user message to session");
        return "";
    }

    // 2. 准备请求参数
    std::map<std::string, std::string> reqParam;
    auto it = _modelConfigs.find(session->_modelName);
    if(it != _modelConfigs.end()) {
        reqParam["temperature"] = std::to_string(it->second._temperature);
        reqParam["max_tokens"] = std::to_string(it->second._maxTokens);
    }

    // 3. 获取历史消息并发送请求
    std::vector<Message> history = _sessionManager.getHistroyMessages(sessionId);
    std::string response = _llmManager.sendMessage(session->_modelName, history, reqParam);

    // 4. 添加助手回复到会话
    if(!response.empty()) {
        Message assistantMsg("assistant", response);
        _sessionManager.addMessage(sessionId, assistantMsg);
    }

    // 5. 更新会话时间戳
    _sessionManager.updateSessionTimestamp(sessionId);

    return response;
}

std::string ChatSDK::sendMessageStream(const std::string& sessionId, const std::string& message, std::function<void(const std::string&, bool)> callback) {
    if(!_isInitialized) {
        ERR("ChatSDK is not initialized");
        return "";
    }

    auto session = _sessionManager.getSession(sessionId);
    if(!session) {
        ERR("ChatSDK sendMessageStream failed: session {} not found", sessionId);
        return "";
    }

    // 1. 添加用户消息
    Message userMsg("user", message);
    if(!_sessionManager.addMessage(sessionId, userMsg)) {
        ERR("ChatSDK sendMessageStream failed: cannot add user message to session");
        return "";
    }

    // 2. 准备请求参数
    std::map<std::string, std::string> reqParam;
    auto it = _modelConfigs.find(session->_modelName);
    if(it != _modelConfigs.end()) {
        reqParam["temperature"] = std::to_string(it->second._temperature);
        reqParam["max_tokens"] = std::to_string(it->second._maxTokens);
    }

    // 3. 获取历史消息并发送请求
    std::vector<Message> history = _sessionManager.getHistroyMessages(sessionId);
    std::string response = _llmManager.sendMessageStream(session->_modelName, history, reqParam, callback);

    // 4. 添加助手回复到会话
    if(!response.empty()) {
        Message assistantMsg("assistant", response);
        _sessionManager.addMessage(sessionId, assistantMsg);
    }

    // 5. 更新会话时间戳
    _sessionManager.updateSessionTimestamp(sessionId);

    return response;
}

}  // namespace Ai_Chat_SDK
