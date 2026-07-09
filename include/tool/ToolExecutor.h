#pragma once

#include <vector>

#include "core/Message.h"
#include "tool/ToolCall.h"
#include "tool/ToolRegistry.h"

namespace aiSDK {

struct ToolExecutionResult {
    ToolCall call;
    ToolResult result;

    Message toToolMessage() const {
        const std::string content = result.success ? result.data.dump() : result.error_message;
        return ToolMessage(content, call.id);
    }
};

// ToolExecutor 第一阶段保持串行执行，
// 先把链路跑通，再考虑并发和调度。
class ToolExecutor {
   public:
    explicit ToolExecutor(ToolRegistry& registry) : registry_(registry) {}

    std::vector<ToolExecutionResult> executeAll(const std::vector<ToolCall>& calls) {
        std::vector<ToolExecutionResult> results;
        results.reserve(calls.size());
        for(const auto& call : calls) {
            results.push_back(ToolExecutionResult{call, registry_.execute(call.name, call.arguments)});
        }
        return results;
    }

   private:
    ToolRegistry& registry_;
};

}  // namespace aiSDK