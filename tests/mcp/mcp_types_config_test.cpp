#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "mcp/MCPServerConfig.h"
#include "mcp/MCPTypes.h"
#include "mcp/detail/MCPDeadline.h"

namespace {

// 本文件只验证 MCP 值类型和静态配置边界，不执行进程或网络 I/O。
// 测试通过公开 API 观察行为，避免依赖 Client 私有签发令牌或传输实现。
// 配置辅助函数始终返回一份独立值，失败用例之间不会共享可变状态。
// 绝对路径按宿主平台构造，只验证语法合同，不要求文件真实存在。
// 所有非法输入都在 validateMCPServerConfig 调用前显式注入。
// 断言重点是稳定枚举、异常类型和公开字段，不绑定诊断文案细节。
// stdio 用例覆盖命令边界、参数边界、环境边界和资源上限。
// HTTP 用例覆盖来源限制、URL authority、端口、SSE 和凭据边界。
// Provider 仅作为配置占位，不会在静态校验期间被调用。
// 如果 Provider 意外被调用，测试中的计数器会暴露静态校验发生了 I/O。
// 默认 Catalog 只能验证无效公开面，不能伪造 Client 私有签发状态。
// OutcomeUnknown 用例同时锁定根因、执行可能性和失败状态快照。
// 其他错误不得携带结果未知字段，避免上层错误地重试副作用工具。
// 回环 HTTP 只接受字面量地址，localhost 不因本机解析结果而放行。
// HTTPS 不依赖开发开关，保持生产 Endpoint 的默认安全路径。
// Header 名称按 HTTP token 校验，且不能覆盖 SDK 控制的保留字段。
// 大小和超时使用零值触发边界，避免依赖平台整数溢出行为。
// 每个测试均可离线重复运行，不读取环境变量、证书或代理配置。
// 错误码表覆盖当前闭合枚举的全部十八个成员，防止漏掉尾部新增项。
// 非法枚举只检查统一兜底名称，不把未定义数值当作可恢复错误分类。
// 普通异常检查 what 文本只是确认传递完整，不把文本用于流程分支。
// 异常状态快照直接断言 Ready 与 Faulted，避免构造后读取实时状态。
// OutcomeUnknown 的 cause 使用 TransportFailure，代表请求提交后的断链根因。
// 缺失 cause 与 may_have_executed 分别测试，防止实现只校验其中一项。
// 递归 OutcomeUnknown 根因被单独拒绝，避免形成无法终止的错误包装链。
// 普通错误携带 cause 和执行标记分别测试，保证反向不变量也成立。
// 默认 Catalog 的 tools、server_id 和 revision 一起检查，确认没有伪默认值。
// valid 只检查签发令牌，不推断空工具列表本身是否有效。
// stdio 合法样例包含两个参数，确保数组传递并非只允许空列表。
// stdio 合法样例包含环境项，确保静态校验不会误拒绝常规变量。
// stdio 工作目录使用不存在的绝对路径，锁定静态阶段不得探测磁盘。
// 执行文件故意没有扩展名，兼容 POSIX 二进制与 Windows 测试占位路径。
// 相对 executable 和 working_directory 分开失败，明确两个字段各自受限。
// CMD 使用大写扩展名，验证扩展名匹配不依赖调用方大小写规范。
// BAT 使用小写扩展名，与 CMD 一起覆盖首版禁止的脚本入口集合。
// 参数 NUL 使用显式长度构造，避免 C 字符串在测试构造阶段提前截断。
// 参数回车和换行分开输入，保证两种行边界字符都被静态层识别。
// 环境名称空值与等号分别代表缺失名称和环境块分隔符注入。
// 环境名称 NUL 同样使用显式长度，保证非法后缀实际进入 std::string。
// 环境名称换行验证日志或平台块边界不能由调用方输入破坏。
// 环境值 NUL 与换行分别覆盖静默截断和多行边界两类风险。
// 公共限制逐项使用新配置，保证变异不会在同一对象上累积。
// request_timeout 零值覆盖普通前台请求的空截止时间。
// absolute_request_timeout 零值覆盖重试后仍必须存在的总截止时间。
// close_timeout 零值覆盖关闭协调不能无限或立即失效的资源边界。
// 消息、队列、错误文本、分页和工具数量都不能用零值关闭保护。
// HTTPS 样例同时包含显式端口、路径和查询参数，覆盖常用 Endpoint 形态。
// HTTPS 合法性不依赖 allow_loopback_http，避免开发开关污染生产路径。
// IPv4 样例选择 127.42.7.9，证明校验遵循完整 127/8 而非单一地址。
// IPv6 样例使用方括号和显式端口，验证 authority 分割不误判冒号。
// 明文回环在开关关闭时失败，证明安全默认值确实生效。
// localhost 在开关开启时仍失败，避免 DNS 或 hosts 文件影响静态结论。
// 私网 IPv4 不属于回环，不能借用开发开关建立明文远程连接。
// 普通域名和 128.0.0.1 补足非回环主机的边界两侧。
// URL 用户信息同时带用户名和密码，验证 authority 中的 at 符号被拒绝。
// Fragment 位于合法路径之后，证明解析不能只检查主机前缀。
// 空端口、零端口、越界端口、非数字端口和超大端口分别覆盖解析分支。
// 超大端口用于验证逐位累积不会先发生无符号整数溢出。
// SSE 事件大小等于消息上限时合法，严格大于时才拒绝。
// connect_timeout 与 stream_idle_timeout 零值分别覆盖短连接和长流路径。
// credential_timeout 超过 close_timeout 会阻塞关闭，因此测试明确拒绝。
// max_sse_retry_delay 零值不能退化为无间隔重连风暴。
// SSE 事件、事件 ID、会话 ID 与重连次数的零值逐项验证。
// Bearer 空 Provider 和固定 Header 空 Provider 是两个独立 variant 分支。
// 合法 Provider 的 calls 计数在所有静态校验后都必须保持为零。
// Authorization 保留项代表应用不能替换 SDK 选择的 Bearer 语义。
// MCP-Session-Id 保留项代表应用不能覆盖传输维护的会话身份。
// Content-Type 与 Host 保留项保护正文解析和远端来源边界。
// Proxy 自定义前缀代表整个代理控制命名空间都不开放给凭据模式。
// 保留 Header 使用混合大小写输入，验证 ASCII 不区分大小写比较。
// 空 Header 名、空格、冒号和换行分别覆盖 token 的主要非法形态。
// 合法自定义 Header 同时使用连字符和下划线，覆盖允许的 tchar。
// Provider 返回值包含中文也不会在静态校验读取，凭据内容不受本测试限制。
// 失败用例只要求 std::invalid_argument，诊断文案可在不破坏合同下演进。
// 成功用例只要求不抛异常，不把内部解析器结构暴露为测试耦合。
// 全部测试对象按值构造，顺带覆盖配置 variant 的可移动与可复制使用方式。
// 测试不依赖 server_id 的日志表现，避免把诊断层合同混入配置语法层。
// 若未来扩展传输类型，应新增独立测试文件，不在这里跨层验证连接状态机。

// StaticCredentialProvider 提供一个合法、无副作用的凭据实现。
// calls 只用于证明配置静态校验没有越权调用应用 Provider。
class StaticCredentialProvider final : public aiSDK::IMCPHttpCredentialProvider {
   public:
    std::string credentialValue(const aiSDK::MCPHttpCredentialRequestContext&) override {
        ++calls;
        return "测试凭据";
    }

