#include "mcp/MCPToolAdapter.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "mcp/detail/MCPText.h"

namespace aiSDK {
namespace {

// 本实现是 MCP 目录与现有 ToolRegistry 之间的显式适配边界。
// Adapter 不拥有连接，也不在构造 Binding 时启动任何 I/O。
// Client 必须由应用以 shared_ptr 管理，才能生成不会悬空的 weak 引用。
// Catalog 必须由同一个 Client 签发且处于当前代次，伪造值会在适配前失败。
// 一批选择只创建一份共享 Catalog，避免按工具复制完整 raw JSON 目录。
// Handler 共享目录值但不共享可变状态，因此并发读取不会产生数据竞争。
// Handler 只持有 weak Client，注册生命周期不会隐式延长进程或 HTTP 会话。
// 应用仍负责在目录变化后注销旧 Binding，并用新 Catalog 重新绑定。
// Adapter 不自动刷新目录，避免模型可见工具集合在调用中途静默改变。
// Adapter 不自动选择远端工具，首版要求应用显式提交 selection 清单。
// 远端名称始终原样用于 tools/call，不能替换成本地别名。
// 本地名称只影响 ToolRegistry 和模型可见定义，不回写远端协议。
// 缺省名称带 server_id 前缀，用于降低多个 Server 合并时的冲突概率。
// 本地名称采用可移植 ASCII 子集，避免模型提供方使用不同 Unicode 规则。
// 名称长度上限固定为 64 字节，防止模型上下文和映射表被异常名称放大。
// 同批远端名称不能重复，避免两份 Handler 看似不同却调用同一远端能力。
// 同批本地名称不能重复，避免注册顺序决定最终可见定义。
// 选择不存在的工具会整体失败，不能留下仅部分可用的 Binding 清单。
// 远端 description 与 inputSchema 按目录快照复制到本地 Tool 定义。
// inputSchema 已由协议层保证为对象，注册阶段仍做防御性复核。
// 远端 annotations 不会降低本地风险等级，缺省风险始终为 High。
// 风险覆盖只是元数据，实际审批策略由 Agent 或应用层负责。
// adaptTools 只生成值，不写 ToolRegistry，便于上层先展示或审批映射。
// registerBindings 对整批定义预检后才执行第一次写入。
// 已存在本地名称被视为冲突，不能借用 ToolRegistry 的替换语义覆盖应用工具。
// 冲突检查覆盖整批后再注册，保证可预见错误不会形成部分写入。
// ToolRegistry 自身异常仍可能来自内存分配，调用方不应把它视为业务冲突。
// unregisterBindings 只按本地名称清理，不依赖 Client 或 Catalog 仍然存活。
// 注销未知名称保持幂等，允许 close 与目录失效清理使用同一路径。
// 非对象 arguments 在 Adapter 本地拒绝，不进入 Client 和 Transport。
// 参数类型错误优先于连接存活检查，确保返回准确且可操作的失败原因。
// tools/call 的其他合法性仍由 Client 负责，Adapter 不重复协议状态机。
// Client 过期时返回固定连接不可用文本，不抛出 bad_weak_ptr 或访问悬空对象。
// Client 状态逻辑错误收敛为连接不可用，不泄漏内部状态实现细节。
// MCPException 按闭合 error code 映射，不把远端正文或底层路径拼入结果。
// OutcomeUnknown 明确提醒不得自动重试，并保留非敏感根因类别名称。
// ToolCatalogStale 明确要求重新列举和绑定，不尝试用旧目录继续调用。
// 其他 MCP 错误只暴露稳定机器类别，避免上层解析动态 what 文本。
// 未分类标准异常使用固定文本，不能把 exception.what() 直接交给模型。
// 非标准异常同样收敛，确保 ToolRegistry 的单工具失败不会中断整批执行。
// 业务失败优先提取全部文本块，并以换行保持块之间的可读边界。
// 没有文本的业务失败使用固定兜底，不返回空错误消息。
// 富媒体结果在当前 ToolResult 结构下无法无损表达，因此明确失败。
// 富媒体失败不修改 MCPToolCallResult，直接使用 Client API 的调用方仍可读原始块。
// 正常结果返回完整 raw_result，保留 structuredContent、_meta 与未知扩展。
// Adapter 不重新拼装成功 JSON，避免协议新增字段在适配时丢失。
// 所有公开失败文本都经过控制字符净化和 UTF-8 字节上限处理。
// 错误文本上限为零在创建 Binding 前失败，避免运行时出现无意义空结果。
// 一个或两个字节无法容纳中文码点时使用单字节标点作为非空失败标记。
// 极小上限的标点仅代表“存在失败”，结构化流程仍依据 ToolResult.success。
// 正常上限下始终返回具体中文原因，调用方无需依赖极小上限兜底。
// 文本截断只影响模型可见诊断，不改变 MCPException 的结构化错误语义。
// collectTextContent 只消费协议层已校验的数组，不执行 URI、资源或媒体内容。
// Adapter 不下载图标、不打开资源链接，也不解释 Server 提供的执行提示。
// 目录查找表只保存对调用期 Catalog 的引用，构造共享副本前不会悬空。
// 共享 Catalog 在所有 Handler 释放后自动销毁，无需单独的清理注册表。
// 每个 Handler 只额外保存远端名称、错误上限、weak Client 与 shared_ptr。
// 目录较大时内存复杂度因此从每工具一份目录降低为整批一份目录。
// 选择预检使用哈希集合，平均时间复杂度与目录和选择数量线性相关。
// Adapter 不持有互斥锁；Client 和 ToolRegistry 各自遵循其公开并发合同。
// registerBindings 与 unregisterBindings 的并发互斥仍由 ToolRegistry 调用方负责。
// 所有固定诊断均为简体中文，协议字段和机器错误名保留既有标识符形式。
// 空 selections 会返回空 Binding 批次，不创建无用 Handler 或目录副作用。
// 适配返回值由调用方持有，Adapter 本身没有全局缓存或隐藏单例状态。

bool isPortableToolName(const std::string& name) {
    // 首版名称合同固定为 ASCII，避免不同模型后端对 Unicode 标识符做不同规范化。
    if(name.empty() || name.size() > 64U) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](unsigned char character) {
        return (character >= static_cast<unsigned char>('A') && character <= static_cast<unsigned char>('Z')) ||
               (character >= static_cast<unsigned char>('a') && character <= static_cast<unsigned char>('z')) ||
               (character >= static_cast<unsigned char>('0') && character <= static_cast<unsigned char>('9')) ||
               character == '-' || character == '_';
    });
}

