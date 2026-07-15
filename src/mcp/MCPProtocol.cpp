#include "mcp/detail/MCPProtocol.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace aiSDK::detail {
namespace {

// 本文件是 JSON-RPC 2.0 与 MCP Tools 的纯值构造和解析边界。
// 协议层不执行 I/O、不读取配置、不管理线程，也不改变 Client 生命周期。
// 输入 JSON 一律视为不可信 Server 数据，任何访问前都先验证所需类型。
// 解析失败使用闭合 MCPException，不把 nlohmann 细节直接暴露给调用方。
// 协议异常保存调用时的 Client 状态快照，便于区分握手和运行期失败。
// 公开诊断采用固定中文文本，不拼接远端 message、方法参数或扩展字段。
// 所有构造函数返回独立 JSON 值，Transport 再负责序列化与消息 framing。
// 所有解析结果返回独立值，不能引用输入缓冲或传输线程的临时对象。
// JSON-RPC 版本固定为字符串 2.0，数字 2.0 与其他版本均不接受。
// 顶层消息必须是对象，数组和标量不能通过宽松转换进入分类器。
// JSON-RPC ID 首版只允许字符串、signed integer 或 unsigned integer。
// 浮点 ID 即使数值为整数也拒绝，避免序列化格式影响请求关联。
// 布尔值在 JSON 中不是整数 ID，不能依赖 C++ 隐式数值转换。
// null 只适合某些 JSON-RPC 兼容场景，首版为稳定关联明确拒绝。
// 字符串 ID 原样保存，Server 请求响应不能做大小写或 Unicode 规范化。
// Client 主动请求使用 uint64_t，因此响应关联允许完整无符号范围。
// nlohmann 的 unsigned 也满足通用整数查询，分支必须先判 unsigned。
// 大于 INT64_MAX 的合法响应 ID 不能通过 signed get 读取或截断。
// signed 响应 ID 允许非负值与本地 uint64 序列匹配，负值一律拒绝。
// 响应 ID 不匹配当前请求时按协议破坏处理，不能投递给错误等待者。
// 响应必须且只能包含 result 或 error，二者并存和二者皆缺都非法。
// JSON-RPC error 必须包含整数 code 与字符串 message 才算合法错误对象。
// 远端 error.message 可能含秘密，只映射为 RemoteProtocolError 固定文本。
// error.data 属于不可信扩展，首版不执行、不回显也不进入控制分支。
// initialize 请求声明唯一支持的协议版本 2025-11-25。
// initialize Client capabilities 保持空对象，不虚假声明 Roots 或 Sampling。
// initialize clientInfo 只提供固定 SDK 名称与版本，不读取主机身份信息。
// initialize 响应必须返回对象 result，其他 JSON 类型均为协议破坏。
// protocolVersion 必须是字符串且精确匹配唯一受支持版本。
// 合法但不支持的版本使用 VersionMismatch，区别于字段结构错误。
// capabilities 必须是对象，避免后续能力读取依赖隐式空值。
// 首版产品范围要求 Tools 能力存在且为对象，否则使用 CapabilityMissing。
// Resources、Prompts 等额外能力会被忽略，不能因此推断 Client 已支持。
// tools.listChanged 缺失时默认为 false，只有显式布尔 true 才开启通知语义。
// listChanged 使用非布尔值时拒绝，不能把字符串或数字宽松转为真假。
// 初始化解析结果只保存状态机所需字段，不传播不可信 serverInfo。
// initialized 是通知，因此不带 ID，也不等待 JSON-RPC response。
// initialized params 固定为空对象，为未来字段扩展保持稳定形状。
// ping 请求带独立 ID 与空 params，用于验证当前逻辑会话往返。
// ping 成功 result 必须是对象，数组或 null 不冒充控制响应。
// tools/list 请求首页省略 cursor，避免空字符串产生不明确分页语义。
// 后续 cursor 视为不透明 Server 值，构造层不编码、不裁剪也不解析。
// 空 cursor 被构造层视为首页缺省，解析层则归一化为没有下一页。
// tools/list response 的 result 必须是对象，tools 必须存在且为数组。
// 单页工具数量必须在 reserve、解析和 raw JSON 复制之前检查。
// remaining_tool_limit 由 Client 根据已收集工具和总量上限计算。
// 页面超过剩余额度使用 PaginationLimitExceeded，而不是普通结构错误。
// 空页在剩余额度为零时仍可完成分页，数量比较使用严格大于。
// 协议层不负责跨页重名，因为只有 Client 持有完整分页上下文。
// 协议层不负责重复 cursor，因为只有 Client 能观察先前页面历史。
// 工具数组中的每个元素必须是对象，标量不能生成部分 MCPTool。
// name 是必需非空字符串，协议层不改写远端名称。
// inputSchema 是必需对象，Adapter 才能安全映射到本地 Tool.parameters。
// title 与 description 可缺失，但出现时必须是字符串。
// outputSchema 可缺失，但出现时必须是对象，不接受布尔 Schema 简写。
// annotations 可缺失，但出现时必须是对象，只作为不可信元数据保存。
// execution 可缺失，但出现时必须是对象，不执行其中的 Server 提示。
// icons 可缺失，但出现时必须是数组，协议层不下载或解析图标资源。
// 常用字段复制到稳定成员，便于 Client 和 Adapter 直接访问。
// 每个工具的完整原始对象还复制到 raw，保证未知扩展字段不丢失。
// raw 与稳定字段并存时，稳定字段只解释首版合同，不覆盖未知语义。
// nextCursor 可缺失；出现时必须是字符串，其他 JSON 类型拒绝。
// 非空 nextCursor 原样保留，空字符串归一化为 std::nullopt。
// tools/call 构造要求远端名称非空，空名称不会进入传输队列。
// tools/call arguments 必须是对象，数组和标量在本地构造阶段拒绝。
// 远端名称和 arguments 原样放入 params，不做 Schema 业务校验。
// inputSchema 的完整字段级校验属于 Server，SDK 只锁定顶层对象合同。
// tools/call response 的 result 必须是对象，content 必须是数组。
// content 每个块必须是对象，并包含非空字符串 type。
// text 内容块必须包含字符串 text，避免 Adapter 的文本提取发生类型异常。
// 非文本块只做最小信封校验，以便 Client API 无损支持协议扩展。
// 图像、音频、资源与未知块不会在协议层解码、打开或执行。
// structuredContent 可缺失；出现时必须是对象，与首版 Adapter 能力一致。
// isError 可缺失并默认为 false；出现时必须是布尔值。
// isError 表示工具业务失败，不转换成 JSON-RPC RemoteProtocolError。
// _meta 可缺失；出现时必须是对象，只保存不解释。
// content、structuredContent、isError 和 _meta 分别进入稳定结果成员。
// 完整 tools/call result 同时保存到 raw_result，未知顶层扩展不会丢失。
// 解析器不根据 outputSchema 校验结果，避免实现不完整 JSON Schema 引擎。
// cancelled 通知引用原始请求 ID，并使用固定中文 reason。
// cancelled 自身不带顶层 ID，因为它是无需响应的通知。
// Server 成功响应使用空对象 result，适合 ping 等已支持控制请求。
// 未实现 Server 方法使用 JSON-RPC 标准 -32601 错误码。
// Server 响应构造器原样保留请求 ID，字符串与大无符号值均不改写。
// 分类器只判断响应、Server 请求与通知，不解释具体业务方法。
// 没有 method 的消息必须是带合法 ID 的 result/error 响应。
// 有 method 的消息不能同时带 result 或 error，避免类型歧义。
// method 必须是非空字符串，数字和空字符串都不能进入路由表。
// params 可缺失，分类结果将其规范化为空对象以简化上层分发。
// params 出现时只允许对象或数组，标量无法形成稳定方法参数。
// 有 method 且有 ID 的消息分类为 Server Request。
// 有 method 且无 ID 的消息分类为 Notification，不产生任何响应。
// 分类结果保存完整 raw 消息，供具体操作解析器再次严格校验。
// 分类器不验证 response error 正文，因为对应操作解析器负责该语义。
// 分类器不验证未知 Server method，Client 决定成功、-32601 或忽略通知。
// 协议层所有循环仅遍历当前消息，时间复杂度与 JSON 正文大小线性相关。
// 工具解析会复制 raw JSON，剩余额度检查是避免无界内存放大的关键前置。
// 本层不记录原始 JSON 日志，避免凭据、参数或 Server 私密正文泄漏。
// 本层不读取 MCP-Session-Id 或 Event-ID，这些属于 HTTP Transport 状态。
// 本层不处理 stdio Content-Length framing，Transport 先恢复完整 JSON 值。
// 本层不决定请求超时或取消线性化点，这些属于 Client 状态机。
// 本层不决定工具副作用是否未知，Client 根据提交与完成状态提升错误。
// 本层不修改 Catalog revision，目录代次由 Client 的通知处理维护。
// 本层不调用 Catalog 回调，纯解析函数可在离线单元测试中确定执行。
// 协议常量只在内部头中公开，避免应用误把版本选择当成可配置项。
// 未来支持新版本时应新增明确解析分支，不能静默复用当前字段语义。
// 未来支持更多内容块时仍应保留 raw_result 的向前兼容承诺。
// 未来支持 Server 方法时应在 Client 路由层扩展，而不是污染通用分类器。
// 未来若接受更多 ID 类型，必须先证明跨序列化器关联仍然确定。
// 构造函数的 std::invalid_argument 表示本地调用错误，不是远端 MCP 故障。
// 解析函数的 MCPException 表示不可信消息或远端协议完成结果。
// 两类异常边界固定，Adapter 才能给模型返回准确而不泄密的失败文本。
// 所有协议字段名保持规范英文，说明与固定诊断遵循项目简体中文要求。
// 测试应覆盖正常、边界与恢复相关的可解析消息，不依赖真实 Transport。
// makeInitializeRequest 不接受调用方能力对象，避免公共 API 声明未实现能力。
// makePingRequest 与其他请求共享 uint64 ID 生成器，但不共享业务 params。
// makeToolsListRequest 不验证 cursor 字符内容，因为它是 Server 签发的不透明值。
// makeToolsCallRequest 不复制 Catalog，目录有效性已由 Client 在构造前检查。
// makeCancelledNotification 允许 Server 字符串 ID，保证控制响应可以原样关联。
// makeServerSuccessResponse 不加入 method，严格保持 JSON-RPC 响应形状。
// makeServerMethodNotFoundResponse 不加入 params，错误正文只含标准字段。
// parseInitializeResponse 不保存 capabilities 原文，减少状态对象内存与攻击面。
// parseEmptyResponse 复用统一信封检查，控制响应不会形成宽松旁路。
// parseToolsListResponse 先验证 nextCursor 类型，再把完整页面交给 Client 提交。
// parseToolsCallResponse 不合并多个文本块，内容保真优先于展示便利。
// classifyIncomingMessage 缺省 params 使用新空对象，不引用静态共享可变值。
// validateOptionalStringField 只校验出现的字段，缺失值由稳定成员默认表示。
// validateOptionalObjectField 不递归解释扩展对象，避免协议层越过抽象边界。
// parseTool 的 raw 副本使调用方可以审计 Server 未知扩展而不重新读取消息。
// extractResponseResult 返回输入内引用只在单个解析调用栈内使用，不对外暴露。
// throwProtocol 统一 ProtocolViolation 构造，避免不同分支丢失 Client 状态。
// 本层不捕获内存分配失败，系统资源耗尽应按 C++ 基础异常策略处理。
// 本层不尝试修复畸形 Server 消息，首版选择确定失败而非猜测意图。
// 正常解析不生成日志，调用方可用结构化错误码进行受控观测。
// 协议单元测试直接调用这些函数，构造与解析可在无网络环境复现。

[[noreturn]] void throwProtocol(MCPClientState state, const char* message) {
    // 协议异常只暴露固定中文分类，不拼接不受信任的 Server 正文。
    throw MCPException(MCPErrorCode::ProtocolViolation, state, message);
}

void validateEnvelopeObject(const nlohmann::json& message, MCPClientState state) {
    if(!message.is_object()) {
        throwProtocol(state, "MCP JSON-RPC 消息必须是对象");
    }
    const auto version = message.find("jsonrpc");
    if(version == message.end() || !version->is_string() || version->get_ref<const std::string&>() != "2.0") {
        throwProtocol(state, "MCP JSON-RPC 版本字段非法");
    }
}

bool isValidJsonRpcId(const nlohmann::json& id) {
    // JSON-RPC 允许字符串和整数；浮点 ID 会导致跨实现关联不稳定，因此首版拒绝。
    return id.is_string() || id.is_number_unsigned() || id.is_number_integer();
}

const nlohmann::json& extractResponseResult(const nlohmann::json& message, std::uint64_t expected_id,
                                            MCPClientState state) {
    validateEnvelopeObject(message, state);
    const auto id = message.find("id");
    if(id == message.end() || (!id->is_number_unsigned() && !id->is_number_integer())) {
        throwProtocol(state, "MCP JSON-RPC 响应缺少整数 ID");
    }

    // 发送 ID 使用 uint64_t；负数或超出范围的响应不能与当前请求关联。
    // nlohmann::json 的 unsigned 值也满足 is_number_integer()，因此必须先判断更具体的无符号类型。
    std::uint64_t actual_id = 0U;
    if(id->is_number_unsigned()) {
        actual_id = id->get<std::uint64_t>();
    } else {
        const std::int64_t signed_id = id->get<std::int64_t>();
        if(signed_id < 0) {
            throwProtocol(state, "MCP JSON-RPC 响应 ID 不能为负数");
        }
        actual_id = static_cast<std::uint64_t>(signed_id);
    }
    if(actual_id != expected_id) {
        throwProtocol(state, "MCP JSON-RPC 响应 ID 与当前请求不匹配");
    }

    const bool has_result = message.contains("result");
    const bool has_error = message.contains("error");
    if(has_result == has_error) {
        throwProtocol(state, "MCP JSON-RPC 响应必须且只能包含 result 或 error");
    }
    if(has_error) {
        const auto& error = message.at("error");
        if(!error.is_object() || !error.contains("code") || !error.at("code").is_number_integer() ||
           !error.contains("message") || !error.at("message").is_string()) {
            throwProtocol(state, "MCP JSON-RPC error 对象格式非法");
        }
        // 远端 message 可能含秘密或大正文；公开异常只保留闭合错误类别。
        throw MCPException(MCPErrorCode::RemoteProtocolError, state, "MCP Server 返回 JSON-RPC 协议错误");
    }
    return message.at("result");
}

void validateOptionalStringField(const nlohmann::json& object, const char* field_name, MCPClientState state) {
    const auto field = object.find(field_name);
    if(field != object.end() && !field->is_string()) {
        throwProtocol(state, "MCP 工具的可选文本字段类型非法");
    }
}

void validateOptionalObjectField(const nlohmann::json& object, const char* field_name, MCPClientState state) {
    const auto field = object.find(field_name);
    if(field != object.end() && !field->is_object()) {
        throwProtocol(state, "MCP 工具的可选对象字段类型非法");
    }
}

MCPTool parseTool(const nlohmann::json& raw_tool, MCPClientState state) {
    if(!raw_tool.is_object()) {
        throwProtocol(state, "MCP tools/list 中的工具必须是对象");
    }
    const auto name = raw_tool.find("name");
    const auto input_schema = raw_tool.find("inputSchema");
    if(name == raw_tool.end() || !name->is_string() || name->get_ref<const std::string&>().empty()) {
        throwProtocol(state, "MCP 工具缺少非空名称");
    }
    if(input_schema == raw_tool.end() || !input_schema->is_object()) {
        throwProtocol(state, "MCP 工具 inputSchema 必须是对象");
    }
    validateOptionalStringField(raw_tool, "title", state);
    validateOptionalStringField(raw_tool, "description", state);
    validateOptionalObjectField(raw_tool, "outputSchema", state);
    validateOptionalObjectField(raw_tool, "annotations", state);
    validateOptionalObjectField(raw_tool, "execution", state);
    const auto icons = raw_tool.find("icons");
    if(icons != raw_tool.end() && !icons->is_array()) {
        throwProtocol(state, "MCP 工具 icons 必须是数组");
    }

    // 常用字段复制到稳定成员，同时把完整对象保存到 raw，保证未知扩展不会丢失。
    MCPTool tool;
    tool.name = name->get<std::string>();
    tool.title = raw_tool.value("title", std::string{});
    tool.description = raw_tool.value("description", std::string{});
    tool.input_schema = *input_schema;
    if(raw_tool.contains("outputSchema")) {
        tool.output_schema = raw_tool.at("outputSchema");
    }
    if(raw_tool.contains("annotations")) {
        tool.annotations = raw_tool.at("annotations");
    }
    if(raw_tool.contains("icons")) {
        tool.icons = raw_tool.at("icons");
    }
    if(raw_tool.contains("execution")) {
        tool.execution = raw_tool.at("execution");
    }
    tool.raw = raw_tool;
    return tool;
}

}  // namespace