    int calls = 0;
};

// absoluteTestPath 生成跨平台绝对路径，不探测文件系统。
// Windows 使用显式盘符，POSIX 使用根目录，确保 is_absolute 结论稳定。
std::filesystem::path absoluteTestPath(const std::string& leaf) {
#ifdef _WIN32
    return std::filesystem::path("C:/ai-sdk-tests") / leaf;
#else
    return std::filesystem::path("/opt/ai-sdk-tests") / leaf;
#endif
}

// makeValidStdioServerConfig 构造最小但完整的合法 stdio 配置。
// executable 是否真实存在属于 connect 阶段，不属于静态校验职责。
aiSDK::MCPServerConfig makeValidStdioServerConfig() {
    aiSDK::MCPStdioServerConfig stdio;
    stdio.executable = absoluteTestPath("mcp-server");
    stdio.arguments = {"--mode", "stdio"};
    stdio.working_directory = absoluteTestPath("work");
    stdio.environment = {
        {"MCP_TEST_MODE", "offline"}
    };

    aiSDK::MCPServerConfig config;
    config.server_id = "stdio-test-server";
    config.transport = std::move(stdio);
    return config;
}

// makeValidHttpServerConfig 构造默认 HTTPS 配置。
// 公共限制保持默认值，使单项变异只指向被测约束。
aiSDK::MCPServerConfig makeValidHttpServerConfig(const std::string& endpoint = "https://mcp.example.test/v1") {
    aiSDK::MCPStreamableHttpConfig http;
    http.endpoint = endpoint;

    aiSDK::MCPServerConfig config;
    config.server_id = "http-test-server";
    config.transport = std::move(http);
    return config;
}

// expectInvalidCommonLimit 每次重建配置后只改变一个公共限制。
// 这样可验证全部零值边界，又不会让前一项失败掩盖后一项。
void expectInvalidCommonLimit(const std::function<void(aiSDK::MCPCommonLimits&)>& mutate) {
    aiSDK::MCPServerConfig config = makeValidStdioServerConfig();
    mutate(config.limits);
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);
}