std::string collectTextContent(const nlohmann::json& content) {
    std::string text;
    for(const auto& block : content) {
        if(block.value("type", std::string{}) != "text") {
            continue;
        }
        if(!text.empty()) {
            text.push_back('\n');
        }
        text += block.value("text", std::string{});
    }
    return text;
}

bool containsUnsupportedRichContent(const nlohmann::json& content) {
    // Client 无损保留所有合法内容块；现有 ToolResult 只安全承接文本和结构化对象。
    return std::any_of(content.begin(), content.end(),
                       [](const nlohmann::json& block) { return block.value("type", std::string{}) != "text"; });
}

std::string boundedAdapterError(std::string_view message, std::size_t max_error_text_bytes);

ToolResult adaptCallResult(const MCPToolCallResult& result, std::size_t max_error_text_bytes) {
    if(containsUnsupportedRichContent(result.content)) {
        return ToolResult::errorResult(boundedAdapterError("暂不支持 MCP 富媒体工具结果", max_error_text_bytes));
    }
    if(result.is_error) {
        std::string error_text = collectTextContent(result.content);
        if(error_text.empty()) {
            error_text = "MCP 工具返回业务失败";
        }
        return ToolResult::errorResult(boundedAdapterError(error_text, max_error_text_bytes));
    }

    // raw_result 是已经校验且保真的标准信封，包含 content、structuredContent 与未知扩展。
    return ToolResult::successResult(result.raw_result);
}

