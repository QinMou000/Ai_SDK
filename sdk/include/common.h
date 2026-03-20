#pragma once
#include <ctime>
#include <string>
#include <vector>

namespace Ai_Chat_SDK {

// 消息结构
struct Message {
    std::string _messageId;  // 消息id
    std::string _role;       // 消息角色
    std::string _content;    // 消息内容
    std::time_t _timestamp;  // 消息时间戳

    Message(const std::string& role, const std::string& content) : _role(role), _content(content) {}
};

// 所有模型的公共配置信息
struct Config {
    std::string _modelName;     // 模型名称
    double _temperature = 0.7;  // 温度参数，控制生成文本的随机性
    int _maxTokens = 2048;      // 最大令牌数，限制生成文本的长度
};

// 通过API 调用的云端模型配置
struct CloudConfig : public Config {
    std::string _apiKey;  // API密钥，用于身份验证
};

// 本地模型
// struct LocalLLMConfig : public Config {

// }

// LLM具体信息
struct LLMInfo {
    std::string _modelName;     // 模型名称
    std::string _modelDesc;     // 模型描述
    std::string _provider;      // 模型提供者
    std::string _endpoint;      // 模型API endpoint base url
    bool _isAvailable = false;  // 是否可用

    LLMInfo(const std::string& modelName = "", const std::string& modelDesc = "", const std::string& provider = "", const std::string& endpoint = "")
        : _modelName(modelName), _modelDesc(modelDesc), _provider(provider), _endpoint(endpoint) {}
};

// 回话信息
struct Session {
    std::string _sessionId;          // 会话id
    std::string _modelName;          // 模型名称
    std::vector<Message> _messages;  // 会话消息列表
    std::time_t _createAt;           // 会话开始时间戳
    std::time_t _updateAt;           // 会话最后活跃时间戳

    Session(const std::string& modelName = "") : _modelName(modelName) {}
};

}  // namespace Ai_Chat_SDK