nlohmann::json makeInitializeRequest(std::uint64_t request_id) {
    // Client 不声明 Roots、Sampling、Elicitation 或 Tasks，能力对象必须保持为空。
    return {
        {"jsonrpc", "2.0"                                            },
        {"id",      request_id                                       },
        {"method",  "initialize"                                     },
        {"params",
         {{"protocolVersion", kMCPProtocolVersion},
          {"capabilities", nlohmann::json::object()},
          {"clientInfo", {{"name", "ai_sdk"}, {"version", "0.1.0"}}}}}
    };
}

nlohmann::json makeInitializedNotification() {
    return {
        {"jsonrpc", "2.0"                      },
        {"method",  "notifications/initialized"},
        {"params",  nlohmann::json::object()   }
    };
}

nlohmann::json makePingRequest(std::uint64_t request_id) {
    return {
        {"jsonrpc", "2.0"                   },
        {"id",      request_id              },
        {"method",  "ping"                  },
        {"params",  nlohmann::json::object()}
    };
}

nlohmann::json makeToolsListRequest(std::uint64_t request_id, const std::optional<std::string>& cursor) {
    nlohmann::json params = nlohmann::json::object();
    // 首页省略 cursor；后续页保留 Server 给出的不透明值，不做编码或规范化。
    if(cursor.has_value() && !cursor->empty()) {
        params["cursor"] = *cursor;
    }
    return {
        {"jsonrpc", "2.0"            },
        {"id",      request_id       },
        {"method",  "tools/list"     },
        {"params",  std::move(params)}
    };
}

