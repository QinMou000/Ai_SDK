#include "../include/LLMManager.h"

#include <vector>

#include "../include/util/Log.h"

namespace Ai_Chat_SDK {
// 注册模型到manager
bool LLMManager::registerModel(const std::string& modelName, std::unique_ptr<LLMProvider> provider) {
    if(!provider) {
        WARN("LLMManager::registerModel provider is nullptr");
        return false;
    }
    if(modelName.empty()) {
        WARN("LLMManager::registerModel modelName is null");
        return false;
    }
    // 判断模型是否已经注册
    if(_modelProvider.find(modelName) != _modelProvider.end()) {
        WARN("LLMManager::registerModel {} have been registered", modelName);
        return false;
    }
    _modelProvider[modelName] = std::move(provider);  // 通过move转移unique_ptr资源

    _modelInfo[modelName] = LLMInfo(modelName);  // 模型的 desc , base url 和 api key 在初始化时写入 _isAvailable _provider // TODO
    INFO("LLMManager::registerModel model:{} register successed", modelName);
    return true;
}
// 初始化模型
bool LLMManager::initModel(const std::string& modelName, const std::map<std::string, std::string>& modelParama) {
    // 判断模型是否已经注册
    auto it = _modelProvider.find(modelName);
    if(it == _modelProvider.end()) {
        WARN("LLMManager::initModel Please register the model first modelName : {}", modelName);
        return false;
    }
    if(!it->second->initModel(modelParama)) {
        WARN("LLMManager::initModel model init failed modelName : {}", modelName);
        return false;
    }
    _modelInfo[modelName]._modelDesc = it->second->getModelDesc();
    _modelInfo[modelName]._isAvailable = true;
    INFO("LLMManager::initModel init model:{} successed", modelName);
    return true;
}
// 查看该模型是否可用
bool LLMManager::isAvailable(const std::string& modelName) const {
    // 确保该模型存在
    auto it = _modelProvider.find(modelName);
    if(it == _modelProvider.end()) {
        WARN("LLMManager::isAvailable this model:{} is not exsist", modelName);
        return false;
    }
    return it->second->isAvailable();
}  // 获取当前manager可用模型

// 这里依然有 双源可用性问题 后续考虑用provider进行统一判断 TODO
std::vector<LLMInfo> LLMManager::getAvailableModels() const {
    std::vector<LLMInfo> ret;
    for(const auto& pair : _modelInfo) {
        if(pair.second._isAvailable) {
            ret.push_back(pair.second);
        }
    }
    return ret;
}
// 发送信息 -- 全量响应
std::string LLMManager::sendMessage(const std::string& modelName, const std::vector<Message>& messages,
                                    const std::map<std::string, std::string>& requestParam) const {
    auto it = _modelProvider.find(modelName);

    if(it == _modelProvider.end()) {
        WARN("LLMManager::sendMessage model not be register");
        return "";
    }
    if(!it->second->isAvailable()) {
        WARN("LLMManager::sendMessage model is not available");
        return "";
    }
    return it->second->sendMessage(messages, requestParam);
}
// 发送信息 -- 流式响应
std::string LLMManager::sendMessageStream(const std::string& modelName, const std::vector<Message>& messages,
                                          const std::map<std::string, std::string>& requestParam,
                                          std::function<void(const std::string&, bool)> callback) const {
    auto it = _modelProvider.find(modelName);

    if(it == _modelProvider.end()) {
        WARN("LLMManager::sendMessageStream model not be register");
        return "";
    }
    if(!it->second->isAvailable()) {
        WARN("LLMManager::sendMessageStream model is not available");
        return "";
    }
    return it->second->sendMessageStream(messages, requestParam, callback);
}
}  // namespace Ai_Chat_SDK