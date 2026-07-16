#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "AIClient.h"
#include "agent/SimpleAgent.h"

namespace {

// 本测试文件只注入 IModelProvider，不替换 AIClient、ToolRegistry 或 ToolExecutor 的生产实现。
// 因此每个用例既保持离线确定性，也覆盖 Agent 与既有 SDK 公开入口之间的真实接线。
// ScriptedProvider 按请求顺序返回响应，测试可精确控制模型何时请求工具、何时自然结束。
// 请求快照保存在 Provider 内，是验证消息历史、工具可见性和独立 run 的主要观察面。
// 所有模型响应都显式补齐 assistant 消息中的 tool_calls，模拟 OpenAI-compatible 协议的真实形态。
// 不能只设置 ChatResponse::tool_calls，因为 Agent 回填的是 response.message 而不是快捷字段。
// 测试不访问网络、不读取 .env，也不依赖真实 Provider 的响应格式或远端可用性。
// ToolRegistry 处理函数均为内存实现，避免工具副作用让 ReAct 循环测试变成集成环境测试。
// 成功路径验证连续两次 Tool Call 后模型返回普通文本，证明 Agent 不只支持固定的两轮模式。
// 同一测试再执行第二次 run，验证局部 messages 不会跨任务残留成为隐式会话或记忆。
// 工具处理函数失败的测试锁定“失败 Observation 可恢复”，不是把失败转换为 Agent 立即终止。
// 未知工具的测试锁定复用 ToolRegistry 既有失败语义，避免 Agent 产生第二套错误协议。
// Medium 工具测试同时验证请求过滤和执行前拦截，覆盖模型直接臆造未展示工具的场景。
// 测试用处理函数调用计数证明被拒绝的工具没有产生真实本地副作用。
// 熔断测试提供恰好 16 个 Tool Call 响应，验证 Agent 不发起第 17 次模型请求。
// 熔断阈值由生产实现内部常量控制，测试只观察对外的确定性结果而不依赖私有成员。
// 空输入测试确认参数错误在 Provider 之前收敛，避免无效输入产生网络成本或 Trace 噪声。
// 工作区选项测试分别构造有无根目录的 Agent，确保文件能力不是静态链接后的默认权限。
// 有根目录时只检查模型可见的工具定义，不对真实临时目录进行写入，保持该用例纯只读。
// Trace 测试复用调用方创建的会话，验证 Agent 不创建自定义步骤类型或隐式全局 Trace。
// Trace 步骤顺序来自真实 AIClient 和 ToolExecutor，实现变动若破坏连接关系会直接暴露。
// 测试刻意不验证模型私有推理文本，Agent 的公开协议只处理结构化 Tool Call 和最终答案。
// 每个 ProviderInfo 都声明工具调用能力，使测试与 Provider 接口的能力元数据约束一致。
// 流式接口在测试 Provider 中明确抛出，若未来 Agent 误走流式路径会立即形成清晰失败。
// helper 只封装测试数据构造，不复制 Agent 的风险判断、消息回填或循环控制逻辑。
// makeClient 创建真实门面后再注入 Provider，避免用 mock AIClient 跳过核心集成点。
// 注册 Low 工具 helper 保留传入的 JSON 参数，便于未来扩展用例验证参数未被 Agent 改写。
// assertions 优先检查业务结果，再检查请求快照，避免观测细节掩盖对外 Agent 契约。
// 结果文本均为本地固定字符串，不包含真实密钥、路径、用户工作区内容或外部服务数据。
// 本文件与 workspace_file_tools_test 分层：本文件关心编排，后者关心具体文件系统边界。
// 该分层使后续会话管理或 MCP 工具适配可新增测试而不复制文件路径安全用例。
// 测试名称采用行为描述，不依赖实现函数名，便于重构内部循环时仍保持验收意图。
// 单个断言失败后使用 ASSERT 阻止访问不存在的请求或消息，确保失败输出本身可诊断。
// 同一 Provider 实例不跨测试共享，响应游标和请求记录不会使测试顺序产生依赖。
// 本测试不尝试覆盖 Provider 网络失败；那是 Provider 和 Trace 测试的既有职责边界。
// Agent 捕获 std::exception 的外层结果协议由循环中的 Provider、工具和文件组件共享使用。
// ToolMessage 的 tool_call_id 断言确保多工具批次未来扩展时仍可正确关联模型协议。
// 当前所有工具调用按 ToolExecutor 的串行语义执行，本测试不暗示并行 Tool Call 支持。
// 普通 run 不记录 Trace 的事实由 API 文档表达；此处只验证显式 Trace 重载的正向契约。
// 已禁用 Trace 的客户端会降级到既有公开入口，该行为由 Trace 模块测试而非本文件重复覆盖。
// 未来增加取消、超时或预算对象时应独立测试，不应改变本文件固定的 16 次安全熔断验收。
// 测试中的临时工作区根使用系统临时目录，其内容不会通过模型请求读取或列举。
// 工作区工具注册顺序被断言，因为 ToolRegistry 的稳定顺序是模型工具输入的一部分。
// Low 风险策略只看 ToolRiskLevel，测试避免根据工具名称推断允许性，保持策略可扩展。
// 中风险拒绝消息只做子串断言，稳定行为是“不执行并回填失败”，而非绑定所有文案细节。
// 未知工具错误同样做协议语义断言，底层错误文本仍由 ToolRegistry 统一维护。
// 该文件全部使用 GTest 与项目现有 CMake 目标，不引入额外测试框架或运行脚本。
// 每个用例可单独执行，便于本地排查 Agent 循环、Trace 或工具策略的回归。
// 默认系统提示词存在于请求首条消息，独立 run 用例通过消息数量间接锁定这一公共默认值。
// Agent 返回空最终文本仍可表示模型自然结束；测试不把非空文本作为协议强制条件。
// 本文件不测试长期记忆，需求明确首版每次 run 都在结束后释放消息轨迹。
// 本文件不测试 Shell、HTTP、MCP 或 Skill，因为首版 Agent 不注册或执行这些工具来源。
// 文件工具均标记 Low 是调用方已经授予受限根目录后的策略决定，风险细分由后续审批层扩展。
// 用例只检查 Medium；生产 lowRiskTools 对 High 使用相同过滤分支，策略实现没有等级遗漏。
// Trace 测试的步数为短链固定值，若下层新增 Provider 子步骤应改为按父子类型定位而非盲目扩展断言。
// 当前脚本 Provider 未覆盖 Trace 重载，IModelProvider 默认委托普通 chat，恰好验证兼容 Provider 仍可接入。
// 所有输入是简短中文文本，避免测试数据自身影响消息数量或工具调用结构。
// 循环测试不使用睡眠或时间依赖，保证本地执行速度和可重复性。
// 通过这些离线断言，在线示例只需验证真实 API 配置，不承担核心状态机回归职责。
// 最终答案、错误结果和 Trace 被分开断言，调用方无需从日志文本猜测 ReAct 是否已自然结束。
// 无状态 run 的测试也为后续会话管理建立基线：新增记忆必须以显式选项改变这一默认行为。
// workspace 选项测试避免触发文件 handler，证明“注册能力”和“执行副作用”可分别验证。
// Provider 响应数量严格受断言约束，若 Agent 意外重试或提前结束，测试会暴露额外或缺少请求。
// 所有工具名字使用稳定 ASCII 标识，符合 ToolRegistry 和供应商协议的现有约定。
// 测试中的中文最终答案只用于区分脚本响应，不参与任何自然语言语义判断。
// 低风险工具 helper 不依赖系统时间，使多轮测试不会因时间格式或时区改变而抖动。
// 拒绝中风险调用后仍请求模型，锁定策略返回 Observation 而不是吞掉模型响应的设计。
// 熔断失败不携带最后一轮工具结果文本作为最终答案，避免调用方误把未完成任务显示为成功回复。
// Trace 用例只有一项工具调用，确保步骤数量可以准确表达 Agent 对现有公开重载的委托关系。
// 这些断言对未来 Agent API 的扩展是回归边界，而不是要求永远使用当前私有函数结构。
// Provider 异常文本收敛可在后续专项用例覆盖；本文件先锁定正常、恢复与策略控制流。
// 目标构建链接 GTest::gtest_main，与现有 core、tool、trace 测试的入口形式保持一致。
// 测试不要求真实 TraceSession 启用详情脱敏器，避免把文件或模型正文引入测试导出数据。
// 通过同一批脚本响应复用，测试能够在毫秒级运行并适合作为每次本地构建的回归检查。

// ScriptedProvider 在不发起网络请求的前提下返回预设模型响应。
// 它同时保存每次请求快照，使测试可以验证 Agent 的消息回填和工具可见性边界。
class ScriptedProvider final : public aiSDK::IModelProvider {
   public:
    explicit ScriptedProvider(std::vector<aiSDK::ChatResponse> responses) : responses_(std::move(responses)) {}