// expectInvalidHttpEndpoint 为 URL 失败矩阵统一开启回环开发开关。
// 非回环 HTTP 即使开关已开也应失败，因此该辅助函数不会弱化断言。
void expectInvalidHttpEndpoint(const std::string& endpoint) {
    aiSDK::MCPServerConfig config = makeValidHttpServerConfig(endpoint);
    std::get<aiSDK::MCPStreamableHttpConfig>(config.transport).allow_loopback_http = true;
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);
}

// 所有公开错误码必须返回固定机器名称。
// 非法枚举值走防御性名称，避免调用方收到空指针。
TEST(MCPTypesTest, ReturnsStableErrorCodeNames) {
    constexpr std::array<std::pair<aiSDK::MCPErrorCode, const char*>, 18U> cases = {
        {
         {aiSDK::MCPErrorCode::VersionMismatch, "VersionMismatch"},
         {aiSDK::MCPErrorCode::CapabilityMissing, "CapabilityMissing"},
         {aiSDK::MCPErrorCode::ProtocolViolation, "ProtocolViolation"},
         {aiSDK::MCPErrorCode::RemoteProtocolError, "RemoteProtocolError"},
         {aiSDK::MCPErrorCode::TransportFailure, "TransportFailure"},
         {aiSDK::MCPErrorCode::RequestTimeout, "RequestTimeout"},
         {aiSDK::MCPErrorCode::ServerExited, "ServerExited"},
         {aiSDK::MCPErrorCode::HttpStatusError, "HttpStatusError"},
         {aiSDK::MCPErrorCode::AuthenticationRequired, "AuthenticationRequired"},
         {aiSDK::MCPErrorCode::CredentialUnavailable, "CredentialUnavailable"},
         {aiSDK::MCPErrorCode::ClientBusy, "ClientBusy"},
         {aiSDK::MCPErrorCode::OperationCancelled, "OperationCancelled"},
         {aiSDK::MCPErrorCode::ToolCatalogStale, "ToolCatalogStale"},
         {aiSDK::MCPErrorCode::SessionExpired, "SessionExpired"},
         {aiSDK::MCPErrorCode::OutcomeUnknown, "OutcomeUnknown"},
         {aiSDK::MCPErrorCode::MessageLimitExceeded, "MessageLimitExceeded"},
         {aiSDK::MCPErrorCode::MessageQueueOverflow, "MessageQueueOverflow"},
         {aiSDK::MCPErrorCode::PaginationLimitExceeded, "PaginationLimitExceeded"},
         }
    };

    // 表驱动断言保证新增或重排实现不会改变既有机器字符串。
    for(const auto& entry : cases) {
        EXPECT_STREQ(aiSDK::mcpErrorCodeName(entry.first), entry.second);
    }
    EXPECT_STREQ(aiSDK::mcpErrorCodeName(static_cast<aiSDK::MCPErrorCode>(-1)), "UnknownMCPError");
}