nlohmann::json makeToolsCallRequest(std::uint64_t request_id, const std::string& name,
                                    const nlohmann::json& arguments) {
    if(name.empty()) {
        throw std::invalid_argument("MCP 远端工具名称不能为空");
    }
    if(!arguments.is_object()) {
        throw std::invalid_argument("MCP 工具参数必须是 JSON 对象");
    }
    return {
        {"jsonrpc", "2.0"                                     },
        {"id",      request_id                                },
        {"method",  "tools/call"                              },
        {"params",  {{"name", name}, {"arguments", arguments}}}
    };
}

nlohmann::json makeCancelledNotification(const nlohmann::json& request_id) {
    if(!isValidJsonRpcId(request_id)) {
        throw std::invalid_argument("MCP 取消通知必须引用合法请求 ID");
    }
    return {
        {"jsonrpc", "2.0"                                                           },
        {"method",  "notifications/cancelled"                                       },
        {"params",  {{"requestId", request_id}, {"reason", "本地请求已取消"}}}
    };
}

nlohmann::json makeServerSuccessResponse(const nlohmann::json& request_id) {
    if(!isValidJsonRpcId(request_id)) {
        throw std::invalid_argument("MCP Server 响应必须包含合法请求 ID");
    }
    return {
        {"jsonrpc", "2.0"                   },
        {"id",      request_id              },
        {"result",  nlohmann::json::object()}
    };
}