    aiSDK::ChatResponse chat(const aiSDK::ChatRequest& request) override {
        requests.push_back(request);
        if(next_response_ >= responses_.size()) {
            throw std::runtime_error("测试 Provider 缺少预设响应");
        }
        return responses_[next_response_++];
    }

    void streamChat(const aiSDK::ChatRequest&, aiSDK::StreamCallback) override {
        throw std::logic_error("SimpleAgent 测试不应调用流式接口");
    }

    aiSDK::ProviderInfo info() const override {
        return aiSDK::ProviderInfo{"scripted", "scripted-model", false, true};
    }

    std::vector<aiSDK::ChatRequest> requests;

   private:
    std::vector<aiSDK::ChatResponse> responses_;
    std::size_t next_response_ = 0U;
};

// toolCallResponse 保持 response.tool_calls 与 assistant 消息中的工具调用一致。
// OpenAI-compatible 消息序列需要先回填该 assistant 消息，测试因此不能只填快捷字段。
aiSDK::ChatResponse toolCallResponse(std::string id, std::string name,
                                     nlohmann::json arguments = nlohmann::json::object()) {
    aiSDK::ToolCall call{std::move(id), std::move(name), std::move(arguments), ""};
    aiSDK::ChatResponse response;
    response.message = aiSDK::AssistantMessage("");
    response.message.tool_calls.push_back(call);
    response.tool_calls.push_back(std::move(call));
    return response;
}

// makeClient 创建真实 AIClient 门面，再注入完全离线的测试 Provider。
// 这样 ReAct 测试覆盖 SDK 现有公开调用边界，而不复制 AIClient 的工具执行逻辑。
aiSDK::AIClient makeClient(const std::shared_ptr<ScriptedProvider>& provider, bool enable_trace = false) {
    aiSDK::Config config;
    config.enable_trace = enable_trace;
    aiSDK::AIClient client(config);
    client.setProvider(provider);
    return client;
}

// registerLowTool 为测试注册最小可执行低风险工具。
// 返回值含名称和原始参数，便于确认 Agent 没有改写模型提供的 JSON 参数。
void registerLowTool(aiSDK::AIClient& client, const std::string& name) {
    client.tools().registerTool(
        aiSDK::Tool{
            name, "测试低风险工具", nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}},
            aiSDK::ToolRiskLevel::Low
    },
        [name](const nlohmann::json& arguments) {
            return aiSDK::ToolResult::successResult({
                {"name",      name     },
                {"arguments", arguments}
            });
        });
}

