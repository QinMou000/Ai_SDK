#include "../include/DeepSeekProvider.h"

#include <httplib.h>
#include <jsoncpp/json/json.h>
#include <jsoncpp/json/reader.h>
#include <jsoncpp/json/value.h>
#include <jsoncpp/json/writer.h>

#include <cstddef>
#include <string>

#include "../include/util/Log.h"

namespace Ai_Chat_SDK {
// 初始化模型基本参数
bool DeepSeekProvider::initModel(const std::map<std::string, std::string>& ModelConfig) {
    // 1. 初始化 api_key
    auto it = ModelConfig.find("api_key");
    if(it == ModelConfig.end()) {
        ERR("DeepSeekProvider api_key not found");
        return false;
    }
    _apiKey = it->second;
    // 2. 初始化 base url
    it = ModelConfig.find("end_point");
    if(it == ModelConfig.end()) {
        ERR("DeepSeekProvider end_point not found");
        return false;
    }
    _endpoint = it->second;

    _isAvailable = true;
    INFO("DeepSeekProvider init successed api_key : {} base_url : {}", _apiKey, _endpoint);
    return true;
}

// 检测模型是否可用
bool DeepSeekProvider::isAvailable() const { return _isAvailable; }

// 获取模型名称
std::string DeepSeekProvider::getModelName() const { return "deepseek-chat"; }

// 获取模型信息
std::string DeepSeekProvider::getModelDesc() const {
    return "DeepSeek是由深度求索公司开发的免费大语言模型,支持1M超长上下文、多格式文档处理、联网搜索和语音输入,可高效完成对话问答、文档分析和信息检索等任务";
}

// 发送消息 全量返回
std::string DeepSeekProvider::sendMessage(const std::vector<Message> messages, const std::map<std::string, std::string> requestParam) {
    // 1. 判断模型是否可用
    if(!isAvailable()) {
        ERR("DeepSeekProvider::sendMessage model not available");
        return "";
    }
    // 2. 构造请求参数
    double temperature = 0.7;
    int max_tokens = 2048;
    auto it = requestParam.find("temperature");
    if(it != requestParam.end()) {
        temperature = std::stod(it->second);
    }
    it = requestParam.find("max_tokens");
    if(it != requestParam.end()) {
        max_tokens = std::stoi(it->second);
    }
    // 3. 构造历史消息
    Json::Value MessageArr(Json::arrayValue);
    // 将历史所有消息统一构建成一个json数组
    for(const auto message : messages) {
        Json::Value MessageObj;
        MessageObj["role"] = message._role;
        MessageObj["content"] = message._content;
        MessageArr.append(MessageObj);
    }
    // 4. 构造请求体
    Json::Value RequestBody;
    RequestBody["model"] = getModelName();
    RequestBody["messages"] = MessageArr;
    RequestBody["temperature"] = temperature;
    RequestBody["max_tokens"] = max_tokens;

    // 5. 序列化
    Json::StreamWriterBuilder writeBuilder;                                     // 创建一个JSON写入器的构建器
    writeBuilder["indentation"] = "";                                           // 设置缩进为空字符串
    std::string requsetBodyStr = Json::writeString(writeBuilder, RequestBody);  //  将JSON对象转换为字符串
    INFO("DeepSeekProvider::sendMessage RequestBody : {}", requsetBodyStr.c_str());

    // 6. 使用httplib构建客户端
    httplib::Client client(_endpoint.c_str());
    client.set_connection_timeout(30, 0);  // param1:second param2:usecond
    client.set_read_timeout(60, 0);        // 设置超时时长60s

    // 7. 设置请求头
    httplib::Headers headers = {
        {"Authorization", "Bearer " + _apiKey},
        {"Content_Type", "application/json"},
    };

    // 8. 发送POST请求
    auto res = client.Post("/chat/completions", headers, requsetBodyStr, "application/json");
    // 检测响应是否成功
    if(!res) {
        ERR("DeepSeekProvider::sendMessage POST request failed");
        return "";
    }
    INFO("DeepSeekProvider::sendMessage POST request successed, status : {}", res->status);
    INFO("DeepSeekProvider::sendMessage POST request successed, body : {}", res->body);
    // 检测状态码
    if(res->status != 200) {
        return "";
    }
    // 9. 解析响应
    Json::Value responseBody;
    Json::CharReaderBuilder readerBuilder;
    std::string parseError;
    std::istringstream responseStream(res->body);
    // 进行反序列化 为json格式
    if(Json::parseFromStream(readerBuilder, responseStream, &responseBody, &parseError)) {
        // 获取choices数组
        if(responseBody.isMember("choices") && responseBody["choices"].isArray() && !responseBody["choices"].empty()) {
            // 获取message数组
            auto choices = responseBody["choices"][0];
            if(choices.isMember("message") && choices["massage"].isMember("content")) {
                // 获取content
                std::string replyContent = choices["message"]["content"].asString();
                INFO("DeepSeekProvider::sendMessage response content : {}", replyContent);
                return replyContent;
            }
        }
    }
    // 10. 解析失败
    ERR("DeepSeekProvider::sendMessage response parse failed");
    return "deepseek response json parse failed";
}

// 发送消息 增量返回 流式响应
std::string DeepSeekProvider::sendMessageStream(const std::vector<Message> messages, const std::map<std::string, std::string> requestParam, std::function<void(std::string&, bool)> callback) {
    return "";
}

}  // namespace Ai_Chat_SDK
