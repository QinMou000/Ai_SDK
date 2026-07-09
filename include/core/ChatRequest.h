#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/Message.h"
#include "tool/Tool.h"

namespace aiSDK {

// ChatRequest 聚合一次模型调用所需的最小上下文。
// 第一阶段先保留最常用的 messages、tools 和采样参数。
struct ChatRequest {
    // model 保存本次请求希望命中的具体模型名。
    std::string model;
    // messages 是模型上下文，顺序直接影响模型行为。
    std::vector<Message> messages;
    // tools 是本轮允许模型调用的工具集合。
    std::vector<Tool> tools;
    // stream 控制是否走流式响应协议。
    bool stream = false;
    // temperature 保留常见采样参数，便于后续直接透传到 Provider。
    double temperature = 0.7;
    // max_tokens 限制模型输出长度，避免调用方在每处重复定义。
    int max_tokens = 2048;
};

// chatRequestToJson 生成统一的请求 JSON，
// 后续 DeepSeek 和 MiniMax Provider 都可以在此基础上适配。
nlohmann::json chatRequestToJson(const ChatRequest& request);

}  // namespace aiSDK
