#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "LLMProvider.h"
#include "common.h"

namespace Ai_Chat_SDK {
class LLMManager {
   public:
    // 注册模型到manager
    bool registerModel(const std::string& modelName, std::unique_ptr<LLMProvider> provider);
    // 初始化模型
    bool initModel(const std::string& modelName, const std::map<std::string, std::string>& modelParama);
    // 查看该模型是否可用
    bool isAvailable(const std::string& modelName) const;
    // 获取当前manager可用模型
    std::vector<LLMInfo> getAvailableModels() const;
    // 发送信息 -- 全量响应
    std::string sendMessage(const std::string& modelName, const std::vector<Message>& messages, const std::map<std::string, std::string>& requestParam) const;
    // 发送信息 -- 流式响应
    std::string sendMessageStream(const std::string& modelName, const std::vector<Message>& messages, const std::map<std::string, std::string>& requestParam,
                                  std::function<void(const std::string&, bool)> callback) const;

   private:
    // Info 里面的 isAvailable 只是镜像备份
    // 不会拿它做逻辑判断 只是一个冗余变量
    // 一切以 provider里面的真实isAvailable为准
    std::map<std::string, LLMInfo> _modelInfo;
    std::map<std::string, std::unique_ptr<LLMProvider>> _modelProvider;
};

}  // namespace Ai_Chat_SDK