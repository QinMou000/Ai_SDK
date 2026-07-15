#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "tool/Tool.h"

namespace aiSDK {

// ToolRegistry 管理模型可见的工具定义和对应的本地处理函数。
// 注册顺序会被保留，确保请求构造、示例输出和测试结果稳定。
// 当前实现不提供内部同步；并发注册或执行时，调用方必须负责互斥。
class ToolRegistry {
   public:
    // registerTool 校验模型协议所需的名称、处理函数和参数 Schema。
    // 同名注册采用替换语义，便于应用更新实现，但不会改变首次展示顺序。
    // 参数非法时抛出 std::invalid_argument，避免留下不可执行的半成品。
    void registerTool(const Tool& tool, ToolHandler handler);

    // unregisterTool 从定义、处理函数和展示顺序中同步移除指定工具。
    // 未知名称按幂等清理处理；空名称属于调用方错误并抛出 std::invalid_argument。
    void unregisterTool(const std::string& name);
    // unregisterTools 在修改状态前校验整批名称，避免非法输入造成部分注销。
    // 空批次不执行任何操作；空名称或重复名称抛出 std::invalid_argument，
    // 未知名称不报错。注销后的同名工具再次注册时会追加到新的展示位置。
    void unregisterTools(const std::vector<std::string>& names);

    // hasTool 只查询定义是否存在，不执行处理函数，也不会抛出未知工具异常。
    bool hasTool(const std::string& name) const;
    // getTool 返回一份工具定义副本；名称未知时抛出 std::out_of_range。
    Tool getTool(const std::string& name) const;
    // listTools 按首次注册顺序返回定义，结果可直接赋给 ChatRequest::tools。
    std::vector<Tool> listTools() const;

    // execute 同步调用指定处理函数，并将参数原样传入。
    // 未知工具和处理函数异常都会收敛为失败 ToolResult，
    // 让同一批 Tool Call 中的其他调用仍可继续执行。
    ToolResult execute(const std::string& name, const nlohmann::json& arguments);

   private:
    // tool_order_ 只保存首次注册顺序，不复制完整工具对象。
    std::vector<std::string> tool_order_;
    // tools_ 保存发送给模型的最新定义；同名注册会原位替换。
    std::unordered_map<std::string, Tool> tools_;
    // handlers_ 与 tools_ 使用同一名称键，保证定义和本地实现可对应。
    std::unordered_map<std::string, ToolHandler> handlers_;
};

}  // namespace aiSDK
