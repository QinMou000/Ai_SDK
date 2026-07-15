#include <gtest/gtest.h>

#include <cstdint>
#include <exception>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mcp/detail/MCPProtocol.h"

namespace {

using aiSDK::MCPClientState;
using aiSDK::MCPErrorCode;
using aiSDK::MCPException;
using aiSDK::detail::MCPIncomingMessageKind;
using nlohmann::json;

// 本文件只验证 JSON-RPC 与 MCP Tools 的纯协议边界，不执行任何进程或网络 I/O。
// 构造测试锁定请求方法、参数形状、通知无 ID 语义以及 Server 响应错误码。
// 解析测试通过公开值对象观察结果，不访问 Client 或 Transport 的内部状态。
// 所有响应都显式带预期 ID，使失败能够区分关联错误与正文结构错误。
// initialize 用例覆盖成功协商、版本拒绝、能力缺失和严格信封检查。
// JSON-RPC error 用例只断言闭合错误码，不依赖远端返回的敏感 message。
// tools/list 用例覆盖完整常用字段、未知扩展保真、游标和非法工具定义。
// tools/call 用例覆盖文本、结构化结果、业务错误、元数据和未知扩展保真。
// 富媒体内容只做最小信封校验，测试确保协议层不会丢弃未知合法字段。
// 消息分类用例分别锁定响应、Server 请求和通知三条分发路径。
// 非法组合测试集中覆盖互斥字段、ID 类型、method 类型和 params 类型。
// 协议异常同时校验失败时的 Client 状态，避免状态快照在重构中丢失。
// 测试中的英文值均为协议字段、方法名、枚举标识或不透明 Server 数据。
// 中文断言说明只描述失败原因，不把生产诊断文本当成流程判断依据。
// 每个测试均可离线、无环境变量且无时序依赖地重复运行。

// makeResponse 生成一条合法的成功响应信封。
// result 由各测试独立传入，避免辅助函数偷偷补齐被测正文。
json makeResponse(std::uint64_t id, json result) {
    return {
        {"jsonrpc", "2.0"            },
        {"id",      id               },
        {"result",  std::move(result)},
    };
}

// expectMCPError 同时检查闭合错误分类和失败状态快照。
// helper 不比较完整文本，允许诊断文字在保持中文和无秘密的前提下演进。
// 非 MCP 异常会得到明确的中文测试失败信息，便于定位异常边界漂移。
template <typename Callable>
void expectMCPError(Callable&& callable, MCPErrorCode expected_code, MCPClientState expected_state) {
    bool caught = false;
    try {
        std::forward<Callable>(callable)();
    } catch(const MCPException& exception) {
        caught = true;
        EXPECT_EQ(exception.code(), expected_code);
        EXPECT_EQ(exception.clientStateAtFailure(), expected_state);
    } catch(const std::exception& exception) {
        caught = true;
        ADD_FAILURE() << "捕获到非 MCP 异常：" << exception.what();
    } catch(...) {
        caught = true;
        ADD_FAILURE() << "捕获到未知类型异常";
    }
    if(!caught) {
        ADD_FAILURE() << "预期抛出 MCPException，但调用正常返回";
    }
}

// 初始化请求必须只声明首版支持的能力。
// 空 capabilities 防止意外宣称 Roots、Sampling、Elicitation 或 Tasks。
// clientInfo 属于协议必需身份，不应混入运行期 Server 配置。
// 构造函数输出的是 JSON 值，序列化空白不属于本层合同。
// 初始化请求使用无符号本地 ID，响应关联由后续解析器负责。
// initialized 必须在 initialize 成功之后发送，但时序由 Client 状态机管理。
// 本测试只锁定单条消息形状，不模拟完整握手时序。
// params 即使为空也必须存在，避免不同 Server 对缺失字段作不同解释。
// capabilities 的空对象类型需要显式断言，不能退化为 null 或数组。
// version 字段先使用首版固定值，后续扩展应新增明确协商测试。
TEST(MCPProtocolConstructionTest, BuildsInitializeAndInitializedMessages) {
    const json request = aiSDK::detail::makeInitializeRequest(7U);

    // 请求 ID 与方法决定响应关联和初始化路由。
    EXPECT_EQ(request.at("jsonrpc"), "2.0");
    EXPECT_EQ(request.at("id"), 7U);
    EXPECT_EQ(request.at("method"), "initialize");
    // 首版版本常量必须原样进入 initialize 参数。
    EXPECT_EQ(request.at("params").at("protocolVersion"), aiSDK::detail::kMCPProtocolVersion);
    EXPECT_EQ(request.at("params").at("capabilities"), json::object());
    // Client 信息保持稳定且不携带环境路径或凭据。
    EXPECT_EQ(request.at("params").at("clientInfo").at("name"), "ai_sdk");
    EXPECT_EQ(request.at("params").at("clientInfo").at("version"), "0.1.0");

    const json notification = aiSDK::detail::makeInitializedNotification();
    // initialized 是通知，因此不得出现可被误关联的 ID。
    EXPECT_EQ(notification.at("jsonrpc"), "2.0");
    EXPECT_EQ(notification.at("method"), "notifications/initialized");
    EXPECT_EQ(notification.at("params"), json::object());
    EXPECT_FALSE(notification.contains("id"));
}

// ping、tools/list 和 tools/call 都是需要响应的请求。
// 首页 list 省略游标，后续页原样传递非空不透明游标。
// call 参数必须保持名称和 JSON 对象，不做 Schema 层改写。
// 三类请求共用 JSON-RPC 2.0 信封，但方法与参数合同彼此独立。
// ping 的空对象参数便于 Server 使用统一请求分发接口。
// 首页省略 cursor 与发送 null 不等价，必须单独锁定字段不存在。
// 空字符串也按首页处理，防止分页器在空游标上反复请求。
// 不透明游标包含斜杠和等号，用于发现错误的 URL 编码行为。
// 工具参数包含嵌套对象，用于发现浅复制或字符串化导致的数据损失。
// 本地参数前置条件使用标准异常，与远端 MCPException 分层处理。
TEST(MCPProtocolConstructionTest, BuildsPingListAndCallRequests) {
    const json ping = aiSDK::detail::makePingRequest(8U);
    EXPECT_EQ(ping, json({
                        {"jsonrpc", "2.0"         },
                        {"id",      8U            },
                        {"method",  "ping"        },
                        {"params",  json::object()}
    }));

    // 缺失游标和空游标都表示首页，避免发送语义模糊的空字符串。
    const json first_page = aiSDK::detail::makeToolsListRequest(9U, std::nullopt);
    const json empty_cursor = aiSDK::detail::makeToolsListRequest(10U, std::string{});
    EXPECT_EQ(first_page.at("params"), json::object());
    EXPECT_EQ(empty_cursor.at("params"), json::object());
    EXPECT_EQ(first_page.at("method"), "tools/list");

    // 非空游标是 Server 所有的不透明值，协议层不得编码或裁剪。
    const std::string cursor = "下一页/opaque==";
    const json next_page = aiSDK::detail::makeToolsListRequest(11U, cursor);
    EXPECT_EQ(next_page.at("params").at("cursor"), cursor);

    // 工具参数中的嵌套对象和数组必须完整保留。
    const json arguments = {
        {"city",    "杭州"                          },
        {"options", {{"days", 3}, {"units", "metric"}}}
    };
    const json call = aiSDK::detail::makeToolsCallRequest(12U, "weather.lookup", arguments);
    EXPECT_EQ(call.at("jsonrpc"), "2.0");
    EXPECT_EQ(call.at("id"), 12U);
    EXPECT_EQ(call.at("method"), "tools/call");
    EXPECT_EQ(call.at("params").at("name"), "weather.lookup");
    EXPECT_EQ(call.at("params").at("arguments"), arguments);

    // 空名称和非对象参数属于本地调用错误，不应生成畸形协议消息。
    EXPECT_THROW(aiSDK::detail::makeToolsCallRequest(13U, "", json::object()), std::invalid_argument);
    EXPECT_THROW(aiSDK::detail::makeToolsCallRequest(13U, "weather.lookup", json::array()), std::invalid_argument);
}

// 取消通知可以引用整数或字符串 ID，并使用固定中文 reason。
// Server 成功响应和“不支持方法”响应必须使用原始请求 ID。
// 浮点、布尔和空 ID 均不符合首版稳定关联约束。
// 取消消息本身是通知，不能再生成新的请求 ID 等待响应。
// requestId 大小写遵循 MCP 协议，不能误写成 JSON-RPC 顶层 id。
// 固定 reason 不包含路径、凭据或远端正文，适合安全日志记录。
// 成功响应返回对象而非 null，为 Server 控制请求提供稳定形状。
// 方法不存在使用标准错误码，消息文本则遵守项目中文规范。
// 字符串 ID 验证 Server 主动请求可以无损完成响应关联。
// 非法 ID 矩阵覆盖三种入口，防止校验逻辑只应用于某一构造器。
TEST(MCPProtocolConstructionTest, BuildsCancellationAndServerResponses) {
    const json cancelled = aiSDK::detail::makeCancelledNotification("server-request-1");
    EXPECT_EQ(cancelled.at("jsonrpc"), "2.0");
    EXPECT_EQ(cancelled.at("method"), "notifications/cancelled");
    EXPECT_EQ(cancelled.at("params").at("requestId"), "server-request-1");
    EXPECT_EQ(cancelled.at("params").at("reason"), "本地请求已取消");
    EXPECT_FALSE(cancelled.contains("id"));

    // Server 成功响应返回空对象，满足 ping 等控制请求的最小结果合同。
    const json success = aiSDK::detail::makeServerSuccessResponse(21);
    EXPECT_EQ(success, json({
                           {"jsonrpc", "2.0"         },
                           {"id",      21            },
                           {"result",  json::object()}
    }));

    // 未实现方法使用 JSON-RPC 标准 -32601，并保持中文安全文本。
    const json not_found = aiSDK::detail::makeServerMethodNotFoundResponse("request-x");
    EXPECT_EQ(not_found.at("id"), "request-x");
    EXPECT_EQ(not_found.at("error").at("code"), -32601);
    EXPECT_EQ(not_found.at("error").at("message"), "客户端未实现该 Server 方法");

    // 三个构造入口必须一致拒绝无法稳定关联的 ID。
    EXPECT_THROW(aiSDK::detail::makeCancelledNotification(1.5), std::invalid_argument);
    EXPECT_THROW(aiSDK::detail::makeServerSuccessResponse(nullptr), std::invalid_argument);
    EXPECT_THROW(aiSDK::detail::makeServerMethodNotFoundResponse(true), std::invalid_argument);
}

// 合法 initialize 响应必须返回协商版本和 listChanged 能力。
// Server 的其他能力与自报信息不进入结果，避免调用方误判首版支持范围。
// capabilities.tools 必须是对象，即使当前只读取 listChanged 一个字段。
// 未声明的 Resources 能力被安全忽略，不代表 Client 支持相应方法。
// serverInfo 只作为不可信原始输入存在，不进入稳定初始化结果。
// listChanged=true 允许 Client 监听目录失效通知，但不自动刷新目录。
// listChanged 缺失采用 false，符合“未声明即不支持”的能力原则。
// 协议版本返回稳定字符串，供状态机记录已经协商的明确版本。
// 正常与缺省分支共享同一状态，避免测试把状态变化混入纯解析逻辑。
TEST(MCPProtocolInitializeTest, ParsesSupportedToolsCapability) {
    const json response = makeResponse(
        30U, {
                 {"protocolVersion", aiSDK::detail::kMCPProtocolVersion                                        },
                 {"capabilities",    {{"tools", {{"listChanged", true}}}, {"resources", {{"subscribe", true}}}}},
                 {"serverInfo",      {{"name", "测试服务"}, {"version", "1.0"}}                            }
    });

    const aiSDK::detail::MCPInitializeResult result =
        aiSDK::detail::parseInitializeResponse(response, 30U, MCPClientState::Initializing);

    EXPECT_EQ(result.protocol_version, aiSDK::detail::kMCPProtocolVersion);
    EXPECT_TRUE(result.tools_list_changed);

    // listChanged 缺失时默认为 false，不能把“未声明”当成支持变更通知。
    const json without_list_changed = makeResponse(31U, {
                                                            {"protocolVersion", aiSDK::detail::kMCPProtocolVersion},
                                                            {"capabilities",    {{"tools", json::object()}}       }
    });
    const auto default_result =
        aiSDK::detail::parseInitializeResponse(without_list_changed, 31U, MCPClientState::Initializing);
    EXPECT_FALSE(default_result.tools_list_changed);
}

// 不受支持的版本必须使用 VersionMismatch，而不是笼统协议错误。
// 缺少 Tools 能力则使用 CapabilityMissing，便于上层给出能力层诊断。
// 两类失败都发生在信封与 ID 校验之后，错误分类应指向真实根因。
// 旧版本字符串是格式合法但语义不支持的输入，不属于结构破损。
// 版本失败不能静默采用 Server 返回值，否则能力语义可能发生漂移。
// resources 对象证明 capabilities 自身合法，只有 Tools 能力缺失。
// 能力缺失不能降级为仅 ping Client，因为首版产品范围明确要求 Tools。
// 状态快照保持 Initializing，便于上层区分连接与运行期失败。
// 本测试不绑定中文 what 文案，调用方应依据结构化错误码分支。
TEST(MCPProtocolInitializeTest, RejectsVersionMismatchAndMissingToolsCapability) {
    const json wrong_version =
        makeResponse(32U, {
                              {"protocolVersion", "2024-11-05"               },
                              {"capabilities",    {{"tools", json::object()}}}
    });
    expectMCPError([&] { aiSDK::detail::parseInitializeResponse(wrong_version, 32U, MCPClientState::Initializing); },
                   MCPErrorCode::VersionMismatch, MCPClientState::Initializing);

    // resources 存在不能替代首版必需的 Tools 能力。
    const json missing_tools = makeResponse(33U, {
                                                     {"protocolVersion", aiSDK::detail::kMCPProtocolVersion},
                                                     {"capabilities",    {{"resources", json::object()}}   }
    });
    expectMCPError([&] { aiSDK::detail::parseInitializeResponse(missing_tools, 33U, MCPClientState::Initializing); },
                   MCPErrorCode::CapabilityMissing, MCPClientState::Initializing);
}

// 响应信封必须是 JSON 对象、版本为 2.0、ID 匹配且 result/error 二选一。
// 矩阵中的每个输入只破坏一个主要约束，方便定位回归来源。
// 数组输入覆盖顶层类型，错误版本覆盖固定 JSON-RPC 常量。
// 缺失 ID 无法关联在途请求，因此不能作为无 ID 通知处理。
// 负整数虽然是 JSON-RPC 可表示值，却不能匹配本地无符号请求序列。
// 不同正整数覆盖响应串线，不能交给其他等待者隐式消费。
// result 与 error 同时存在会产生矛盾完成语义，必须整体拒绝。
// 两者都不存在也不是合法响应，不能把缺失 result 当作空对象。
// 所有分支保留 Initializing 快照，确保错误诊断可追溯到握手阶段。
TEST(MCPProtocolInitializeTest, RejectsInvalidResponseEnvelopes) {
    const json valid_result = {
        {"protocolVersion", aiSDK::detail::kMCPProtocolVersion},
        {"capabilities",    {{"tools", json::object()}}       }
    };
    const std::vector<json> invalid_messages = {
        json::array(),
        {{"jsonrpc", "1.0"}, {"id", 34U}, {"result", valid_result}},
        {{"jsonrpc", "2.0"}, {"result", valid_result}},
        {{"jsonrpc", "2.0"}, {"id", -1}, {"result", valid_result}},
        {{"jsonrpc", "2.0"}, {"id", 35U}, {"result", valid_result}},
        {{"jsonrpc", "2.0"}, {"id", 34U}, {"result", valid_result}, {"error", {{"code", -32000}, {"message", "冲突"}}}},
        {{"jsonrpc", "2.0"}, {"id", 34U}},
    };

    // 每个非法信封都应收敛为 ProtocolViolation，并保留初始化状态快照。
    for(const json& message : invalid_messages) {
        expectMCPError([&] { aiSDK::detail::parseInitializeResponse(message, 34U, MCPClientState::Initializing); },
                       MCPErrorCode::ProtocolViolation, MCPClientState::Initializing);
    }
}

// nlohmann::json 的无符号整数也满足通用整数查询，解析器必须先走 unsigned 分支。
// 该值大于 INT64_MAX，若误走 signed get 会抛库异常而不是完成合法响应关联。
// 分类阶段同样应原样保留 uint64，不能因内部转换把高位 ID 改写为负数。
TEST(MCPProtocolResponseIdTest, PreservesUnsignedIdAboveSignedRange) {
    constexpr std::uint64_t large_id = std::numeric_limits<std::uint64_t>::max();
    const json response = makeResponse(large_id, json::object());

    EXPECT_NO_THROW(aiSDK::detail::parseEmptyResponse(response, large_id, MCPClientState::Ready));
    const auto incoming = aiSDK::detail::classifyIncomingMessage(response, MCPClientState::Ready);
    EXPECT_TRUE(incoming.id.is_number_unsigned());
    EXPECT_EQ(incoming.id.get<std::uint64_t>(), large_id);
}

// 合法 JSON-RPC error 应映射为 RemoteProtocolError。
// 远端 message 可能包含秘密，公开异常文本不得把它原样拼接出来。
// error.code 与 error.message 类型合法，因此不应误判为信封结构错误。
// data 属于不可信扩展，协议层既不执行也不拼入公开异常。
// 测试放入明确秘密标记，以直接观察文本净化边界是否失守。
// 错误码映射保留远端协议失败语义，调用方不需要解析 what 文本。
// Client 状态仍为 Initializing，因为远端错误完成发生在协商阶段。
// 本用例不要求保存原始 error，避免扩展对象经异常路径泄漏。
// 若后续增加安全诊断字段，应继续使用白名单而非复制远端正文。
TEST(MCPProtocolInitializeTest, MapsJsonRpcErrorWithoutLeakingRemoteMessage) {
    const json response = {
        {"jsonrpc", "2.0"                                                                                               },
        {"id",      36U                                                                                                 },
        {"error",   {{"code", -32001}, {"message", "远端秘密令牌 secret-123"}, {"data", {{"trace", "不可信"}}}}},
    };

    try {
        aiSDK::detail::parseInitializeResponse(response, 36U, MCPClientState::Initializing);
        FAIL() << "预期 JSON-RPC error 抛出 MCPException";
    } catch(const MCPException& exception) {
        EXPECT_EQ(exception.code(), MCPErrorCode::RemoteProtocolError);
        EXPECT_EQ(exception.clientStateAtFailure(), MCPClientState::Initializing);
        EXPECT_EQ(std::string(exception.what()).find("secret-123"), std::string::npos);
    }
}

// ping 的空对象结果是唯一有效的控制响应形状。
// 非对象结果即使信封合法，也必须在协议边界拒绝。
// parseEmptyResponse 复用响应关联和 JSON-RPC error 校验。
// 成功分支证明空对象不会因没有成员而被误判为缺失 result。
// 数组输入专门区分“空集合”与“空对象”的协议类型差异。
// 该解析器用于控制操作，不解释任何业务字段或工具内容。
// Ready 状态快照覆盖运行期 ping，而不是初始化阶段控制消息。
// 测试没有时钟依赖，请求超时属于 Client 而非纯解析器职责。
// 未来如协议允许控制结果字段，应通过明确对象字段测试扩展。
TEST(MCPProtocolInitializeTest, ParsesEmptyControlResponse) {
    const json success = makeResponse(37U, json::object());
    EXPECT_NO_THROW(aiSDK::detail::parseEmptyResponse(success, 37U, MCPClientState::Ready));

    // 数组不能冒充空对象，否则后续控制协议扩展会失去稳定形状。
    const json invalid = makeResponse(38U, json::array());
    expectMCPError([&] { aiSDK::detail::parseEmptyResponse(invalid, 38U, MCPClientState::Ready); },
                   MCPErrorCode::ProtocolViolation, MCPClientState::Ready);
}

// tools/list 应解析常用字段，同时无损保留单个工具的完整原始对象。
// 未知扩展只进入 raw，不能被协议层解释或丢弃。
// 工具名称保持远端原样，调用时不能改写为本地注册名称。
// inputSchema 是必需对象，outputSchema 则允许合法缺失。
// annotations 与 execution 只保存值，不把提示视为安全策略。
// icons 数组按不受信任展示数据保留，不在协议解析层下载资源。
// vendorExtension 用于验证未知字段经过解析后仍能完整获取。
// nextCursor 包含不透明字符，分页请求必须原样回传给同一 Server。
// 空游标归一化为无下一页，阻断 Server 空游标造成的循环风险。
TEST(MCPProtocolToolsListTest, ParsesCompleteToolAndOpaqueCursor) {
    const json raw_tool = {
        {"name",            "weather.lookup"                                                                         },
        {"title",           "天气查询"                                                                           },
        {"description",     "查询指定城市天气"                                                               },
        {"inputSchema",     {{"type", "object"}, {"properties", {{"city", {{"type", "string"}}}}}}                   },
        {"outputSchema",    {{"type", "object"}, {"properties", {{"temperature", {{"type", "number"}}}}}}            },
        {"annotations",     {{"readOnlyHint", true}}                                                                 },
        {"icons",           json::array({{{"src", "data:image/svg+xml;base64,AA=="}, {"mimeType", "image/svg+xml"}}})},
        {"execution",       {{"taskSupport", "forbidden"}}                                                           },
        {"vendorExtension", {{"revision", 9}}                                                                        },
    };
    const json response = makeResponse(40U, {
                                                {"tools",      json::array({raw_tool})},
                                                {"nextCursor", "opaque/cursor=="      }
    });

    const aiSDK::detail::MCPToolsPage page =
        aiSDK::detail::parseToolsListResponse(response, 40U, MCPClientState::Ready, 8U);

    ASSERT_EQ(page.tools.size(), 1U);
    const aiSDK::MCPTool& tool = page.tools.front();
    // 稳定成员为 Adapter 和目录查询提供便捷访问。
    EXPECT_EQ(tool.name, "weather.lookup");
    EXPECT_EQ(tool.title, "天气查询");
    EXPECT_EQ(tool.description, "查询指定城市天气");
    EXPECT_EQ(tool.input_schema, raw_tool.at("inputSchema"));
    EXPECT_EQ(tool.output_schema, raw_tool.at("outputSchema"));
    EXPECT_EQ(tool.annotations, raw_tool.at("annotations"));
    EXPECT_EQ(tool.icons, raw_tool.at("icons"));
    EXPECT_EQ(tool.execution, raw_tool.at("execution"));
    // 完整 raw 必须保留未知 vendorExtension，证明解析没有有损重建对象。
    EXPECT_EQ(tool.raw, raw_tool);
    ASSERT_TRUE(page.next_cursor.has_value());
    EXPECT_EQ(*page.next_cursor, "opaque/cursor==");

    // 空 nextCursor 按“无下一页”处理，避免 Client 发送空游标死循环。
    const json last_page = makeResponse(41U, {
                                                 {"tools",      json::array()},
                                                 {"nextCursor", ""           }
    });
    const auto parsed_last_page = aiSDK::detail::parseToolsListResponse(last_page, 41U, MCPClientState::Ready, 0U);
    EXPECT_TRUE(parsed_last_page.tools.empty());
    EXPECT_FALSE(parsed_last_page.next_cursor.has_value());
}

// 每个工具都必须是对象，并具有非空名称和对象 inputSchema。
// 可选字段一旦出现，也必须使用对应的稳定 JSON 类型。
// 字符串工具覆盖顶层对象要求，防止 value() 隐式转换掩盖错误。
// 空名称不能进入 Catalog，否则 Adapter 无法生成可调用绑定。
// 缺失 Schema 与数组 Schema 分开覆盖存在性和类型两个约束。
// description 数字覆盖可选文本字段的严格类型检查。
// icons 对象覆盖数组展示字段，避免上层遍历时发生类型异常。
// execution 数组覆盖不可信执行元数据仍需保持对象形状。
// 每轮使用新 ID，使失败不依赖某个固定数字的特殊处理。
TEST(MCPProtocolToolsListTest, RejectsInvalidToolDefinitions) {
    const std::vector<json> invalid_tools = {
        "not-an-object",
        {{"name", ""}, {"inputSchema", json::object()}},
        {{"name", "missing-schema"}},
        {{"name", "array-schema"}, {"inputSchema", json::array()}},
        {{"name", "bad-description"}, {"inputSchema", json::object()}, {"description", 7}},
        {{"name", "bad-icons"}, {"inputSchema", json::object()}, {"icons", json::object()}},
        {{"name", "bad-execution"}, {"inputSchema", json::object()}, {"execution", json::array()}},
    };

    // 所有非法工具都放入合法 tools/list 信封，确保失败来自工具正文校验。
    std::uint64_t id = 50U;
    for(const json& invalid_tool : invalid_tools) {
        const json response = makeResponse(id, {
                                                   {"tools", json::array({invalid_tool})}
        });
        expectMCPError([&] { aiSDK::detail::parseToolsListResponse(response, id, MCPClientState::Ready, 8U); },
                       MCPErrorCode::ProtocolViolation, MCPClientState::Ready);
        ++id;
    }
}

// nextCursor 必须是字符串；数字和对象都不能成为下一次请求参数。
// tools 字段本身也必须存在且为数组，不能依靠隐式空列表降级。
// 缺失 tools 代表响应不完整，不能被当成合法的空目录。
// 对象 tools 防止解析器错误遍历键值并生成虚假工具。
// 数字游标覆盖常见的宽松 JSON 转字符串风险。
// 对象游标覆盖复杂值，确保协议层不自行序列化游标。
// 全部信封本身合法，断言失败来源集中于 result 正文。
// Ready 状态表示目录拉取是公开运行期操作而非初始化握手。
// 解析失败不返回部分页，跨页提交原子性由上层 Client 继续保证。
TEST(MCPProtocolToolsListTest, RejectsInvalidToolsArrayAndCursor) {
    const std::vector<json> invalid_results = {
        json::object(),
        {{"tools", json::object()}},
        {{"tools", json::array()}, {"nextCursor", 17}},
        {{"tools", json::array()}, {"nextCursor", json::object()}},
    };

    // 使用不同 ID 避免测试意外依赖某个固定请求值。
    std::uint64_t id = 60U;
    for(const json& invalid_result : invalid_results) {
        const json response = makeResponse(id, invalid_result);
        expectMCPError([&] { aiSDK::detail::parseToolsListResponse(response, id, MCPClientState::Ready, 8U); },
                       MCPErrorCode::ProtocolViolation, MCPClientState::Ready);
        ++id;
    }
}

// 单页数量必须在 reserve、MCPTool 构造和 raw JSON 深拷贝前与剩余额度比较。
// 第二个元素故意使用非法工具；若实现先解析正文，错误会错误地落为 ProtocolViolation。
// 空页在剩余额度为零时仍然合法，避免达到总量上限后无法完成最后一页。
TEST(MCPProtocolToolsListTest, RejectsOversizedPageBeforeCopyingTools) {
    const json oversized = makeResponse(
        64U, {
                 {"tools", json::array({{{"name", "valid"}, {"inputSchema", json::object()}}, "非法工具"})}
    });
    expectMCPError([&] { aiSDK::detail::parseToolsListResponse(oversized, 64U, MCPClientState::Ready, 1U); },
                   MCPErrorCode::PaginationLimitExceeded, MCPClientState::Ready);

    const json empty_page = makeResponse(65U, {
                                                  {"tools", json::array()}
    });
    EXPECT_TRUE(aiSDK::detail::parseToolsListResponse(empty_page, 65U, MCPClientState::Ready, 0U).tools.empty());
}

// tools/call 成功解析必须同时保留内容、结构化对象、业务错误标记和元数据。
// raw_result 无损保存未知字段，为上层需要完整 MCP 语义时提供原始值。
// text 块的字符串字段是首版 Adapter 可直接消费的主要内容。
// structuredContent 必须保留对象类型，供结构化 ToolResult 优先映射。
// isError=true 是工具业务失败，不应转换成 JSON-RPC 异常。
// _meta 只保存不解释，避免远端元数据影响本地控制流程。
// audience 是内容块未知字段，用于验证块级别也保持原始值。
// vendorExtension 是结果未知字段，用于验证顶层 raw_result 保真。
// 解析器返回值为独立 JSON 值，测试只比较内容而不依赖引用寿命。
TEST(MCPProtocolToolsCallTest, ParsesTextStructuredContentAndRawResult) {
    const json result = {
        {"content",           json::array({{{"type", "text"}, {"text", "查询失败：城市不存在"}, {"audience", "user"}}})},
        {"structuredContent", {{"code", "CITY_NOT_FOUND"}, {"retryable", false}}                                                 },
        {"isError",           true                                                                                               },
        {"_meta",             {{"requestRevision", 42}}                                                                          },
        {"vendorExtension",   {{"latencyMs", 12}}                                                                                },
    };
    const json response = makeResponse(70U, result);

    const aiSDK::MCPToolCallResult parsed = aiSDK::detail::parseToolsCallResponse(response, 70U, MCPClientState::Ready);

    // 文本块中的未知 audience 字段也必须被完整保留。
    EXPECT_EQ(parsed.content, result.at("content"));
    EXPECT_EQ(parsed.structured_content, result.at("structuredContent"));
    EXPECT_TRUE(parsed.is_error);
    EXPECT_EQ(parsed.meta, result.at("_meta"));
    // 原始结果包含所有已知和未知字段，不能由稳定成员重新拼装。
    EXPECT_EQ(parsed.raw_result, result);
}

// 图像、音频、资源链接和嵌入资源都属于合法富媒体内容。
// 首版协议层只要求非空 type，不应误删数据、MIME 类型或扩展字段。
// 图像与音频的 base64 数据只保存，不在协议层解码或验证媒体内容。
// resource_link 的 URI 只作为数据保留，不能触发本地文件或网络访问。
// resource 嵌入正文保持原始嵌套对象，交由明确的上层能力处理。
// vendor/custom 证明未知非空类型可以前向兼容地通过协议解析。
// 缺失 structuredContent 和 _meta 应保持 null，而不是伪造空对象。
// 缺失 isError 使用 false，代表正常工具结果的协议默认值。
// raw_result 与 content 同时断言，覆盖稳定字段和完整对象两条访问路径。
TEST(MCPProtocolToolsCallTest, PreservesRichMediaContentBlocks) {
    const json content = json::array({
        {{"type", "image"}, {"data", "aW1hZ2U="}, {"mimeType", "image/png"}},
        {{"type", "audio"}, {"data", "YXVkaW8="}, {"mimeType", "audio/wav"}},
        {{"type", "resource_link"}, {"uri", "file:///report.json"}, {"name", "报告"}},
        {{"type", "resource"}, {"resource", {{"uri", "memory://item/1"}, {"text", "资源正文"}}}},
        {{"type", "vendor/custom"}, {"opaque", json::array({1, 2, 3})}},
    });
    const json result = {
        {"content", content}
    };

    const auto parsed = aiSDK::detail::parseToolsCallResponse(makeResponse(71U, result), 71U, MCPClientState::Ready);

    EXPECT_EQ(parsed.content, content);
    EXPECT_FALSE(parsed.is_error);
    EXPECT_TRUE(parsed.structured_content.is_null());
    EXPECT_TRUE(parsed.meta.is_null());
    EXPECT_EQ(parsed.raw_result, result);
}

// content 必须是数组，数组元素必须是带非空字符串 type 的对象。
// text 块额外要求字符串 text；其他可选顶层字段也有固定类型。
// 缺失 content 与错误容器类型分开覆盖存在性和数组约束。
// 标量块防止解析器对非对象调用 contains 后静默接受。
// 空对象和空 type 分别覆盖字段缺失与非空字符串要求。
// text 缺失和数字 text 分别覆盖存在性与严格文本类型。
// structuredContent 数组不能降级为结构化对象，避免 Adapter 类型漂移。
// isError 字符串不能按真值宽松转换，业务失败标记必须是布尔值。
// _meta 数组不能作为对象暴露，防止调用方错误使用键查找。
TEST(MCPProtocolToolsCallTest, RejectsInvalidCallContentAndOptionalFields) {
    const std::vector<json> invalid_results = {
        json::object(),
        {{"content", json::object()}},
        {{"content", json::array({"not-an-object"})}},
        {{"content", json::array({json::object()})}},
        {{"content", json::array({{{"type", ""}}})}},
        {{"content", json::array({{{"type", "text"}}})}},
        {{"content", json::array({{{"type", "text"}, {"text", 9}}})}},
        {{"content", json::array()}, {"structuredContent", json::array()}},
        {{"content", json::array()}, {"isError", "true"}},
        {{"content", json::array()}, {"_meta", json::array()}},
    };

    // 所有异常都发生在 result 校验阶段，因此统一映射 ProtocolViolation。
    std::uint64_t id = 80U;
    for(const json& invalid_result : invalid_results) {
        const json response = makeResponse(id, invalid_result);
        expectMCPError([&] { aiSDK::detail::parseToolsCallResponse(response, id, MCPClientState::Ready); },
                       MCPErrorCode::ProtocolViolation, MCPClientState::Ready);
        ++id;
    }
}

// 无 method 且包含合法 ID 与 result/error 之一的消息属于响应。
// 分类阶段只负责信封路由，不提前解释具体 result 或 error 正文。
// 成功响应使用整数 ID，覆盖 Client 主动请求的常见关联方式。
// 错误响应使用字符串 ID，覆盖 Server 或测试传输的合法关联方式。
// success 的 result 正文保持 raw，不在分类阶段提前解析业务结构。
// error 故意省略 message，证明具体 error 格式留给操作解析器判断。
// 分类结果不设置 method，避免响应误入 Server 请求分发路径。
// 原始消息逐值比较，确保分类过程没有删除未知或未解释字段。
// 本测试只验证类别，不验证响应是否匹配当前某个在途公开操作。
TEST(MCPProtocolClassificationTest, ClassifiesSuccessAndErrorResponses) {
    const json success = {
        {"jsonrpc", "2.0"         },
        {"id",      90U           },
        {"result",  {{"value", 1}}}
    };
    const auto success_message = aiSDK::detail::classifyIncomingMessage(success, MCPClientState::Ready);
    EXPECT_EQ(success_message.kind, MCPIncomingMessageKind::Response);
    EXPECT_EQ(success_message.id, 90U);
    EXPECT_TRUE(success_message.method.empty());
    EXPECT_EQ(success_message.raw, success);

    // error 正文格式由具体响应解析器负责，分类器只锁定 result/error 互斥。
    const json error = {
        {"jsonrpc", "2.0"             },
        {"id",      "op-91"           },
        {"error",   {{"code", -32000}}}
    };
    const auto error_message = aiSDK::detail::classifyIncomingMessage(error, MCPClientState::Ready);
    EXPECT_EQ(error_message.kind, MCPIncomingMessageKind::Response);
    EXPECT_EQ(error_message.id, "op-91");
    EXPECT_EQ(error_message.raw, error);
}

// 带合法 ID 的 method 消息属于 Server 请求；没有 ID 的属于通知。
// params 允许对象或数组，缺失时规范化为空对象以简化上层分发。
// Server 请求使用字符串 ID，响应构造器必须能够原样引用该值。
// 数组 params 是 JSON-RPC 合法形式，不能被错误限制为对象。
// method 原样保存，具体支持范围由上层 Server 请求处理器决定。
// 通知没有 ID，任何响应都会违反 JSON-RPC 通知语义。
// list_changed 通知只使 Catalog 代次失效，不在分类器内触发刷新。
// 缺失 params 统一为空对象，减少分发层的重复缺省处理。
// raw 字段保留完整输入，便于受控诊断和后续方法级解析。
TEST(MCPProtocolClassificationTest, ClassifiesServerRequestsAndNotifications) {
    const json request = {
        {"jsonrpc", "2.0"              },
        {"id",      "server-1"         },
        {"method",  "ping"             },
        {"params",  json::array({1, 2})},
    };
    const auto request_message = aiSDK::detail::classifyIncomingMessage(request, MCPClientState::Ready);
    EXPECT_EQ(request_message.kind, MCPIncomingMessageKind::Request);
    EXPECT_EQ(request_message.id, "server-1");
    EXPECT_EQ(request_message.method, "ping");
    EXPECT_EQ(request_message.params, json::array({1, 2}));
    EXPECT_EQ(request_message.raw, request);

    // tools/list_changed 无参数也应成为可分发通知，而不是协议错误。
    const json notification = {
        {"jsonrpc", "2.0"                             },
        {"method",  "notifications/tools/list_changed"}
    };
    const auto notification_message = aiSDK::detail::classifyIncomingMessage(notification, MCPClientState::Ready);
    EXPECT_EQ(notification_message.kind, MCPIncomingMessageKind::Notification);
    EXPECT_TRUE(notification_message.id.is_null());
    EXPECT_EQ(notification_message.method, "notifications/tools/list_changed");
    EXPECT_EQ(notification_message.params, json::object());
    EXPECT_EQ(notification_message.raw, notification);
}

// JSON-RPC 三类消息的字段组合必须互斥，不能同时表现为响应和请求。
// method、ID 与 params 也必须使用首版可稳定关联和分发的类型。
// 错误版本先在公共信封校验失败，不能进入后续类别推断。
// 只有 ID 没有 result/error 的消息不构成完整响应。
// result/error 共存会产生矛盾类别，必须在排队前丢弃。
// 浮点响应 ID 与布尔请求 ID都无法稳定匹配首版关联键。
// method 与 result 共存会同时表现为请求和响应，因此整体非法。
// 空 method 和数字 method 都无法进入字符串方法路由表。
// 标量 params 不符合 JSON-RPC 对象或数组约束，不能宽松包装。
TEST(MCPProtocolClassificationTest, RejectsInvalidMessageCombinations) {
    const std::vector<json> invalid_messages = {
        {{"jsonrpc", "1.0"}, {"method", "ping"}},
        {{"jsonrpc", "2.0"}, {"id", 1U}},
        {{"jsonrpc", "2.0"}, {"id", 1U}, {"result", json::object()}, {"error", json::object()}},
        {{"jsonrpc", "2.0"}, {"id", 1.25}, {"result", json::object()}},
        {{"jsonrpc", "2.0"}, {"method", "ping"}, {"result", json::object()}},
        {{"jsonrpc", "2.0"}, {"method", ""}},
        {{"jsonrpc", "2.0"}, {"method", 7}},
        {{"jsonrpc", "2.0"}, {"id", false}, {"method", "ping"}},
        {{"jsonrpc", "2.0"}, {"method", "ping"}, {"params", "scalar"}},
    };

    // 分类器统一抛 ProtocolViolation，并记录分类发生时的 Ready 状态。
    for(const json& message : invalid_messages) {
        expectMCPError([&] { aiSDK::detail::classifyIncomingMessage(message, MCPClientState::Ready); },
                       MCPErrorCode::ProtocolViolation, MCPClientState::Ready);
    }
}

}  // namespace
