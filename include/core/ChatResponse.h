#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/Message.h"
#include "tool/ToolCall.h"

namespace aiSDK {

struct Usage {
    // prompt_tokens / completion_tokens / total_tokens 保持主流模型接口惯例，
    // 便于后续直接映射不同 Provider 的统计字段。
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
};

// ChatResponse 统一承载文本结果、工具调用和原始响应。
// 后续不同 Provider 只需要填充这一个结构。
struct ChatResponse {
    // message 保存统一的 assistant 消息对象。
    Message message = AssistantMessage("");
    // content 是对最常见“文本答复”场景的快捷访问。
    std::string content;
    // tool_calls 承载模型要求执行的工具调用。
    std::vector<ToolCall> tool_calls;
    // usage 用于上层做成本统计或调试分析。
    Usage usage;
    // raw_response 保留原始响应文本，便于排查协议适配问题。
    std::string raw_response;

    // hasToolCalls 用于让调用方快速判断是否进入工具执行分支。
    bool hasToolCalls() const {
        return !tool_calls.empty();
    }
};

// usageToJson 和 chatResponseToJson 为测试、日志与调试提供统一导出格式。
nlohmann::json usageToJson(const Usage& usage);
nlohmann::json chatResponseToJson(const ChatResponse& response);

// assistantTextResponse 是最常用的响应构造器，
// 适合普通文本回复或当前阶段的占位实现。
ChatResponse assistantTextResponse(std::string content);

}  // namespace aiSDK
