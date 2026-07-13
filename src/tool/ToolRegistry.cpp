#include "tool/ToolRegistry.h"

#include <exception>
#include <stdexcept>
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
