#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mcp/MCPClient.h"
#include "tool/ToolRegistry.h"

namespace aiSDK {

// Adapter 公共面只描述显式选择、Binding 值与注册操作，不隐藏连接管理。
// 目录刷新、Client Pool、审批策略和并发互斥均由上层 Agent 或应用负责。
// 所有 Handler 都返回 ToolResult，MCP 运行期异常不会越过模型工具边界。
// 本接口不暴露协议 raw 指针或 Transport 类型，Binding 可直接交给现有注册表。
// 生成与注册分为两步，调用方可以在写入模型可见工具前审计完整映射。

// MCPToolSelection 由应用显式选择远端工具、别名和风险覆盖。
// local_name 为空时使用 <server_id>__<remote_name>，风险默认 High。
struct MCPToolSelection {
    std::string remote_name;
    std::optional<std::string> local_name;
    std::optional<ToolRiskLevel> risk_level;
};

// MCPToolAdapterOptions 只控制公开错误文本上限。
// 命名规则和默认高风险是安全合同，不提供关闭选项。
struct MCPToolAdapterOptions {
    std::size_t max_error_text_bytes = 4096U;
};

// MCPToolBinding 保存现有 Tool、同步 Handler 和可审计映射。
// 同批 Handler 共享 Catalog 快照并捕获 weak Client，不会因注册而延长连接生命周期。
struct MCPToolBinding {
    Tool tool;
    ToolHandler handler;
    std::string remote_name;
    std::string server_id;
};

// MCPToolAdapter 没有隐式注册副作用，所有写入由调用方显式触发。
class MCPToolAdapter {
   public:
    // adaptTools 整批校验目录、选择、命名、Schema 和重复项后生成绑定。
    static std::vector<MCPToolBinding> adaptTools(const std::shared_ptr<MCPClient>& client,
                                                  const MCPToolCatalog& catalog,
                                                  const std::vector<MCPToolSelection>& selections,
                                                  const MCPToolAdapterOptions& options = {});
    // registerBindings 在任何写入前重新检查当前注册表冲突。
    static void registerBindings(ToolRegistry& registry, const std::vector<MCPToolBinding>& bindings);
    // unregisterBindings 由上层在目录失效后显式调用，未知名称幂等。
    static void unregisterBindings(ToolRegistry& registry, const std::vector<MCPToolBinding>& bindings);
};

}  // namespace aiSDK
