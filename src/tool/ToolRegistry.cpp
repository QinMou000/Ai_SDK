#include "tool/ToolRegistry.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace aiSDK {

// 注册是工具进入 SDK 的唯一写入边界。
// 所有影响模型请求合法性或本地可执行性的静态条件都应在这里检查；
// 运行期业务参数仍由具体处理函数判断，避免注册表猜测每个工具的领域语义。
void ToolRegistry::registerTool(const Tool& tool, ToolHandler handler) {
    // 工具名会直接进入模型协议，也是定义表和处理函数表的共同键。
    // 空名称无法被模型稳定引用，也无法提供可诊断的执行错误。
    if(tool.name.empty()) {
        throw std::invalid_argument("工具名称不能为空");
    }
    // 没有处理函数的工具只能被模型看见却无法执行，
    // 因此必须在注册阶段拒绝，而不是把失败推迟到模型已经选中工具以后。
    if(!handler) {
        throw std::invalid_argument("工具处理函数不能为空: " + tool.name);
    }
    // DeepSeek 接收的 parameters 是 JSON Schema 对象。
    // 这里只校验顶层类型，完整 JSON Schema 语义校验仍由后续独立能力负责。
    if(!tool.parameters.is_object()) {
        throw std::invalid_argument("工具参数 Schema 必须是 JSON 对象: " + tool.name);
    }

    // 先记录是否首次注册，避免赋值后 hasTool 永远为 true。
    const bool is_new_tool = !hasTool(tool.name);
    // 定义与处理函数使用相同名称同步替换，避免出现新 Schema 配旧实现。
    tools_[tool.name] = tool;
    handlers_[tool.name] = std::move(handler);

    // 重复注册只替换定义和实现，保持列表顺序不抖动。
    // 这能让请求 JSON、调试输出和离线测试保持可重复。
    if(is_new_tool) {
        tool_order_.push_back(tool.name);
    }
}

void ToolRegistry::unregisterTool(const std::string& name) {
    // 单项入口复用批量入口的校验和清理规则，避免两套注销语义逐步漂移。
    // 临时批次在任何注册表修改前构造完成，分配失败不会留下部分状态。
    unregisterTools(std::vector<std::string>{name});
}

void ToolRegistry::unregisterTools(const std::vector<std::string>& names) {
    // 空批次直接返回，既避免无意义扫描顺序表，也保证幂等清理没有额外分配。
    if(names.empty()) {
        return;
    }

    // 唯一名称集合同时承担重复检查和后续顺序表过滤。
    // 所有可能抛出的业务校验都必须发生在修改三个内部容器之前。
    std::unordered_set<std::string> unique_names;
    unique_names.reserve(names.size());

    for(const auto& name : names) {
        // 空名称无法对应稳定的工具键，应作为调用方输入错误整体拒绝。
        if(name.empty()) {
            throw std::invalid_argument("注销工具名称不能为空");
        }
        // 重复名称通常意味着上层绑定清单存在错误，静默合并会掩盖该问题。
        if(!unique_names.insert(name).second) {
            throw std::invalid_argument("批量注销包含重复工具名称: " + name);
        }
    }

    // 校验成功后再同步清除定义与处理函数；erase 对未知键天然保持幂等。
    // 两张表使用同一名称集合，保证工具可见性与可执行性不会主动分叉。
    for(const auto& name : unique_names) {
        tools_.erase(name);
        handlers_.erase(name);
    }

    // 顺序表移除全部目标名称，但保留其他工具之间的相对顺序。
    // 旧位置被真正删除后，同名工具再次注册会自然追加到列表末尾。
    //  std::remove_if 它不会真正删除容器元素，而是把“不满足删除条件”的元素搬到前面，并返回新的逻辑末尾。
    tool_order_.erase(
        std::remove_if(tool_order_.begin(), tool_order_.end(),
                       [&](const std::string& name) { return unique_names.find(name) != unique_names.end(); }),
        tool_order_.end());
}

bool ToolRegistry::hasTool(const std::string& name) const {
    // 定义表是工具存在性的唯一来源；处理函数表不对外形成第二套状态。
    return tools_.find(name) != tools_.end();
}

Tool ToolRegistry::getTool(const std::string& name) const {
    // 返回副本可以避免调用方绕过 registerTool 直接修改内部定义。
    const auto iterator = tools_.find(name);
    if(iterator == tools_.end()) {
        // 查询 API 用异常表达调用方指定了不存在的名称。
        throw std::out_of_range("工具不存在: " + name);
    }
    return iterator->second;
}

std::vector<Tool> ToolRegistry::listTools() const {
    // 输出容量等于首次注册过的名称数，正常状态下与当前定义数一致。
    std::vector<Tool> result;
    result.reserve(tool_order_.size());

    // 只依据首次注册顺序输出，避免 unordered_map 迭代顺序泄漏到公开 API。
    // 防御性查找允许未来加入删除能力后仍安全跳过已经失效的名称。
    for(const auto& name : tool_order_) {
        const auto iterator = tools_.find(name);
        if(iterator != tools_.end()) {
            result.push_back(iterator->second);
        }
    }
    // 返回值可直接赋给 ChatRequest::tools，不暴露内部容器引用。
    return result;
}

ToolResult ToolRegistry::execute(const std::string& name, const nlohmann::json& arguments) {
    // 执行路径查处理函数表；模型返回未知名称时不应让整个批次抛异常。
    const auto iterator = handlers_.find(name);
    if(iterator == handlers_.end()) {
        return ToolResult::errorResult("工具不存在: " + name);
    }

    try {
        // 参数保持模型解析后的 JSON 结构，不在注册表层做业务字段猜测。
        return iterator->second(arguments);
    } catch(const std::exception& exception) {
        // 工具是外部扩展边界，异常需要转成稳定结果，避免打断同批其他调用。
        // 标准异常保留 what()，同时补充工具名以便从批量执行中定位来源。
        return ToolResult::errorResult("工具执行失败: " + name + " | " + exception.what());
    } catch(...) {
        // 非标准异常无法提取细节，但仍需保留工具名方便定位来源。
        // 这里不重新抛出，因为 SDK 对批量工具执行承诺单个失败不会中断后续调用。
        return ToolResult::errorResult("工具执行失败: " + name + " | 未知异常");
    }
}

}  // namespace aiSDK