// 首个主路径测试覆盖两次工具循环、最终自然结束和同一实例的独立 run。
// 第二次 run 的首个请求只允许包含新的系统与用户消息，不能携带第一次任务历史。
TEST(SimpleAgentTest, CompletesMultipleToolRoundsAndKeepsRunsIndependent) {
    const auto provider = std::make_shared<ScriptedProvider>(std::vector<aiSDK::ChatResponse>{
        toolCallResponse("call-first", "echo", {{"round", 1}}
          ),
        toolCallResponse("call-second", "echo", {{"round", 2}}
          ),
        aiSDK::assistantTextResponse("第一项任务完成"),
        aiSDK::assistantTextResponse("第二项任务完成"),
    });
    aiSDK::AIClient client = makeClient(provider);
    registerLowTool(client, "echo");
    aiSDK::SimpleAgent agent(client);

    const aiSDK::AgentResult first = agent.run("先完成第一项任务");
    ASSERT_TRUE(first.success);
    EXPECT_EQ(first.final_answer, "第一项任务完成");
    ASSERT_EQ(provider->requests.size(), 3U);
    // 第二轮必须带上一轮 assistant Tool Call 与绑定调用 ID 的工具结果。
    ASSERT_EQ(provider->requests[1].messages.size(), 4U);
    EXPECT_EQ(provider->requests[1].messages[3].role, aiSDK::Role::Tool);
    ASSERT_TRUE(provider->requests[1].messages[3].tool_call_id.has_value());
    EXPECT_EQ(*provider->requests[1].messages[3].tool_call_id, "call-first");

    const aiSDK::AgentResult second = agent.run("再完成第二项任务");
    ASSERT_TRUE(second.success);
    EXPECT_EQ(second.final_answer, "第二项任务完成");
    ASSERT_EQ(provider->requests.size(), 4U);
    // SimpleAgent 不保存会话，新的 run 只能从当前输入重新开始。
    ASSERT_EQ(provider->requests[3].messages.size(), 2U);
    EXPECT_EQ(provider->requests[3].messages[1].role, aiSDK::Role::User);
    EXPECT_EQ(provider->requests[3].messages[1].content, "再完成第二项任务");
    ASSERT_EQ(provider->requests[3].tools.size(), 1U);
    EXPECT_EQ(provider->requests[3].tools.front().name, "echo");
}