// 普通错误保存分类、中文文本和发生失败时的 Client 状态。
// 普通错误不得自动生成 OutcomeUnknown 专属字段。
TEST(MCPTypesTest, PreservesOrdinaryExceptionStateSnapshot) {
    const aiSDK::MCPException exception(aiSDK::MCPErrorCode::RequestTimeout, aiSDK::MCPClientState::Ready, "请求超时");

    EXPECT_EQ(exception.code(), aiSDK::MCPErrorCode::RequestTimeout);
    EXPECT_EQ(exception.clientStateAtFailure(), aiSDK::MCPClientState::Ready);
    EXPECT_FALSE(exception.causeCode().has_value());
    EXPECT_FALSE(exception.mayHaveExecuted());
    EXPECT_STREQ(exception.what(), "请求超时");
}

// OutcomeUnknown 必须同时携带真实根因和“可能已执行”标记。
// 根因不能递归指向 OutcomeUnknown，其他错误也不能伪造这些字段。
TEST(MCPTypesTest, EnforcesOutcomeUnknownInvariant) {
    const aiSDK::MCPException outcome(aiSDK::MCPErrorCode::OutcomeUnknown, aiSDK::MCPClientState::Faulted,
                                      "工具结果未知", aiSDK::MCPErrorCode::TransportFailure, true);

    ASSERT_TRUE(outcome.causeCode().has_value());
    EXPECT_EQ(*outcome.causeCode(), aiSDK::MCPErrorCode::TransportFailure);
    EXPECT_TRUE(outcome.mayHaveExecuted());
    EXPECT_EQ(outcome.clientStateAtFailure(), aiSDK::MCPClientState::Faulted);

    // 缺失根因、缺失执行标记或递归根因都破坏结果未知语义。
    EXPECT_THROW(aiSDK::MCPException(aiSDK::MCPErrorCode::OutcomeUnknown, aiSDK::MCPClientState::Ready, "非法结果未知"),
                 std::invalid_argument);
    EXPECT_THROW(aiSDK::MCPException(aiSDK::MCPErrorCode::OutcomeUnknown, aiSDK::MCPClientState::Ready, "非法结果未知",
                                     aiSDK::MCPErrorCode::RequestTimeout, false),
                 std::invalid_argument);
    EXPECT_THROW(aiSDK::MCPException(aiSDK::MCPErrorCode::OutcomeUnknown, aiSDK::MCPClientState::Ready, "非法递归根因",
                                     aiSDK::MCPErrorCode::OutcomeUnknown, true),
                 std::invalid_argument);

    // 普通错误不得带 cause 或 may_have_executed，防止上层错误分支漂移。
    EXPECT_THROW(aiSDK::MCPException(aiSDK::MCPErrorCode::TransportFailure, aiSDK::MCPClientState::Ready, "非法根因",
                                     aiSDK::MCPErrorCode::RequestTimeout, false),
                 std::invalid_argument);
    EXPECT_THROW(aiSDK::MCPException(aiSDK::MCPErrorCode::TransportFailure, aiSDK::MCPClientState::Ready, "非法标记",
                                     std::nullopt, true),
                 std::invalid_argument);
}