nlohmann::json makeServerMethodNotFoundResponse(const nlohmann::json& request_id) {
    if(!isValidJsonRpcId(request_id)) {
        throw std::invalid_argument("MCP Server 响应必须包含合法请求 ID");
    }
    return {
        {"jsonrpc", "2.0"                                                                 },
        {"id",      request_id                                                            },
        {"error",   {{"code", -32601}, {"message", "客户端未实现该 Server 方法"}}}
    };
}

MCPInitializeResult parseInitializeResponse(const nlohmann::json& message, std::uint64_t expected_id,
                                            MCPClientState client_state) {
    const auto& result = extractResponseResult(message, expected_id, client_state);
    if(!result.is_object()) {
        throwProtocol(client_state, "MCP initialize result 必须是对象");
    }
    const auto protocol_version = result.find("protocolVersion");
    if(protocol_version == result.end() || !protocol_version->is_string()) {
        throwProtocol(client_state, "MCP initialize result 缺少协议版本");
    }
    if(protocol_version->get_ref<const std::string&>() != kMCPProtocolVersion) {
        throw MCPException(MCPErrorCode::VersionMismatch, client_state, "MCP Server 协商了不受支持的协议版本");
    }
    const auto capabilities = result.find("capabilities");
    if(capabilities == result.end() || !capabilities->is_object()) {
        throwProtocol(client_state, "MCP initialize result 缺少能力对象");
    }
    const auto tools = capabilities->find("tools");
    if(tools == capabilities->end() || !tools->is_object()) {
        throw MCPException(MCPErrorCode::CapabilityMissing, client_state, "MCP Server 未声明 Tools 能力");
    }

    bool list_changed = false;
    const auto list_changed_field = tools->find("listChanged");
    if(list_changed_field != tools->end()) {
        if(!list_changed_field->is_boolean()) {
            throwProtocol(client_state, "MCP Tools listChanged 能力必须是布尔值");
        }
        list_changed = list_changed_field->get<bool>();
    }
    return {protocol_version->get<std::string>(), list_changed};
}

