
#include "../include/GeminiProvider.h"

#include <httplib.h>
#include <jsoncpp/json/json.h>
#include <jsoncpp/json/reader.h>
#include <jsoncpp/json/value.h>
#include <jsoncpp/json/writer.h>

#include <cstddef>
#include <sstream>
#include <string>

#include "../include/util/Log.h"

namespace Ai_Chat_SDK {
// 初始化模型
bool GeminiProvider::initModel(const std::map<std::string, std::string>& ModelConfig) {
    auto it = ModelConfig.find("api_key");
    if(it == ModelConfig.end()) {
        ERR("GeminiProvider api_key not found");
        return false;
    }
    _apiKey = it->second;
    it = ModelConfig.find("end_point");
    if(it == ModelConfig.end()) {
        ERR("GeminiProvider end_point not found");
        return false;
    }
    _endpoint = it->second;
    _isAvailable = true;
    INFO("GeminiProvider init successed api_key : {} base_url : {}", _apiKey, _endpoint);
    return true;
}
// 获取模型名称
std::string GeminiProvider::getModelName() const { return "Gemini 3.1 Pro"; }
// 获取模型信息
std::string GeminiProvider::getModelDesc() const { return "Gemini, 一个富有洞察力且带点幽默感的全能 AI 协作伙伴, 致力于为你提供精准、地道且有温度的帮助"; }
// 检测模型是否有效
bool GeminiProvider::isAvailable() const { return _isAvailable; }
// 发送消息 全量返回
std::string GeminiProvider::sendMessage(const std::vector<Message>& messages, const std::map<std::string, std::string>& requestParam) { return ""; }
// 发送消息 增量返回 流式响应
// callback 处理增量数据
// // param1 : 增量数据
// // param2 : 是否为最后一个增量数据
std::string GeminiProvider::sendMessageStream(const std::vector<Message>& messages, const std::map<std::string, std::string>& requestParam,
                                              std::function<void(const std::string&, bool)> callback) {
    return "";
}
}  // namespace Ai_Chat_SDK
