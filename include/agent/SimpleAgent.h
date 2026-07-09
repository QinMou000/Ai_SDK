#pragma once

#include <string>
#include <vector>

#include "AIClient.h"
#include "core/Message.h"
#include "tool/ToolExecutor.h"

namespace aiSDK {

struct AgentResult {
    bool success = false;
    std::string final_answer;
};

// SimpleAgent 目前只预留最小接口，
// 真正的多轮调度逻辑放到后续阶段实现。
class SimpleAgent {
   public:
    SimpleAgent(AIClient& client, ToolExecutor& tool_executor);

    AgentResult run(const std::string& input);

   private:
    AIClient& client_;
    ToolExecutor& tool_executor_;
    std::vector<Message> messages_;
};

}  // namespace aiSDK