void parseEmptyResponse(const nlohmann::json& message, std::uint64_t expected_id, MCPClientState client_state) {
    const auto& result = extractResponseResult(message, expected_id, client_state);
    if(!result.is_object()) {
        throwProtocol(client_state, "MCP ping result 必须是对象");
    }
}

MCPToolsPage parseToolsListResponse(const nlohmann::json& message, std::uint64_t expected_id,
                                    MCPClientState client_state, std::size_t remaining_tool_limit) {
    const auto& result = extractResponseResult(message, expected_id, client_state);
    if(!result.is_object()) {
        throwProtocol(client_state, "MCP tools/list result 必须是对象");
    }
    const auto tools = result.find("tools");
    if(tools == result.end() || !tools->is_array()) {
        throwProtocol(client_state, "MCP tools/list result 缺少工具数组");
    }
    // 在 reserve 和 MCPTool/raw 深拷贝之前拒绝超大单页，避免 Server 用一页正文突破目录总量上限。
    if(tools->size() > remaining_tool_limit) {
        throw MCPException(MCPErrorCode::PaginationLimitExceeded, client_state,
                           "MCP tools/list 单页工具数量超过剩余额度");
    }

    MCPToolsPage page;
    page.tools.reserve(tools->size());
    for(const auto& raw_tool : *tools) {
        page.tools.push_back(parseTool(raw_tool, client_state));
    }
    const auto cursor = result.find("nextCursor");
    if(cursor != result.end()) {
        if(!cursor->is_string()) {
            throwProtocol(client_state, "MCP tools/list nextCursor 必须是字符串");
        }
        // 空字符串等价于没有下一页，避免发送语义模糊的空游标请求。
        if(!cursor->get_ref<const std::string&>().empty()) {
            page.next_cursor = cursor->get<std::string>();
        }
    }
    return page;
}