std::string boundedAdapterError(std::string_view message, std::size_t max_error_text_bytes) {
    std::string bounded = detail::sanitizeMCPErrorText(message, max_error_text_bytes);
    // 一到两个字节放不下任何中文 UTF-8 码点；用单个标点保持失败结果非空且不突破调用方上限。
    if(bounded.empty()) {
        bounded = "!";
    }
    return bounded;
}

ToolResult executeBinding(const std::weak_ptr<MCPClient>& weak_client, const MCPToolCatalog& catalog,
                          const std::string& remote_name, std::size_t max_error_text_bytes,
                          const nlohmann::json& arguments) {
    // ToolRegistry 允许任意 JSON 参数，但 MCP tools/call 首版合同只接受对象；在连接状态检查前返回准确输入错误。
    if(!arguments.is_object()) {
        return ToolResult::errorResult(boundedAdapterError("MCP 工具参数必须是 JSON 对象", max_error_text_bytes));
    }
    const auto client = weak_client.lock();
    if(!client) {
        return ToolResult::errorResult(boundedAdapterError("该 MCP 连接不可用", max_error_text_bytes));
    }
    try {
        return adaptCallResult(client->callTool(catalog, remote_name, arguments), max_error_text_bytes);
    } catch(const MCPException& exception) {
        if(exception.code() == MCPErrorCode::OutcomeUnknown) {
            std::string message = "MCP 工具结果未知，工具可能已执行，请勿自动重试";
            if(exception.causeCode().has_value()) {
                message += "；根因类别：";
                message += mcpErrorCodeName(*exception.causeCode());
            }
            return ToolResult::errorResult(boundedAdapterError(message, max_error_text_bytes));
        }
        if(exception.code() == MCPErrorCode::ToolCatalogStale) {
            return ToolResult::errorResult(
                boundedAdapterError("MCP 工具目录已失效，请重新列举并绑定", max_error_text_bytes));
        }
        std::string message = "MCP 工具调用失败；错误类别：";
        message += mcpErrorCodeName(exception.code());
        return ToolResult::errorResult(boundedAdapterError(message, max_error_text_bytes));
    } catch(const std::logic_error&) {
        return ToolResult::errorResult(boundedAdapterError("该 MCP 连接不可用", max_error_text_bytes));
    } catch(const std::exception&) {
        // 不把未知异常 what() 送给模型，避免 Provider、路径或底层网络细节泄漏。
        return ToolResult::errorResult(boundedAdapterError("MCP 工具调用发生未分类错误", max_error_text_bytes));
    } catch(...) {
        return ToolResult::errorResult(boundedAdapterError("MCP 工具调用发生未知错误", max_error_text_bytes));
    }
}

}  // namespace

