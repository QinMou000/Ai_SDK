#include "../include/ChatGPTProvider.h"

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
// 初始化模型基本参数
bool ChatGPTProvider::initModel(const std::map<std::string, std::string>& ModelConfig) {
    // 1. 初始化 api_key
    auto it = ModelConfig.find("api_key");
    if(it == ModelConfig.end()) {
        ERR("ChatGPTProvider api_key not found");
        return false;
    }
    _apiKey = it->second;
    // 2. 初始化 base url
    it = ModelConfig.find("end_point");
    if(it == ModelConfig.end()) {
        ERR("ChatGPTProvider end_point not found");
        return false;
    }
    _endpoint = it->second;

    _isAvailable = true;
    INFO("ChatGPTProvider init successed api_key : {} base_url : {}", _apiKey, _endpoint);
    return true;
}

// 检测模型是否可用
bool ChatGPTProvider::isAvailable() const { return _isAvailable; }

// 获取模型名称
std::string ChatGPTProvider::getModelName() const { return "gpt-4o"; }

// 获取模型信息
std::string ChatGPTProvider::getModelDesc() const {
    return "ChatGPT, 一个由 OpenAI 训练的大语言模型，可以帮你解答问题、提供建议、写作和解决各种学习与技术难题";
}
// 基本请求 curl
// curl https://api.openai.com/v1/chat/completions \
//   -H "Content-Type: application/json" \
//   -H "Authorization: Bearer YOUR_API_KEY" \
//   -d '{
//     "model": "gpt-5.3",
//     "messages": [
//       {
//         "role": "user",
//         "content": "你好，介绍一下你自己"
//       }
//     ],
//     "temperature": 0.7
//   }'
// 返回格式
// {
//   "id": "chatcmpl-abc123",
//   "object": "chat.completion",
//   "created": 1710000000,
//   "model": "gpt-5.3",
//   "choices": [
//     {
//       "index": 0,
//       "message": {
//         "role": "assistant",
//         "content": "我是ChatGPT，一个由OpenAI训练的大语言模型..."
//       },
//       "finish_reason": "stop"
//     }
//   ],
//   "usage": {
//     "prompt_tokens": 20,
//     "completion_tokens": 30,
//     "total_tokens": 50
//   }
// }
// 发送消息 全量返回
std::string ChatGPTProvider::sendMessage(const std::vector<Message>& messages, const std::map<std::string, std::string>& requestParam) {
    // 1. 判断模型是否可用
    if(!isAvailable()) {
        ERR("ChatGPTProvider::sendMessage model not available");
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
    INFO("ChatGPTProvider::sendMessage RequestBody : {}", requsetBodyStr.c_str());

    // 6. 使用httplib构建客户端
    httplib::Client client(_endpoint.c_str());
    client.set_connection_timeout(30, 0);  // param1:second param2:usecond
    client.set_read_timeout(60, 0);        // 设置超时时长60s

    // 7. 设置请求头
    httplib::Headers headers = {
        {"Authorization", "Bearer " + _apiKey},
        {"Content-Type", "application/json"},
    };

    // 8. 发送POST请求
    auto res = client.Post("/v1/chat/completions", headers, requsetBodyStr, "application/json");
    // 检测响应是否成功
    if(!res) {
        ERR("ChatGPTProvider::sendMessage POST request failed");
        return "";
    }
    INFO("ChatGPTProvider::sendMessage POST request successed, status : {}", res->status);
    INFO("ChatGPTProvider::sendMessage POST request successed, body : {}", res->body);
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
            if(choices.isMember("message") && choices["message"].isMember("content")) {
                // 获取content
                std::string replyContent = choices["message"]["content"].asString();
                INFO("ChatGPTProvider::sendMessage response content : {}", replyContent);
                return replyContent;
            }
        }
    }
    // 10. 解析失败
    ERR("ChatGPTProvider::sendMessage response parse failed");
    return "ChatGPT response json parse failed";
}
// 发送消息 增量返回 流式响应
// callback 处理增量数据
// // param1 : 增量数据
// // param2 : 是否为最后一个增量数据
std::string ChatGPTProvider::sendMessageStream(const std::vector<Message>& messages, const std::map<std::string, std::string>& requestParam,
                                               std::function<void(const std::string&, bool)> callback) {
    // 1. 检测模型是否可用
    if(!_isAvailable) {
        ERR("ChatGPTProvider::sendMessageStream model not available");
        return "";
    }
    // 2. 构建请求参数
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
    RequestBody["stream"] = true;  // 开启流式响应
    // 5. 序列化
    Json::StreamWriterBuilder writeBuilder;                                     // 创建一个JSON写入器的构建器
    writeBuilder["indentation"] = "";                                           // 设置缩进为空字符串
    std::string requsetBodyStr = Json::writeString(writeBuilder, RequestBody);  //  将JSON对象转换为字符串
    INFO("ChatGPTProvider::sendMessageStream RequestBody : {}", requsetBodyStr.c_str());
    // 6. 使用httplib构建客户端
    httplib::Client client(_endpoint.c_str());
    client.set_connection_timeout(30, 0);  // param1:second param2:usecond
    client.set_read_timeout(300, 0);       // 设置超时时长60s // 流式响应需要更长时间
    // 7. 设置请求头
    httplib::Headers headers = {{"Authorization", "Bearer " + _apiKey}, {"Content-Type", "application/json"}, {"Accept", "text/event-stream"}};
    // 8. 定义流式处理变量
    std::string buffer;         // 接收流式数据的buffer
    bool gotError = false;      // 标记响应是否成功
    std::string errorMsg;       // 错误描述符
    int status_code;            // 响应状态码
    bool streamFinish = false;  // 标志流式响应是否结束
    std::string fullResponse;   // 累计完整响应
    // 9. 创建请求对象 并初始化
    httplib::Request req;
    req.method = "POST";
    req.path = "/v1/chat/completions";
    req.headers = headers;
    req.body = requsetBodyStr;
    // 10. 设置响应处理器
    // std::function<bool(const Response &response)>
    req.response_handler = [&](const httplib::Response& response) {
        if(response.status != 200) {
            gotError = true;
            errorMsg = "HTTP status code: " + std::to_string(response.status);
            ERR("ChatGPTProvider::sendMessageStream::response_handler : {}", errorMsg);
            return false;  // 终止请求
        }
        return true;  // 继续请求
    };
    // 11. 设置数据接收处理器 解释流式相应的每一个数据块的数据
    // std::function<bool(const char *data, size_t data_length, size_t offset, size_t total_length)>
    req.content_receiver = [&](const char* data, size_t data_length, size_t offset, size_t total_length) {
        // 验证响应头是否出错
        if(gotError) {
            return false;
        }
        // 追加数据到buffer
        buffer.append(data, data_length);
        INFO("ChatGPTProvider::sendMessageStream::content_receiver buffer : {}", buffer);
        // 处理所有流式数据块 数据块之间以\n\n进行分割
        size_t pos = 0;
        while((pos = buffer.find("\n\n")) != std::string::npos) {
            // 找到了一块可用数据
            // 截取第一块数据
            std::string chunk = buffer.substr(0, pos);
            buffer.erase(0, pos + 2);
            // 处理空行 || ':'开头的是注释行
            if(chunk.empty() || chunk[0] == ':') continue;
            // 判断chunk前六个字符是否为"data: "
            if(chunk.compare(0, 6, "data: ") == 0) {
                std::string modelData = chunk.substr(6);  // 从第六个字符一直截取到末尾

                // 检测是否为结束标记 [DONE]
                if(modelData == "[DONE]") {
                    streamFinish = true;
                    callback("", true);
                    return true;
                }
                // 将得到的流式数据反序列化为json
                Json::Value modelDataJson;
                Json::CharReaderBuilder reader;
                std::string errors;  // 如果解析错误 保存错误信息
                std::istringstream modelDataStream(modelData);
                if(Json::parseFromStream(reader, modelDataStream, &modelDataJson, &errors)) {
                    // 拿到模型返回的json格式的数据 modelDataJson
                    if(modelDataJson.isMember("choices") && modelDataJson["choices"].isArray() && !modelDataJson["choices"].empty() &&
                       modelDataJson["choices"][0].isMember("delta") && modelDataJson["choices"][0]["delta"].isMember("content")) {
                        // 拿到最后的content 追加到fullResponse
                        std::string content = modelDataJson["choices"][0]["delta"]["content"].asString();
                        fullResponse += content;
                        // 将本次模型返回的数据返回给用户使用 callback
                        callback(content, false);
                    } else {
                        // 解析失败
                        WARN("ChatGPTProvider::sendMessageStream::parseFromStream parse failed : {}", errors);
                        // continue;
                    }
                }
            }
        }
        return true;
    };
    httplib::Result result = client.send(req);
    if(!result) {
        // 请求发送失败
        ERR("ChatGPTProvider::sendMessageStream client.send failed : {}", httplib::to_string(result.error()));
        return "";
    }
    if(!streamFinish) {
        WARN("stream ended without [DONE] marker");
        callback("", true);
    }

    return fullResponse;
}

}  // namespace Ai_Chat_SDK