// 默认 Catalog 没有 Client 私有签发令牌，公开面必须呈现空且无效。
// 测试不能调用私有构造函数，因此不会绕过真实签发边界。
TEST(MCPTypesTest, DefaultCatalogIsInvalidAndEmpty) {
    const aiSDK::MCPToolCatalog catalog;

    EXPECT_FALSE(catalog.valid());
    EXPECT_TRUE(catalog.tools().empty());
    EXPECT_TRUE(catalog.serverId().empty());
    EXPECT_EQ(catalog.revision(), 0U);
}

// 合法 stdio 配置应只做静态校验，不检查不存在的可执行文件。
// 该断言也锁定参数数组和环境映射可以安全通过。
TEST(MCPServerConfigTest, AcceptsValidStdioConfigurationWithoutIo) {
    aiSDK::MCPServerConfig config = makeValidStdioServerConfig();

    EXPECT_NO_THROW(aiSDK::validateMCPServerConfig(config));
}

// 合法 HTTPS、IPv4 回环 HTTP 和 IPv6 回环 HTTP 都应通过。
// Provider 调用次数保持零，证明配置校验未获取秘密。
TEST(MCPServerConfigTest, AcceptsHttpsAndExplicitLiteralLoopbackHttp) {
    aiSDK::MCPServerConfig https = makeValidHttpServerConfig("https://mcp.example.test:443/v1?mode=tools");
    auto provider = std::make_shared<StaticCredentialProvider>();
    std::get<aiSDK::MCPStreamableHttpConfig>(https.transport).credential = aiSDK::MCPBearerCredential{provider};
    EXPECT_NO_THROW(aiSDK::validateMCPServerConfig(https));
    EXPECT_EQ(provider->calls, 0);

    // 整个 127/8 属于 IPv4 回环范围，不只允许 127.0.0.1。
    aiSDK::MCPServerConfig ipv4 = makeValidHttpServerConfig("http://127.42.7.9:8080/mcp");
    std::get<aiSDK::MCPStreamableHttpConfig>(ipv4.transport).allow_loopback_http = true;
    EXPECT_NO_THROW(aiSDK::validateMCPServerConfig(ipv4));

    // IPv6 回环必须使用 URL 方括号形式。
    aiSDK::MCPServerConfig ipv6 = makeValidHttpServerConfig("http://[::1]:8080/mcp");
    std::get<aiSDK::MCPStreamableHttpConfig>(ipv6.transport).allow_loopback_http = true;
    EXPECT_NO_THROW(aiSDK::validateMCPServerConfig(ipv6));
}

// 公共超时和大小限制都要求严格大于零。
// 每一项独立变异，防止第一个失败掩盖未校验字段。
TEST(MCPServerConfigTest, RejectsZeroCommonTimeoutsAndSizes) {
    expectInvalidCommonLimit(
        [](aiSDK::MCPCommonLimits& limits) { limits.request_timeout = std::chrono::milliseconds::zero(); });
    expectInvalidCommonLimit(
        [](aiSDK::MCPCommonLimits& limits) { limits.absolute_request_timeout = std::chrono::milliseconds::zero(); });
    expectInvalidCommonLimit(
        [](aiSDK::MCPCommonLimits& limits) { limits.close_timeout = std::chrono::milliseconds::zero(); });
    expectInvalidCommonLimit([](aiSDK::MCPCommonLimits& limits) { limits.max_message_bytes = 0U; });
    expectInvalidCommonLimit([](aiSDK::MCPCommonLimits& limits) { limits.max_pending_messages = 0U; });
    expectInvalidCommonLimit([](aiSDK::MCPCommonLimits& limits) { limits.max_error_text_bytes = 0U; });
    expectInvalidCommonLimit([](aiSDK::MCPCommonLimits& limits) { limits.max_pages = 0U; });
    expectInvalidCommonLimit([](aiSDK::MCPCommonLimits& limits) { limits.max_tools = 0U; });
}