std::vector<MCPToolBinding> MCPToolAdapter::adaptTools(const std::shared_ptr<MCPClient>& client,
                                                       const MCPToolCatalog& catalog,
                                                       const std::vector<MCPToolSelection>& selections,
                                                       const MCPToolAdapterOptions& options) {
    if(!client) {
        throw std::invalid_argument("MCPToolAdapter 的 Client 不能为空");
    }
    if(options.max_error_text_bytes == 0U) {
        throw std::invalid_argument("MCPToolAdapter 错误文本上限必须大于零");
    }
    if(!client->ownsCatalog(catalog)) {
        throw std::invalid_argument("MCPToolAdapter 只能绑定当前 Client 的有效 Catalog");
    }

    std::unordered_map<std::string, const MCPTool*> tools_by_name;
    tools_by_name.reserve(catalog.tools().size());
    for(const auto& tool : catalog.tools()) {
        tools_by_name.emplace(tool.name, &tool);
    }

    // 整批预检先完成所有可预见失败，之后构造结果不会产生部分注册副作用。
    std::unordered_set<std::string> selected_remote_names;
    std::unordered_set<std::string> local_names;
    std::vector<MCPToolBinding> bindings;
    bindings.reserve(selections.size());
    // 整批 Handler 共享唯一目录快照；每个 Handler 只复制 shared_ptr，不再深拷贝完整 tools/raw JSON。
    const auto shared_catalog = std::make_shared<const MCPToolCatalog>(catalog);
    for(const auto& selection : selections) {
        if(selection.remote_name.empty() || !selected_remote_names.insert(selection.remote_name).second) {
            throw std::invalid_argument("MCP 工具选择包含空名称或重复远端名称");
        }
        const auto tool_iterator = tools_by_name.find(selection.remote_name);
        if(tool_iterator == tools_by_name.end()) {
            // 远端名称来自不可信 Server；公开配置异常不能回显原文或控制字符。
            throw std::invalid_argument("选择的 MCP 工具不在当前 Catalog 中");
        }

        const std::string local_name = selection.local_name.value_or(catalog.serverId() + "__" + selection.remote_name);
        if(!isPortableToolName(local_name)) {
            throw std::invalid_argument("MCP 本地工具名称必须满足 [A-Za-z0-9_-]{1,64}");
        }
        if(!local_names.insert(local_name).second) {
            throw std::invalid_argument("MCP 工具选择产生重复本地名称");
        }

        const MCPTool& remote_tool = *tool_iterator->second;
        Tool local_tool;
        local_tool.name = local_name;
        local_tool.description = remote_tool.description;
        local_tool.parameters = remote_tool.input_schema;
        local_tool.risk_level = selection.risk_level.value_or(ToolRiskLevel::High);

        // Handler 捕获 weak Client 与共享 Catalog；注册表不会延长 Client 或子进程寿命。
        const std::weak_ptr<MCPClient> weak_client = client;
        const std::string remote_name = selection.remote_name;
        ToolHandler handler = [weak_client, shared_catalog, remote_name,
                               max_error_text_bytes = options.max_error_text_bytes](const nlohmann::json& arguments) {
            return executeBinding(weak_client, *shared_catalog, remote_name, max_error_text_bytes, arguments);
        };
        bindings.push_back({std::move(local_tool), std::move(handler), selection.remote_name, catalog.serverId()});
    }
    return bindings;
}

void MCPToolAdapter::registerBindings(ToolRegistry& registry, const std::vector<MCPToolBinding>& bindings) {
    std::unordered_set<std::string> names;
    names.reserve(bindings.size());
    for(const auto& binding : bindings) {
        if(binding.tool.name.empty() || !binding.handler || !binding.tool.parameters.is_object()) {
            throw std::invalid_argument("MCP Binding 包含不可注册的工具定义");
        }
        if(!names.insert(binding.tool.name).second) {
            throw std::invalid_argument("MCP Binding 包含重复本地名称");
        }
        if(registry.hasTool(binding.tool.name)) {
            throw std::invalid_argument("MCP Binding 与现有工具名称冲突");
        }
    }

    // 所有业务校验完成后才写入注册表；正常情况下不会形成部分注册。
    for(const auto& binding : bindings) {
        registry.registerTool(binding.tool, binding.handler);
    }
}

void MCPToolAdapter::unregisterBindings(ToolRegistry& registry, const std::vector<MCPToolBinding>& bindings) {
    std::vector<std::string> names;
    names.reserve(bindings.size());
    for(const auto& binding : bindings) {
        names.push_back(binding.tool.name);
    }
    // ToolRegistry 负责整批空名称、重复名称预检和未知名称幂等语义。
    registry.unregisterTools(names);
}

}  // namespace aiSDK