// 工具失败不是 Agent 的终止条件；失败结果应作为 Tool 消息回传，让模型选择修复或解释。
TEST(SimpleAgentTest, ReturnsToolFailureToModelAndAllowsRecovery) {
    const auto provider = std::make_shared<ScriptedProvider>(std::vector<aiSDK::ChatResponse>{
        toolCallResponse("call-broken", "broken"),
        aiSDK::assistantTextResponse("我已根据工具失败调整方案"),
    });
    aiSDK::AIClient client = makeClient(provider);
    client.tools().registerTool(
        aiSDK::Tool{
            "broken", "会失败的测试工具", nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}},
            aiSDK::ToolRiskLevel::Low
    },
        [](const nlohmann::json&) { return aiSDK::ToolResult::errorResult("可恢复的测试失败"); });
    aiSDK::SimpleAgent agent(client);

    const aiSDK::AgentResult result = agent.run("请尝试失败工具");
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.final_answer, "我已根据工具失败调整方案");
    ASSERT_EQ(provider->requests.size(), 2U);
    ASSERT_EQ(provider->requests[1].messages.size(), 4U);
    EXPECT_EQ(provider->requests[1].messages.back().role, aiSDK::Role::Tool);
    EXPECT_EQ(provider->requests[1].messages.back().tool_call_id, "call-broken");
    EXPECT_NE(provider->requests[1].messages.back().content.find("可恢复的测试失败"), std::string::npos);
}

// 模型可能返回未注册的工具名称；该情况应复用 ToolRegistry 的失败结果并让模型继续收尾。
TEST(SimpleAgentTest, ReturnsUnknownToolFailureToModelAndAllowsRecovery) {
    const auto provider = std::make_shared<ScriptedProvider>(std::vector<aiSDK::ChatResponse>{
        toolCallResponse("call-missing", "missing_tool"),
        aiSDK::assistantTextResponse("未知工具已被说明"),
    });
    aiSDK::AIClient client = makeClient(provider);
    aiSDK::SimpleAgent agent(client);

    const aiSDK::AgentResult result = agent.run("尝试不存在的工具");
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.final_answer, "未知工具已被说明");
    ASSERT_EQ(provider->requests.size(), 2U);
    EXPECT_EQ(provider->requests[1].messages.back().role, aiSDK::Role::Tool);
    EXPECT_NE(provider->requests[1].messages.back().content.find("工具不存在"), std::string::npos);
}

// Medium 工具即使被模型直接臆造，也不能越过 Agent 的可见性筛选和执行前风险拦截。
TEST(SimpleAgentTest, HidesAndRejectsNonLowRiskTools) {
    const auto provider = std::make_shared<ScriptedProvider>(std::vector<aiSDK::ChatResponse>{
        toolCallResponse("call-medium", "medium_tool"),
        aiSDK::assistantTextResponse("已停止高风险操作"),
    });
    aiSDK::AIClient client = makeClient(provider);
    std::size_t executions = 0U;
    client.tools().registerTool(
        aiSDK::Tool{
            "medium_tool", "中风险测试工具",
            nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}},
            aiSDK::ToolRiskLevel::Medium
    },
        [&executions](const nlohmann::json&) {
            ++executions;
            return aiSDK::ToolResult::successResult(nullptr);
        });
    aiSDK::SimpleAgent agent(client);

    const aiSDK::AgentResult result = agent.run("执行中风险工具");
    ASSERT_TRUE(result.success);
    EXPECT_EQ(executions, 0U);
    ASSERT_EQ(provider->requests.size(), 2U);
    EXPECT_TRUE(provider->requests.front().tools.empty());
    EXPECT_EQ(provider->requests[1].messages.back().role, aiSDK::Role::Tool);
    EXPECT_NE(provider->requests[1].messages.back().content.find("拒绝执行非低风险工具"), std::string::npos);
}