// stdio 必须使用绝对真实可执行路径，不能把 shell 脚本当作程序。
// 工作目录同样要求绝对路径，避免启动时受当前目录隐式影响。
TEST(MCPServerConfigTest, RejectsRelativeStdioPathsAndShellScripts) {
    aiSDK::MCPServerConfig config = makeValidStdioServerConfig();
    std::get<aiSDK::MCPStdioServerConfig>(config.transport).executable = "relative/mcp-server";
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    config = makeValidStdioServerConfig();
    std::get<aiSDK::MCPStdioServerConfig>(config.transport).working_directory = "relative/work";
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    // 扩展名采用不区分大小写语义，分别覆盖 cmd 与 bat。
    config = makeValidStdioServerConfig();
    std::get<aiSDK::MCPStdioServerConfig>(config.transport).executable = absoluteTestPath("server.CMD");
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    config = makeValidStdioServerConfig();
    std::get<aiSDK::MCPStdioServerConfig>(config.transport).executable = absoluteTestPath("server.bat");
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);
}

// 工作目录与 executable 一样最终进入平台进程 API，不能允许 NUL 静默截断路径。
// 回车和换行会破坏诊断与审计边界，即使宿主文件系统允许也必须在静态阶段拒绝。
// 三个用例均保持绝对路径前缀，证明失败来自控制字符而不是相对路径规则。
TEST(MCPServerConfigTest, RejectsStdioWorkingDirectoryControlBytes) {
    aiSDK::MCPServerConfig config = makeValidStdioServerConfig();
    std::get<aiSDK::MCPStdioServerConfig>(config.transport).working_directory =
        absoluteTestPath(std::string("work\0tail", 9U));
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    config = makeValidStdioServerConfig();
    std::get<aiSDK::MCPStdioServerConfig>(config.transport).working_directory = absoluteTestPath("work\rsegment");
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    config = makeValidStdioServerConfig();
    std::get<aiSDK::MCPStdioServerConfig>(config.transport).working_directory = absoluteTestPath("work\nsegment");
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);
}

// 参数中的 NUL、回车和换行会破坏平台进程参数边界，必须在启动前拒绝。
// 分别重建配置，确保三种控制字节都被独立覆盖。
TEST(MCPServerConfigTest, RejectsStdioArgumentControlBytes) {
    aiSDK::MCPServerConfig config = makeValidStdioServerConfig();
    std::get<aiSDK::MCPStdioServerConfig>(config.transport).arguments = {std::string("abc\0def", 7U)};
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    config = makeValidStdioServerConfig();
    std::get<aiSDK::MCPStdioServerConfig>(config.transport).arguments = {"第一行\n第二行"};
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    config = makeValidStdioServerConfig();
    std::get<aiSDK::MCPStdioServerConfig>(config.transport).arguments = {"第一段\r第二段"};
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);
}

// 环境名称必须非空、不含等号或控制字节，环境值不能含控制字节。
// 这些输入均可能在平台 API 中截断或改变环境块结构。
TEST(MCPServerConfigTest, RejectsInvalidStdioEnvironmentEntries) {
    aiSDK::MCPServerConfig config = makeValidStdioServerConfig();
    auto& environment = std::get<aiSDK::MCPStdioServerConfig>(config.transport).environment;
    environment.clear();
    environment.emplace("", "value");
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    config = makeValidStdioServerConfig();
    std::get<aiSDK::MCPStdioServerConfig>(config.transport).environment = {
        {"BAD=NAME", "value"}
    };
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    config = makeValidStdioServerConfig();
    std::get<aiSDK::MCPStdioServerConfig>(config.transport).environment.clear();
    std::get<aiSDK::MCPStdioServerConfig>(config.transport).environment.emplace(std::string("BAD\0NAME", 8U), "value");
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    config = makeValidStdioServerConfig();
    std::get<aiSDK::MCPStdioServerConfig>(config.transport).environment = {
        {"BAD\nNAME", "value"}
    };
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    config = makeValidStdioServerConfig();
    std::get<aiSDK::MCPStdioServerConfig>(config.transport).environment.clear();
    std::get<aiSDK::MCPStdioServerConfig>(config.transport)
        .environment.emplace("VALID_NAME", std::string("abc\0def", 7U));
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    config = makeValidStdioServerConfig();
    std::get<aiSDK::MCPStdioServerConfig>(config.transport).environment = {
        {"VALID_NAME", "第一行\n第二行"}
    };
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);
}

