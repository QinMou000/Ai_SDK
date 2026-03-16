#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "common.h"

namespace Ai_Chat_SDK {
// 抽象类
class LLMProvider {
   public:
    // 初始化模型
    virtual bool initModel(const std::map<std::string, std::string>& ModelConfig) = 0;
    // 获取模型名称
    virtual std::string getModelName() const = 0;
    // 获取模型信息
    virtual std::string getModelDesc() const = 0;
    // 检测模型是否有效
    virtual bool isAvailable() const = 0;
    // 发送消息 全量返回
    virtual std::string sendMessage(const std::vector<Message> messages, const std::map<std::string, std::string> requestParam) = 0;
    // 发送消息 增量返回 流式响应
    // callback 处理增量数据
    // // param1 : 增量数据
    // // param2 : 是否为最后一个增量数据
    virtual std::string sendMessageStream(const std::vector<Message> messages, const std::map<std::string, std::string> requestParam, std::function<void(std::string&, bool)> callback) = 0;

   protected:                   // 确保在子类中可以访问
    bool _isAvailable = false;  // 模型是否可用
    std::string _apiKey;        // API 密钥
    std::string _endpoint;      // API base URL
};

}  // namespace Ai_Chat_SDK