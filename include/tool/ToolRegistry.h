#pragma once

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tool/Tool.h"

namespace aiSDK {

// ToolRegistry 先提供最小可用的内存注册表，
// 后续再补启停、校验和更细粒度的错误信息。
class ToolRegistry {
   public:
    void registerTool(const Tool& tool, ToolHandler handler) {
        tools_[tool.name] = tool;
        handlers_[tool.name] = std::move(handler);
    }

    bool hasTool(const std::string& name) const {
        return tools_.find(name) != tools_.end();
    }

    Tool getTool(const std::string& name) const {
        const auto iterator = tools_.find(name);
        if(iterator == tools_.end()) {
            throw std::out_of_range("tool not found");
        }
        return iterator->second;
    }

    std::vector<Tool> listTools() const {
        std::vector<Tool> result;
        result.reserve(tools_.size());
        for(const auto& entry : tools_) {
            result.push_back(entry.second);
        }
        return result;
    }

    ToolResult execute(const std::string& name, const nlohmann::json& arguments) {
        const auto iterator = handlers_.find(name);
        if(iterator == handlers_.end()) {
            return ToolResult::errorResult("tool not found");
        }
        return iterator->second(arguments);
    }

   private:
    std::unordered_map<std::string, Tool> tools_;
    std::unordered_map<std::string, ToolHandler> handlers_;
};

}  // namespace aiSDK