// 明文 HTTP 必须同时满足显式开关和字面量回环来源。
// localhost、非回环 IPv4 与普通域名都不能依赖 DNS 解析后再决定安全性。
TEST(MCPServerConfigTest, RejectsUnsafePlainHttpOrigins) {
    aiSDK::MCPServerConfig disabled = makeValidHttpServerConfig("http://127.0.0.1:8080/mcp");
    EXPECT_THROW(aiSDK::validateMCPServerConfig(disabled), std::invalid_argument);

    expectInvalidHttpEndpoint("http://localhost:8080/mcp");
    expectInvalidHttpEndpoint("http://192.168.1.10:8080/mcp");
    expectInvalidHttpEndpoint("http://mcp.example.test/mcp");
    expectInvalidHttpEndpoint("http://128.0.0.1/mcp");
}

// Endpoint 禁止空白、控制字符、用户信息与 Fragment，端口必须是 1 到 65535 的十进制数。
// URL 的这些错误必须在任何 HTTP 请求发出前稳定失败，避免后端对畸形地址作宽松解析。
TEST(MCPServerConfigTest, RejectsUserInfoFragmentAndInvalidPorts) {
    expectInvalidHttpEndpoint("https://mcp.example.test/a b");
    expectInvalidHttpEndpoint("https://user:password@mcp.example.test/v1");
    expectInvalidHttpEndpoint("https://mcp.example.test/v1#tools");
    expectInvalidHttpEndpoint("https://mcp.example.test:/v1");
    expectInvalidHttpEndpoint("https://mcp.example.test:0/v1");
    expectInvalidHttpEndpoint("https://mcp.example.test:65536/v1");
    expectInvalidHttpEndpoint("https://mcp.example.test:not-a-port/v1");
    expectInvalidHttpEndpoint("https://mcp.example.test:42949672960/v1");
}

// HTTP 专属超时和容量同样要求正值，凭据截止时间不能越过 close 上限。
// SSE 单事件上限不能大于完整 MCP 消息上限；相等属于合法边界。
TEST(MCPServerConfigTest, EnforcesHttpTimeoutAndSseLimits) {
    aiSDK::MCPServerConfig config = makeValidHttpServerConfig();
    auto& valid_http = std::get<aiSDK::MCPStreamableHttpConfig>(config.transport);
    valid_http.max_sse_event_bytes = config.limits.max_message_bytes;
    EXPECT_NO_THROW(aiSDK::validateMCPServerConfig(config));

    config = makeValidHttpServerConfig();
    std::get<aiSDK::MCPStreamableHttpConfig>(config.transport).max_sse_event_bytes =
        config.limits.max_message_bytes + 1U;
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    config = makeValidHttpServerConfig();
    std::get<aiSDK::MCPStreamableHttpConfig>(config.transport).connect_timeout = std::chrono::milliseconds::zero();
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    config = makeValidHttpServerConfig();
    std::get<aiSDK::MCPStreamableHttpConfig>(config.transport).stream_idle_timeout = std::chrono::milliseconds::zero();
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    config = makeValidHttpServerConfig();
    std::get<aiSDK::MCPStreamableHttpConfig>(config.transport).credential_timeout =
        config.limits.close_timeout + std::chrono::milliseconds(1);
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    config = makeValidHttpServerConfig();
    std::get<aiSDK::MCPStreamableHttpConfig>(config.transport).max_sse_retry_delay = std::chrono::milliseconds::zero();
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    // 各个缓冲上限和重连次数都不能使用零值绕过资源保护。
    config = makeValidHttpServerConfig();
    std::get<aiSDK::MCPStreamableHttpConfig>(config.transport).max_sse_event_bytes = 0U;
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);
    config = makeValidHttpServerConfig();
    std::get<aiSDK::MCPStreamableHttpConfig>(config.transport).max_event_id_bytes = 0U;
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);
    config = makeValidHttpServerConfig();
    std::get<aiSDK::MCPStreamableHttpConfig>(config.transport).max_session_id_bytes = 0U;
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);
    config = makeValidHttpServerConfig();
    std::get<aiSDK::MCPStreamableHttpConfig>(config.transport).max_reconnect_attempts = 0U;
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);
}