MCPToolCallResult parseToolsCallResponse(const nlohmann::json& message, std::uint64_t expected_id,
                                         MCPClientState client_state) {
    const auto& result = extractResponseResult(message, expected_id, client_state);
    if(!result.is_object()) {
        throwProtocol(client_state, "MCP tools/call result 必须是对象");
    }
    const auto content = result.find("content");
    if(content == result.end() || !content->is_array()) {
        throwProtocol(client_state, "MCP tools/call result 缺少内容数组");
    }
    for(const auto& block : *content) {
        if(!block.is_object() || !block.contains("type") || !block.at("type").is_string() ||
           block.at("type").get_ref<const std::string&>().empty()) {
            throwProtocol(client_state, "MCP tools/call 内容块格式非法");
        }
        if(block.at("type") == "text" && (!block.contains("text") || !block.at("text").is_string())) {
            throwProtocol(client_state, "MCP 文本内容块缺少文本字段");
        }
    }

    MCPToolCallResult call_result;
    call_result.content = *content;
    const auto structured_content = result.find("structuredContent");
    if(structured_content != result.end()) {
        if(!structured_content->is_object()) {
            throwProtocol(client_state, "MCP structuredContent 必须是对象");
        }
        call_result.structured_content = *structured_content;
    }
    const auto is_error = result.find("isError");
    if(is_error != result.end()) {
        if(!is_error->is_boolean()) {
            throwProtocol(client_state, "MCP tools/call isError 必须是布尔值");
        }
        call_result.is_error = is_error->get<bool>();
    }
    const auto meta = result.find("_meta");
    if(meta != result.end()) {
        if(!meta->is_object()) {
            throwProtocol(client_state, "MCP tools/call _meta 必须是对象");
        }
        call_result.meta = *meta;
    }
    call_result.raw_result = result;
    return call_result;
}