// 连续 16 次工具调用后，Agent 必须停止，而不是把模型输出转化为无限请求和副作用。
TEST(SimpleAgentTest, StopsAtInternalSafetyFuse) {
    std::vector<aiSDK::ChatResponse> responses;
    for(std::size_t index = 0U; index < 16U; ++index) {
        responses.push_back(toolCallResponse("call-" + std::to_string(index), "noop"));
    }
    const auto provider = std::make_shared<ScriptedProvider>(std::move(responses));
    aiSDK::AIClient client = makeClient(provider);
    registerLowTool(client, "noop");
    aiSDK::SimpleAgent agent(client);

    const aiSDK::AgentResult result = agent.run("请持续调用工具");
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.final_answer.empty());
    EXPECT_NE(result.error_message.find("安全熔断"), std::string::npos);
    EXPECT_EQ(provider->requests.size(), 16U);
}

// 空输入在发起模型请求前失败，避免把没有语义的任务送入 Provider。
TEST(SimpleAgentTest, RejectsEmptyInputBeforeCallingProvider) {
    const auto provider = std::make_shared<ScriptedProvider>(std::vector<aiSDK::ChatResponse>{});
    aiSDK::AIClient client = makeClient(provider);
    aiSDK::SimpleAgent agent(client);

    const aiSDK::AgentResult result = agent.run("");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_message, "Agent 输入不能为空");
    EXPECT_TRUE(provider->requests.empty());
}

// 文件工具只能由构造选项显式开启，默认 Agent 不会因链接到 SDK 就获得文件访问能力。
TEST(SimpleAgentTest, RegistersWorkspaceFileToolsOnlyWithExplicitRoot) {
    const auto no_workspace_provider = std::make_shared<ScriptedProvider>(
        std::vector<aiSDK::ChatResponse>{aiSDK::assistantTextResponse("无文件工具")});
    aiSDK::AIClient no_workspace_client = makeClient(no_workspace_provider);
    aiSDK::SimpleAgent no_workspace_agent(no_workspace_client);
    ASSERT_TRUE(no_workspace_agent.run("不授权工作区").success);
    ASSERT_EQ(no_workspace_provider->requests.size(), 1U);
    EXPECT_TRUE(no_workspace_provider->requests.front().tools.empty());

    const auto workspace_provider = std::make_shared<ScriptedProvider>(
        std::vector<aiSDK::ChatResponse>{aiSDK::assistantTextResponse("已授权工作区")});
    aiSDK::AIClient workspace_client = makeClient(workspace_provider);
    aiSDK::SimpleAgentOptions options;
    options.workspace_file_tools = aiSDK::WorkspaceFileToolOptions{std::filesystem::temp_directory_path()};
    aiSDK::SimpleAgent workspace_agent(workspace_client, std::move(options));
    ASSERT_TRUE(workspace_agent.run("授权临时工作区").success);
    ASSERT_EQ(workspace_provider->requests.size(), 1U);
    const std::vector<aiSDK::Tool>& tools = workspace_provider->requests.front().tools;
    ASSERT_EQ(tools.size(), 7U);
    EXPECT_EQ(tools[0].name, "list_directory");
    EXPECT_EQ(tools[1].name, "read_text_file");
    EXPECT_EQ(tools[2].name, "create_text_file");
    EXPECT_EQ(tools[3].name, "write_text_file");
    EXPECT_EQ(tools[4].name, "replace_text_in_file");
    EXPECT_EQ(tools[5].name, "find_files");
    EXPECT_EQ(tools[6].name, "search_text");
}

// 显式 Trace 重载应把两次模型调用和一批工具执行写入同一调用方会话。
// 测试不要求 Agent 创造新的步骤类型，只锁定其复用 AIClient 现有 Trace 契约。
TEST(SimpleAgentTest, ReusesExplicitTraceSessionForEntireLoop) {
    const auto provider = std::make_shared<ScriptedProvider>(std::vector<aiSDK::ChatResponse>{
        toolCallResponse("call-trace", "echo"),
        aiSDK::assistantTextResponse("链路完成"),
    });
    aiSDK::AIClient client = makeClient(provider, true);
    registerLowTool(client, "echo");
    aiSDK::SimpleAgent agent(client);
    aiSDK::TraceSession session = client.startTrace();

    const aiSDK::AgentResult result = agent.run("记录链路", session);
    ASSERT_TRUE(result.success);
    const aiSDK::Trace trace = session.snapshot();
    ASSERT_EQ(trace.steps.size(), 4U);
    EXPECT_EQ(trace.steps[0].type, aiSDK::TraceStepType::ModelRequest);
    EXPECT_EQ(trace.steps[1].type, aiSDK::TraceStepType::ToolBatch);
    EXPECT_EQ(trace.steps[2].type, aiSDK::TraceStepType::ToolExecution);
    EXPECT_EQ(trace.steps[3].type, aiSDK::TraceStepType::ModelRequest);
}

}  // namespace