// Bearer 与固定 Header 模式都必须持有真实 Provider。
// 空指针在构造期拒绝，不能推迟到首个网络请求才失败。
TEST(MCPServerConfigTest, RejectsNullCredentialProviders) {
    aiSDK::MCPServerConfig config = makeValidHttpServerConfig();
    std::get<aiSDK::MCPStreamableHttpConfig>(config.transport).credential = aiSDK::MCPBearerCredential{};
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);

    config = makeValidHttpServerConfig();
    std::get<aiSDK::MCPStreamableHttpConfig>(config.transport).credential =
        aiSDK::MCPFixedHeaderCredential{"X-Api-Key", nullptr};
    EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);
}

// 固定凭据 Header 不能覆盖鉴权、MCP 会话、正文或代理控制字段。
// 名称比较不区分大小写，proxy- 前缀整体保留。
TEST(MCPServerConfigTest, RejectsReservedCredentialHeaders) {
    const auto provider = std::make_shared<StaticCredentialProvider>();
    constexpr std::array<const char*, 5U> reserved = {
        "Authorization", "mCp-SeSsIoN-iD", "Content-Type", "Host", "Proxy-Custom-Credential",
    };

    for(const char* header_name : reserved) {
        aiSDK::MCPServerConfig config = makeValidHttpServerConfig();
        std::get<aiSDK::MCPStreamableHttpConfig>(config.transport).credential =
            aiSDK::MCPFixedHeaderCredential{header_name, provider};
        EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);
    }
    EXPECT_EQ(provider->calls, 0);
}

// 固定 Header 名必须是非空 HTTP token，不能包含空白、冒号或换行。
// 合法自定义 Header 与合法 Provider 应通过且不触发 Provider。
TEST(MCPServerConfigTest, ValidatesCustomCredentialHeaderToken) {
    const auto provider = std::make_shared<StaticCredentialProvider>();
    constexpr std::array<const char*, 4U> invalid = {"", "X Api Key", "X:Api-Key", "X-Api\nKey"};

    for(const char* header_name : invalid) {
        aiSDK::MCPServerConfig config = makeValidHttpServerConfig();
        std::get<aiSDK::MCPStreamableHttpConfig>(config.transport).credential =
            aiSDK::MCPFixedHeaderCredential{header_name, provider};
        EXPECT_THROW(aiSDK::validateMCPServerConfig(config), std::invalid_argument);
    }

    aiSDK::MCPServerConfig valid = makeValidHttpServerConfig();
    std::get<aiSDK::MCPStreamableHttpConfig>(valid.transport).credential =
        aiSDK::MCPFixedHeaderCredential{"X-MCP-Test_Key", provider};
    EXPECT_NO_THROW(aiSDK::validateMCPServerConfig(valid));
    EXPECT_EQ(provider->calls, 0);
}

// 超时配置允许 milliseconds 的完整正值范围，内部绝对截止时间必须饱和。
// 该边界测试防止直接相加回绕后，把极长超时误判为立即超时。
TEST(MCPDeadlineTest, SaturatesAtSteadyClockUpperBound) {
    const auto deadline = aiSDK::detail::saturatingSteadyDeadlineAfter(std::chrono::milliseconds::max());
    EXPECT_EQ(deadline, std::chrono::steady_clock::time_point::max());
}

}  // namespace