MCPIncomingMessage classifyIncomingMessage(const nlohmann::json& message, MCPClientState client_state) {
    validateEnvelopeObject(message, client_state);
    const bool has_method = message.contains("method");
    const bool has_id = message.contains("id");
    const bool has_result = message.contains("result");
    const bool has_error = message.contains("error");

    MCPIncomingMessage incoming;
    incoming.raw = message;
    if(!has_method) {
        // 响应的 ID 与 result/error 互斥关系在此先做结构检查，具体关联由等待操作完成。
        if(!has_id || !isValidJsonRpcId(message.at("id")) || has_result == has_error) {
            throwProtocol(client_state, "MCP JSON-RPC 响应信封非法");
        }
        incoming.kind = MCPIncomingMessageKind::Response;
        incoming.id = message.at("id");
        return incoming;
    }

    if(!message.at("method").is_string() || message.at("method").get_ref<const std::string&>().empty() || has_result ||
       has_error) {
        throwProtocol(client_state, "MCP JSON-RPC 请求或通知信封非法");
    }
    incoming.method = message.at("method").get<std::string>();
    const auto params = message.find("params");
    if(params != message.end()) {
        if(!params->is_object() && !params->is_array()) {
            throwProtocol(client_state, "MCP JSON-RPC params 必须是对象或数组");
        }
        incoming.params = *params;
    } else {
        incoming.params = nlohmann::json::object();
    }

    if(has_id) {
        if(!isValidJsonRpcId(message.at("id"))) {
            throwProtocol(client_state, "MCP Server 请求 ID 类型非法");
        }
        incoming.kind = MCPIncomingMessageKind::Request;
        incoming.id = message.at("id");
    } else {
        incoming.kind = MCPIncomingMessageKind::Notification;
    }
    return incoming;
}

}  // namespace aiSDK::detail
