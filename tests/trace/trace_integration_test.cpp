#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "AIClient.h"
#include "http/HttpClient.h"
#include "provider/DeepSeekProvider.h"

namespace {

// 本文件验证 AIClient、Provider、HTTP、SSE 与 ToolExecutor 的真实接线。
// 唯一替换点是最底层 IHttpTransport，避免真实外网使错误分支不可重复。
// Provider 请求构造、鉴权、响应解析和 Trace 步骤仍使用生产实现。
// 脚本响应分别覆盖同步成功、非 2xx、流式成功、事件错误和回调异常。
// 每个敏感层都有独立哨兵，最终导出扫描能发现跨层泄露。
// 步骤层级断言关注直接父节点，不依赖隐式全局当前 Trace。
// SSE 只验证流级汇总，禁止逐 token 创建步骤或保存内容。
// HTTP 只验证状态和字节元数据，不保存 URL、头或正文。
// 工具链只验证批次与子步骤，不让 SDK 自动形成 Agent Loop。
// 脱敏器异常同时覆盖同步和流式公开入口的业务隔离。
// 全部测试均可在本地重复运行，不依赖 API Key、CI 或人工服务。
// 在线 Provider 测试继续作为独立补充，不承担 Trace 验收职责。
// ScriptedTransport 为 Provider/HTTP Trace 提供完全本地的确定性响应。
// 它不解释请求内容，也不记录鉴权头，测试只控制状态码、正文、分块和异常。
// 传输调用计数用于证明空回调与旧入口是否真正触达网络边界。
// 纯传输异常在首个分块前发生，不应创建误导性的 SSE 子步骤。
// 回调异常与传输异常使用不同固定错误码，便于消费端准确归因。
// SSE 缺少 Done 只影响流协议步骤，不改变现有公开函数返回契约。
// 流式兄弟步骤按实际启动排序，父子断言不依赖隐式当前上下文。
// 默认详情关闭时，任何请求正文、响应正文和异常文本都不得进入导出。
// 脱敏器失败使用结构化 status/value 槽位，不保存异常诊断原文。
// 总开关关闭时，即使外部会话有效也必须走无 Trace 业务路径。
// 旧无 Trace 重载分别覆盖同步与流式入口，防止兼容路径退化为空操作。
// 空回调仍允许门面和 Provider 记录成功，但不得创建 HTTP 或 SSE 步骤。
// 非 2xx 与抛出的传输异常分别验证，避免把状态失败和连接失败混为一类。
// 流式非 2xx 还会独立验证 SSE Error 事实，避免责任分类互相覆盖。
// 纯流式传输失败发生在首字节前，不允许生成孤立或虚假的 SSE 步骤。
// error_code 来自 TraceFailure 固定映射，测试不从 error_summary 反向解析状态。
// findSingleStep 只用于每种类型唯一的短链，完整链仍显式检查全部层级。
// 所有异常哨兵只用于最终字符串扫描，断言消息不会主动输出敏感内容。
// 这些测试不替换 Provider 或解析器，因此能覆盖真实跨层数据与控制流。
// 脚本传输只隔离外网依赖，不改变 HttpClient 对回调和异常的包装逻辑。
// 每个边界先断言业务结果，再检查 Trace，确保旁路能力不主导主流程。
// 新增 Provider 时可复用同样测试结构，但不能复用 DeepSeek 专属事件假设。
// 本文件不测试随机 ID 和并发锁，相关契约由 TraceRecorderTest 单独覆盖。
// 测试过程不依赖睡眠、端口或环境变量，保证本地重复执行稳定。
// Provider 配置中的密钥与地址只进入真实请求构造，不进入 Trace 属性。
// 模型名是允许元数据，测试使用固定非敏感值验证白名单行为。
// 工具名可观测但调用 ID 不可观测，完整链哨兵覆盖两者边界。
// HTTP response_bytes 与事件计数只记录规模，不记录对应负载内容。
// 用户回调始终收到原始业务事件，Trace 不替换、过滤或重排事件。
// 缺少 Done 和 Error 事件采用不同失败枚举，避免消费端猜测计数语义。
// 传输对象由 shared_ptr 保持到断言结束，调用计数读取不存在悬空引用。
// 所有会话由公开 startTrace 创建，不直接构造内部共享状态绕过配置。
class ScriptedTransport final : public aiSDK::IHttpTransport {
   public:
    aiSDK::HttpResponse chat_response;
    aiSDK::HttpResponse stream_response;
    std::vector<std::string> stream_chunks;
    bool throw_chat = false;
    bool throw_stream = false;
    mutable std::size_t chat_calls = 0;
    mutable std::size_t stream_calls = 0;

    aiSDK::HttpResponse postJson(const std::string&, const nlohmann::json&, const aiSDK::HttpHeaders&, int) const override {
        ++chat_calls;
        if(throw_chat) {
            throw std::runtime_error("传输异常秘密");
        }
        return chat_response;
    }

    aiSDK::HttpResponse postJsonStream(const std::string&, const nlohmann::json&, const aiSDK::HttpHeaders&, int,
                                       aiSDK::HttpStreamCallback callback) const override {
        ++stream_calls;
        if(throw_stream) {
            throw std::runtime_error("流式传输异常秘密");
        }
        // 分块顺序与脚本一致，用户回调异常原样穿透，便于验证现有传播契约。
        if(callback) {
            for(const auto& chunk : stream_chunks) {
                callback(chunk);
            }
        }
        return stream_response;
    }
};

// traceConfig 只开启 Trace；Provider 随后通过实例重载注入。
// 这样 AIClient 公开入口也能在无网络环境下覆盖成功与失败分支。
aiSDK::Config traceConfig(bool enabled = true) {
    aiSDK::Config config;
    config.enable_trace = enabled;
    return config;
}

// findSingleStep 用于错误分支按职责定位步骤，避免把断言绑定到兄弟步骤的启动先后。
// 调用方需先保证目标类型在当前短链中只出现一次；缺失时返回 nullptr 供 ASSERT 检查。
const aiSDK::TraceStep* findSingleStep(const aiSDK::Trace& trace, aiSDK::TraceStepType type) {
    for(const auto& step : trace.steps) {
        if(step.type == type) {
            return &step;
        }
    }
    return nullptr;
}

// validChatBody 生成 DeepSeek/OpenAI-compatible 的最小成功响应。
// marker 故意包含敏感哨兵文本，用于证明默认 Trace 不保存响应正文。
std::string validChatBody(const std::string& marker) {
    return nlohmann::json{
        {"choices", {{{"message", {{"content", marker}}}}}                               },
        {"usage",   {{"prompt_tokens", 3}, {"completion_tokens", 5}, {"total_tokens", 8}}},
    }
        .dump();
}

// attachDeepSeekProvider 把真实 Provider 逻辑与脚本传输组合后注入门面。
// API Key、基础地址和响应正文均带哨兵，任何泄露都可由字符串断言发现。
void attachDeepSeekProvider(aiSDK::AIClient& client, const std::shared_ptr<ScriptedTransport>& transport) {
    aiSDK::ProviderConfig provider_config;
    provider_config.api_key = "测试密钥-不应记录";
    provider_config.base_url = "https://fake.local/秘密路径";
    provider_config.default_model = "trace-test-model";

    aiSDK::HttpClient http_client(transport);
    client.setProvider(std::make_shared<aiSDK::DeepSeekProvider>(provider_config, 250, std::move(http_client)));
}

// registerLocalTool 添加完整链路中间的离线工具步骤。
// 工具参数和结果同样含哨兵，默认 Trace 只能保存工具名与成功计数。
void registerLocalTool(aiSDK::AIClient& client) {
    const aiSDK::Tool tool{
        "echo_safe",
        "返回固定结构",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}},
        aiSDK::ToolRiskLevel::Low,
    };
    client.tools().registerTool(tool, [](const nlohmann::json&) {
        return aiSDK::ToolResult::successResult({
            {"secret_output", "工具输出秘密"}
        });
    });
}

// 一个显式会话跨越模型请求、工具执行和后续模型请求。
// 三个公开操作都是根步骤，只有各自内部层级形成父子关系。
// 脚本传输返回两次相同成功响应，整个用例不访问真实网络。
// AIClient 使用公开 Provider 实例注入，仍经过真实 Provider 与 HttpClient 逻辑。
// 第一轮 chat 生成门面、Provider 和 HTTP 三层步骤。
// 中间 executeToolCalls 生成批次根步骤和单工具子步骤。
// 第二轮 chat 再生成独立的三层根链，不挂到前一工具或模型步骤。
// 八个步骤的精确顺序来自开始序号，而不是函数完成时机。
// 每个嵌套层只引用直接父 ID，避免跨层跳跃或隐式最近步骤。
// 所有业务结果都先断言，确保 Trace 没有改变聊天和工具返回。
// API Key、URL、用户消息、模型正文、工具输入与输出分别使用哨兵。
// 最终 JSON 扫描全部哨兵，覆盖默认白名单的跨层安全边界。
// Provider 和 Model 只存在于模型步骤属性，不放在 Trace 顶层。
// 工具名称属于允许元数据，但 call.id 和 raw_arguments 默认不保存。
// 本用例代表调用方显式组织 LLM-Tool-LLM，而不是 SDK 内部 Agent Loop。
// 后续增加其他 Provider 时，也必须保持相同根步骤与嵌套层级语义。
TEST(TraceIntegrationTest, RecordsFullExplicitChainWithStableHierarchy) {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->chat_response = {200, validChatBody("模型响应秘密")};

    aiSDK::AIClient client(traceConfig());
    attachDeepSeekProvider(client, transport);
    registerLocalTool(client);
    aiSDK::TraceSession session = client.startTrace();

    aiSDK::ChatRequest request;
    request.messages.push_back(aiSDK::UserMessage("用户消息秘密"));
    const aiSDK::ChatResponse first_response = client.chat(request, session);
    const auto tool_results = client.executeToolCalls(
        {
            {"call_full", "echo_safe", nlohmann::json{{"secret_input", "工具输入秘密"}}, "原始工具参数秘密"}
    },
        session);
    const aiSDK::ChatResponse second_response = client.chat(request, session);

    EXPECT_EQ(first_response.content, "模型响应秘密");
    ASSERT_EQ(tool_results.size(), 1U);
    EXPECT_TRUE(tool_results.front().result.success);
    EXPECT_EQ(second_response.content, "模型响应秘密");

    // 开始顺序固定为：模型/Provider/HTTP、工具批次/工具、模型/Provider/HTTP。
    const aiSDK::Trace trace = session.snapshot();
    ASSERT_EQ(trace.steps.size(), 8U);
    EXPECT_EQ(trace.steps[0].type, aiSDK::TraceStepType::ModelRequest);
    EXPECT_EQ(trace.steps[1].type, aiSDK::TraceStepType::ProviderRequest);
    EXPECT_EQ(trace.steps[2].type, aiSDK::TraceStepType::HttpRequest);
    EXPECT_EQ(trace.steps[3].type, aiSDK::TraceStepType::ToolBatch);
    EXPECT_EQ(trace.steps[4].type, aiSDK::TraceStepType::ToolExecution);
    EXPECT_EQ(trace.steps[5].type, aiSDK::TraceStepType::ModelRequest);
    EXPECT_EQ(trace.steps[6].type, aiSDK::TraceStepType::ProviderRequest);
    EXPECT_EQ(trace.steps[7].type, aiSDK::TraceStepType::HttpRequest);

    // 跨公开操作不猜测父节点，嵌套步骤只引用当前操作的显式父 ID。
    EXPECT_FALSE(trace.steps[0].parent_step_id.has_value());
    EXPECT_FALSE(trace.steps[3].parent_step_id.has_value());
    EXPECT_FALSE(trace.steps[5].parent_step_id.has_value());
    EXPECT_EQ(*trace.steps[1].parent_step_id, trace.steps[0].step_id);
    EXPECT_EQ(*trace.steps[2].parent_step_id, trace.steps[1].step_id);
    EXPECT_EQ(*trace.steps[4].parent_step_id, trace.steps[3].step_id);
    EXPECT_EQ(*trace.steps[6].parent_step_id, trace.steps[5].step_id);
    EXPECT_EQ(*trace.steps[7].parent_step_id, trace.steps[6].step_id);
    for(const auto& step : trace.steps) {
        EXPECT_EQ(step.status, aiSDK::TraceStepStatus::Success);
    }

    // 默认元数据白名单不得包含任何请求、鉴权、URL、响应或工具详情。
    const std::string exported = session.toJson().dump();
    EXPECT_EQ(exported.find("测试密钥-不应记录"), std::string::npos);
    EXPECT_EQ(exported.find("fake.local"), std::string::npos);
    EXPECT_EQ(exported.find("用户消息秘密"), std::string::npos);
    EXPECT_EQ(exported.find("模型响应秘密"), std::string::npos);
    EXPECT_EQ(exported.find("工具输入秘密"), std::string::npos);
    EXPECT_EQ(exported.find("原始工具参数秘密"), std::string::npos);
    EXPECT_EQ(exported.find("工具输出秘密"), std::string::npos);
}

// 流式成功路径只记录事件计数，HTTP 与 SSE 是 Provider 下的兄弟步骤。
// 逐块文本和最终完整正文都不得进入默认 Trace。
// 第一块产生一个 Delta，第二块产生一个 Done，事件数量应精确为二。
// ScriptedTransport 按原顺序调用 HttpClient 包装后的字节回调。
// DeepSeekProvider 先完成完整 SSE 边界缓冲，再调用用户事件回调。
// 用户回调收到真实增量内容，证明 Trace 没有替换或清空业务事件。
// AIClient 根步骤覆盖整个流式公开调用生命周期。
// Provider 步骤同时拥有 HTTP 与 SSE 两个直接子步骤。
// HTTP 先启动，首个分块到达时再创建 SSE，避免纯传输失败被误归因到解析层。
// 四个步骤均成功，Done 是流正常结束的必要事实。
// event_count 与 done_count 属于固定数字元数据，可以默认保存。
// delta 文本属于用户内容，即使只出现一个 token 也禁止默认保存。
// 请求消息和 API Key 使用独立哨兵，防止只检查响应方向。
// HTTP response_bytes 可以保存长度，但不能保存对应正文。
// 测试不依赖分块大小等网络实现细节，只依赖脚本输入协议。
// 若新增工具增量类型，应只增加汇总计数而不是逐事件步骤。
TEST(TraceIntegrationTest, SummarizesSuccessfulStreamWithoutTokenContent) {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->stream_chunks = {
        "data: {\"choices\":[{\"delta\":{\"content\":\"流式内容秘密\"}}]}\n\n",
        "data: [DONE]\n\n",
    };
    transport->stream_response = {200, transport->stream_chunks[0] + transport->stream_chunks[1]};

    aiSDK::AIClient client(traceConfig());
    attachDeepSeekProvider(client, transport);
    aiSDK::TraceSession session = client.startTrace();
    aiSDK::ChatRequest request;
    request.messages.push_back(aiSDK::UserMessage("流式请求秘密"));
    std::vector<aiSDK::StreamEvent> events;

    client.streamChat(
        request, [&](const aiSDK::StreamEvent& event) { events.push_back(event); }, session);

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].type, aiSDK::StreamEventType::Delta);
    EXPECT_EQ(events[1].type, aiSDK::StreamEventType::Done);
    const aiSDK::Trace trace = session.snapshot();
    ASSERT_EQ(trace.steps.size(), 4U);
    EXPECT_EQ(trace.steps[0].type, aiSDK::TraceStepType::ModelRequest);
    EXPECT_EQ(trace.steps[1].type, aiSDK::TraceStepType::ProviderRequest);
    EXPECT_EQ(trace.steps[2].type, aiSDK::TraceStepType::HttpRequest);
    EXPECT_EQ(trace.steps[3].type, aiSDK::TraceStepType::SseStream);
    EXPECT_EQ(*trace.steps[2].parent_step_id, trace.steps[1].step_id);
    EXPECT_EQ(*trace.steps[3].parent_step_id, trace.steps[1].step_id);
    EXPECT_EQ(trace.steps[3].attributes.at("event_count"), 2U);
    EXPECT_EQ(trace.steps[3].attributes.at("done_count"), 1U);
    for(const auto& step : trace.steps) {
        EXPECT_EQ(step.status, aiSDK::TraceStepStatus::Success);
    }

    const std::string exported = session.toJson().dump();
    EXPECT_EQ(exported.find("流式内容秘密"), std::string::npos);
    EXPECT_EQ(exported.find("流式请求秘密"), std::string::npos);
    EXPECT_EQ(exported.find("测试密钥-不应记录"), std::string::npos);
}

// SSE Error 事件属于流级错误事实，但现有回调接口不会因此抛异常。
// 因此只把 SSE 子步骤标记错误，Provider 和公开调用仍按正常返回完成。
// 错误事件后仍发送 Done，用于区分“观察到错误”与“流不完整”。
// 用户回调必须收到 Error 事件，Trace 不能把事件转成额外异常。
// AIClient 根步骤成功表示公开函数按原契约正常返回。
// Provider 步骤成功表示协议处理与回调派发正常完成。
// SSE 子步骤错误专门表达流内容携带远端错误事实。
// HTTP 步骤仍成功，因为状态码为 200 且传输没有失败。
// error_count 会记录数量，但 error_message 原文禁止进入属性或摘要。
// 固定摘要允许定位 SSE 层，又不会复制服务端返回的敏感正文。
// 两个事件均计入流级统计，Done 不会覆盖之前的 Error 状态。
// 本用例防止接入层为了“统一失败”错误地新增 throw。
// 同样也防止 Provider/AIClient 被错误标记为失败而误导调用方。
// Trace 状态层级因此能同时表达传输成功和协议内容错误。
// 哨兵扫描覆盖 SSEParser 生成的 StreamEvent::error_message。
// 若未来改变 SSE 错误策略，必须先显式修改公开回调契约。
TEST(TraceIntegrationTest, MarksSseErrorEventWithoutChangingCallbackContract) {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->stream_chunks = {
        "data: {\"error\":{\"message\":\"服务端流式错误秘密\"}}\n\n",
        "data: [DONE]\n\n",
    };
    transport->stream_response = {200, transport->stream_chunks[0] + transport->stream_chunks[1]};

    aiSDK::AIClient client(traceConfig());
    attachDeepSeekProvider(client, transport);
    aiSDK::TraceSession session = client.startTrace();
    aiSDK::ChatRequest request;
    std::vector<aiSDK::StreamEvent> events;
    EXPECT_NO_THROW(client.streamChat(
        request, [&](const aiSDK::StreamEvent& event) { events.push_back(event); }, session));

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events.front().type, aiSDK::StreamEventType::Error);
    const aiSDK::Trace trace = session.snapshot();
    ASSERT_EQ(trace.steps.size(), 4U);
    EXPECT_EQ(trace.steps[0].status, aiSDK::TraceStepStatus::Success);
    EXPECT_EQ(trace.steps[1].status, aiSDK::TraceStepStatus::Success);
    EXPECT_EQ(trace.steps[2].status, aiSDK::TraceStepStatus::Success);
    EXPECT_EQ(trace.steps[3].status, aiSDK::TraceStepStatus::Error);
    EXPECT_EQ(trace.steps[3].failure, aiSDK::TraceFailure::SseErrorEvent);
    EXPECT_EQ(trace.steps[3].error_summary, "SSE 返回错误事件");
    EXPECT_EQ(session.toJson().dump().find("服务端流式错误秘密"), std::string::npos);
}

// 用户回调异常必须原样传播，并让模型、Provider、SSE 与 HTTP 步骤全部结束为错误。
// Trace 只记录固定摘要，不能复制回调异常或流式内容。
// 回调在收到第一个 Delta 时抛出带唯一哨兵的 runtime_error。
// ScriptedTransport 不捕获该异常，模拟传输层恢复原始回调异常后的效果。
// HttpClient 捕获只用于结束 Trace 步骤，然后必须重新抛出同一异常。
// Provider 与 AIClient 继续逐层结束各自在途 Scope，也不能替换异常对象。
// try/catch 明确比较 what()，防止错误地变成通用传输异常。
// 四个步骤都必须从 Running 收敛为 Error，不能留下半完成状态。
// HTTP 错误码明确区分下游回调失败与纯传输失败，不写回调异常字符串。
// SSE 错误摘要只说明流处理失败，不写 Delta 文本。
// Provider 与模型根步骤同样使用各层固定安全摘要。
// 返回正文不会用于成功解析，因为异常优先终止当前公开调用。
// 脚本传输只锁定 HttpClient 外层的传播契约，不替代 cpr 回调边界的源码审查。
// 哨兵扫描同时检查异常文本和回调前收到的内容。
// Trace 失败记录本身必须 noexcept，否则会在异常展开时终止进程。
// 若未来增加重试，原始回调异常仍不能被自动重试或吞掉。
TEST(TraceIntegrationTest, PreservesStreamingCallbackException) {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->stream_chunks = {
        "data: {\"choices\":[{\"delta\":{\"content\":\"回调前内容秘密\"}}]}\n\n",
    };
    transport->stream_response = {200, transport->stream_chunks.front()};

    aiSDK::AIClient client(traceConfig());
    attachDeepSeekProvider(client, transport);
    aiSDK::TraceSession session = client.startTrace();
    aiSDK::ChatRequest request;

    try {
        client.streamChat(
            request, [](const aiSDK::StreamEvent&) { throw std::runtime_error("调用方回调异常秘密"); }, session);
        FAIL() << "预期回调异常被原样传播";
    } catch(const std::runtime_error& exception) {
        EXPECT_STREQ(exception.what(), "调用方回调异常秘密");
    }

    const aiSDK::Trace trace = session.snapshot();
    ASSERT_EQ(trace.steps.size(), 4U);
    for(const auto& step : trace.steps) {
        EXPECT_EQ(step.status, aiSDK::TraceStepStatus::Error);
    }
    const aiSDK::TraceStep* http_step = findSingleStep(trace, aiSDK::TraceStepType::HttpRequest);
    const aiSDK::TraceStep* sse_step = findSingleStep(trace, aiSDK::TraceStepType::SseStream);
    ASSERT_NE(http_step, nullptr);
    ASSERT_NE(sse_step, nullptr);
    EXPECT_EQ(http_step->failure, aiSDK::TraceFailure::StreamCallbackFailed);
    EXPECT_EQ(sse_step->failure, aiSDK::TraceFailure::SseProcessingFailed);
    const std::string exported = session.toJson().dump();
    EXPECT_EQ(exported.find("调用方回调异常秘密"), std::string::npos);
    EXPECT_EQ(exported.find("回调前内容秘密"), std::string::npos);
}

// 非 2xx 保持原有 Provider 异常，同时 HTTP、Provider 和门面步骤均记录错误。
// 服务端正文只用于原异常构造，不能进入 Trace 的安全错误摘要。
// 脚本返回 429 和可解析错误对象，覆盖 Provider 的结构化错误提取路径。
// HttpClient 按原契约返回非 2xx，而不是在传输层提前抛出。
// HTTP Trace 在返回前记录 status_code 并标记该层错误。
// Provider 随后通过 ensureSuccessStatus 构造原有 runtime_error。
// AIClient 只透传异常并关闭根步骤，不能改变异常类型。
// 三个步骤均为 Error，消费者可以从层级判断失败发生在 HTTP 状态。
// http_status_code 是安全白名单字段，可用于聚合限流或服务端故障。
// 远端错误正文可能包含用户数据，因此绝不进入安全摘要。
// URL 可能含查询参数或租户路径，也不作为默认属性保存。
// API Key 只存在于传给脚本传输的头部，不应进入任何 Trace 值。
// 哨兵扫描以最终 JSON 为准，覆盖属性、详情和错误摘要全部字段。
// 该用例不要求检查原异常全文，避免把敏感服务端正文写入测试日志。
// 若未来增加错误分类，应保持现有固定摘要仍不含原文。
// 非 2xx 与传输异常是不同事实，不能合并为无状态码的同一分支。
TEST(TraceIntegrationTest, RecordsHttpFailureWithoutRemoteErrorBody) {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->chat_response = {429, R"({"error":{"message":"远端错误正文秘密"}})"};

    aiSDK::AIClient client(traceConfig());
    attachDeepSeekProvider(client, transport);
    aiSDK::TraceSession session = client.startTrace();
    aiSDK::ChatRequest request;
    EXPECT_THROW(client.chat(request, session), std::runtime_error);

    const aiSDK::Trace trace = session.snapshot();
    ASSERT_EQ(trace.steps.size(), 3U);
    EXPECT_EQ(trace.steps[0].status, aiSDK::TraceStepStatus::Error);
    EXPECT_EQ(trace.steps[1].status, aiSDK::TraceStepStatus::Error);
    EXPECT_EQ(trace.steps[2].status, aiSDK::TraceStepStatus::Error);
    EXPECT_EQ(trace.steps[2].attributes.at("http_status_code"), 429);
    EXPECT_EQ(trace.steps[2].error_summary, "HTTP 返回非成功状态");
    const std::string exported = session.toJson().dump();
    EXPECT_EQ(exported.find("远端错误正文秘密"), std::string::npos);
    EXPECT_EQ(exported.find("fake.local"), std::string::npos);
}

// 流式非 2xx 同时包含 SSE Error 事件时，HTTP 与 SSE 必须各自记录职责事实。
// ScriptedTransport 先派发一个完整错误事件，再返回 429 状态码。
// 用户回调仍按既有流协议收到 Error，随后 Provider 按状态码抛出异常。
// HTTP 步骤使用 http_status_failed，不能误归类为连接或回调失败。
// SSE 步骤使用 sse_error_event，不能被 Provider 异常覆盖成处理失败。
// Provider 与模型根步骤均结束为 Error，原 runtime_error 继续向上传播。
// 远端错误正文只存在于业务事件和异常，不进入默认 Trace 导出。
// 本用例锁定 http_status_failed 分支，防止流式错误责任边界回归。
// HTTP 在首个分块前已经创建，SSE 在分块到达时作为 Provider 的兄弟步骤创建。
// 两个子步骤的开始顺序不影响各自失败码，消费端无需猜测主因覆盖规则。
// error_count 只保存数量，不允许保存 StreamEvent::error_message。
// 429 仍由 Provider 现有 ensureSuccessStatus 转成业务异常，Trace 不新增异常类型。
// 响应缺少 Done 不再成为主分类，因为明确的 SSE Error 事件优先级更高。
TEST(TraceIntegrationTest, SeparatesStreamingHttpStatusAndSseErrorFacts) {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->stream_chunks = {
        "data: {\"error\":{\"message\":\"流式状态错误秘密\"}}\n\n",
    };
    transport->stream_response = {429, transport->stream_chunks.front()};

    aiSDK::AIClient client(traceConfig());
    attachDeepSeekProvider(client, transport);
    aiSDK::TraceSession session = client.startTrace();
    std::vector<aiSDK::StreamEvent> events;

    EXPECT_THROW(client.streamChat(
                     aiSDK::ChatRequest{}, [&](const aiSDK::StreamEvent& event) { events.push_back(event); }, session),
                 std::runtime_error);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, aiSDK::StreamEventType::Error);

    const aiSDK::Trace trace = session.snapshot();
    ASSERT_EQ(trace.steps.size(), 4U);
    const aiSDK::TraceStep* http_step = findSingleStep(trace, aiSDK::TraceStepType::HttpRequest);
    const aiSDK::TraceStep* sse_step = findSingleStep(trace, aiSDK::TraceStepType::SseStream);
    ASSERT_NE(http_step, nullptr);
    ASSERT_NE(sse_step, nullptr);
    EXPECT_EQ(http_step->failure, aiSDK::TraceFailure::HttpStatusFailed);
    EXPECT_EQ(http_step->attributes.at("http_status_code"), 429);
    EXPECT_EQ(sse_step->failure, aiSDK::TraceFailure::SseErrorEvent);
    EXPECT_EQ(sse_step->attributes.at("error_count"), 1U);
    EXPECT_EQ(trace.steps[0].status, aiSDK::TraceStepStatus::Error);
    EXPECT_EQ(trace.steps[1].status, aiSDK::TraceStepStatus::Error);
    EXPECT_EQ(session.toJson().dump().find("流式状态错误秘密"), std::string::npos);
}

// 空回调沿用原接口的空操作契约，不得进入传输层。
// 门面与 Provider 仍可记录一次成功调用，并用白名单布尔值表达回调缺失。
// 调用计数是唯一网络副作用证据，比仅检查返回值更能发现误发请求。
// 本用例同时保证空回调不会创建 HTTP 或 SSE 伪步骤。
TEST(TraceIntegrationTest, EmptyStreamingCallbackDoesNotStartTransport) {
    auto transport = std::make_shared<ScriptedTransport>();
    aiSDK::AIClient client(traceConfig());
    attachDeepSeekProvider(client, transport);
    aiSDK::TraceSession session = client.startTrace();

    EXPECT_NO_THROW(client.streamChat(aiSDK::ChatRequest{}, aiSDK::StreamCallback{}, session));
    EXPECT_EQ(transport->stream_calls, 0U);

    const aiSDK::Trace trace = session.snapshot();
    ASSERT_EQ(trace.steps.size(), 2U);
    EXPECT_EQ(trace.steps[0].type, aiSDK::TraceStepType::ModelRequest);
    EXPECT_EQ(trace.steps[1].type, aiSDK::TraceStepType::ProviderRequest);
    EXPECT_EQ(trace.steps[0].attributes.at("callback_present"), false);
    EXPECT_EQ(trace.steps[1].attributes.at("callback_present"), false);
    EXPECT_EQ(trace.steps[0].status, aiSDK::TraceStepStatus::Success);
    EXPECT_EQ(trace.steps[1].status, aiSDK::TraceStepStatus::Success);
}

// 同步和流式传输异常都必须原样传播，并让已启动步骤全部收敛为 Error。
// 两条链分别使用独立会话，便于精确断言错误码与传输调用次数。
// 流式异常发生在任何字节到达前，因此不应伪造已经处理 SSE 的成功事实。
// 异常哨兵不得进入 Trace，错误摘要只能来自 TraceFailure 固定映射。
TEST(TraceIntegrationTest, RecordsSynchronousAndStreamingTransportFailures) {
    auto chat_transport = std::make_shared<ScriptedTransport>();
    chat_transport->throw_chat = true;
    aiSDK::AIClient chat_client(traceConfig());
    attachDeepSeekProvider(chat_client, chat_transport);
    aiSDK::TraceSession chat_session = chat_client.startTrace();

    EXPECT_THROW(chat_client.chat(aiSDK::ChatRequest{}, chat_session), std::runtime_error);
    EXPECT_EQ(chat_transport->chat_calls, 1U);
    const aiSDK::Trace chat_trace = chat_session.snapshot();
    const aiSDK::TraceStep* chat_http = findSingleStep(chat_trace, aiSDK::TraceStepType::HttpRequest);
    ASSERT_NE(chat_http, nullptr);
    EXPECT_EQ(chat_http->status, aiSDK::TraceStepStatus::Error);
    EXPECT_EQ(chat_http->failure, aiSDK::TraceFailure::HttpTransportFailed);
    for(const auto& step : chat_trace.steps) {
        EXPECT_EQ(step.status, aiSDK::TraceStepStatus::Error);
    }
    EXPECT_EQ(chat_session.toJson().dump().find("传输异常秘密"), std::string::npos);

    auto stream_transport = std::make_shared<ScriptedTransport>();
    stream_transport->throw_stream = true;
    aiSDK::AIClient stream_client(traceConfig());
    attachDeepSeekProvider(stream_client, stream_transport);
    aiSDK::TraceSession stream_session = stream_client.startTrace();
    EXPECT_THROW(stream_client.streamChat(
                     aiSDK::ChatRequest{}, [](const aiSDK::StreamEvent&) {}, stream_session),
                 std::runtime_error);
    EXPECT_EQ(stream_transport->stream_calls, 1U);

    const aiSDK::Trace stream_trace = stream_session.snapshot();
    const aiSDK::TraceStep* stream_http = findSingleStep(stream_trace, aiSDK::TraceStepType::HttpRequest);
    ASSERT_NE(stream_http, nullptr);
    EXPECT_EQ(stream_http->status, aiSDK::TraceStepStatus::Error);
    EXPECT_EQ(stream_http->failure, aiSDK::TraceFailure::HttpStreamFailed);
    for(const auto& step : stream_trace.steps) {
        EXPECT_EQ(step.status, aiSDK::TraceStepStatus::Error);
    }
    EXPECT_EQ(stream_session.toJson().dump().find("流式传输异常秘密"), std::string::npos);
}

// 已收到增量但未观察到 Done 时，业务回调仍按旧契约正常返回。
// SSE 子步骤单独标记流不完整，HTTP、Provider 和门面步骤保持成功。
// 增量内容只交给回调，Trace 仅保存计数与固定错误码。
// 该边界用于区分协议不完整与底层传输抛异常。
TEST(TraceIntegrationTest, MarksStreamWithoutDoneAsIncomplete) {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->stream_chunks = {
        "data: {\"choices\":[{\"delta\":{\"content\":\"缺失结束标记秘密\"}}]}\n\n",
    };
    transport->stream_response = {200, transport->stream_chunks.front()};
    aiSDK::AIClient client(traceConfig());
    attachDeepSeekProvider(client, transport);
    aiSDK::TraceSession session = client.startTrace();
    std::vector<aiSDK::StreamEvent> events;

    EXPECT_NO_THROW(client.streamChat(
        aiSDK::ChatRequest{}, [&](const aiSDK::StreamEvent& event) { events.push_back(event); }, session));
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, aiSDK::StreamEventType::Delta);

    const aiSDK::Trace trace = session.snapshot();
    const aiSDK::TraceStep* sse_step = findSingleStep(trace, aiSDK::TraceStepType::SseStream);
    ASSERT_NE(sse_step, nullptr);
    EXPECT_EQ(sse_step->status, aiSDK::TraceStepStatus::Error);
    EXPECT_EQ(sse_step->failure, aiSDK::TraceFailure::SseIncomplete);
    EXPECT_EQ(sse_step->attributes.at("done_count"), 0U);
    for(const auto& step : trace.steps) {
        if(step.type != aiSDK::TraceStepType::SseStream) {
            EXPECT_EQ(step.status, aiSDK::TraceStepStatus::Success);
        }
    }
    EXPECT_EQ(session.toJson().dump().find("缺失结束标记秘密"), std::string::npos);
}

// 原有无 Trace 重载必须继续执行真实 Provider/HTTP/SSE 业务路径。
// 随后把外部有效会话传给总开关关闭的客户端，验证显式重载也无法绕过配置。
// 调用计数覆盖同步与流式两条入口，外部会话始终保持零步骤。
// 两轮业务结果都正常返回，证明禁用只移除旁路记录而不改变主流程。
TEST(TraceIntegrationTest, PreservesLegacyPathsAndEnforcesTraceMasterSwitch) {
    auto legacy_transport = std::make_shared<ScriptedTransport>();
    legacy_transport->chat_response = {200, validChatBody("旧入口响应")};
    legacy_transport->stream_chunks = {"data: [DONE]\n\n"};
    legacy_transport->stream_response = {200, legacy_transport->stream_chunks.front()};
    aiSDK::AIClient legacy_client(traceConfig());
    attachDeepSeekProvider(legacy_client, legacy_transport);

    EXPECT_EQ(legacy_client.chat(aiSDK::ChatRequest{}).content, "旧入口响应");
    std::vector<aiSDK::StreamEvent> legacy_events;
    legacy_client.streamChat(aiSDK::ChatRequest{}, [&](const aiSDK::StreamEvent& event) { legacy_events.push_back(event); });
    ASSERT_EQ(legacy_events.size(), 1U);
    EXPECT_EQ(legacy_events.front().type, aiSDK::StreamEventType::Done);
    EXPECT_EQ(legacy_transport->chat_calls, 1U);
    EXPECT_EQ(legacy_transport->stream_calls, 1U);

    aiSDK::AIClient trace_owner(traceConfig());
    aiSDK::TraceSession external_session = trace_owner.startTrace();
    auto disabled_transport = std::make_shared<ScriptedTransport>();
    disabled_transport->chat_response = {200, validChatBody("关闭总开关响应")};
    disabled_transport->stream_chunks = {"data: [DONE]\n\n"};
    disabled_transport->stream_response = {200, disabled_transport->stream_chunks.front()};
    aiSDK::AIClient disabled_client(traceConfig(false));
    attachDeepSeekProvider(disabled_client, disabled_transport);

    EXPECT_EQ(disabled_client.chat(aiSDK::ChatRequest{}, external_session).content, "关闭总开关响应");
    std::vector<aiSDK::StreamEvent> disabled_events;
    disabled_client.streamChat(
        aiSDK::ChatRequest{}, [&](const aiSDK::StreamEvent& event) { disabled_events.push_back(event); }, external_session);
    ASSERT_EQ(disabled_events.size(), 1U);
    EXPECT_EQ(disabled_transport->chat_calls, 1U);
    EXPECT_EQ(disabled_transport->stream_calls, 1U);
    EXPECT_TRUE(external_session.snapshot().steps.empty());
}

// 模型请求与响应详情必须把完整统一模型 JSON 交给脱敏器，
// 但 Trace 只能保存脱敏器明确返回的顶层对象。
// 本用例与工具详情测试合在一起覆盖四类原始输入协议。
// request 分支检查消息正文和统一请求字段确实可供调用方筛选。
// response 分支检查正文、原始响应和 usage 确实来自最终 ChatResponse。
// 两次回调的 operation_name 都必须使用当前 Provider 名称，不能误用模型名。
// 脱敏结果只保留消息数量与总 Token 数，原始业务内容仍不得进入导出。
// 业务 ChatResponse 必须保留真实正文，证明脱敏只影响旁路数据。
// Provider 与 HTTP 子步骤不接收模型详情，调用次数应严格等于二。
// 该测试使用真实 AIClient 和 DeepSeekProvider，仅替换最底层传输。
TEST(TraceIntegrationTest, RecordsSanitizedModelRequestAndResponseWithProviderContext) {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->chat_response = {200, validChatBody("模型脱敏响应秘密")};

    aiSDK::AIClient client(traceConfig());
    attachDeepSeekProvider(client, transport);
    int sanitizer_calls = 0;
    bool saw_request = false;
    bool saw_response = false;
    aiSDK::TraceOptions options;
    options.detail_sanitizer = [&](const aiSDK::TraceDetailContext& context, const nlohmann::json& raw) {
        ++sanitizer_calls;
        EXPECT_EQ(context.operation_name, "deepseek");
        if(context.kind == aiSDK::TraceDetailKind::ModelRequest) {
            saw_request = true;
            EXPECT_EQ(raw.at("messages").at(0).at("content"), "模型脱敏请求秘密");
            EXPECT_EQ(raw.at("max_tokens"), 321);
            return nlohmann::json{
                {"message_count", raw.at("messages").size()}
            };
        }
        if(context.kind == aiSDK::TraceDetailKind::ModelResponse) {
            saw_response = true;
            EXPECT_EQ(raw.at("content"), "模型脱敏响应秘密");
            EXPECT_EQ(raw.at("raw_response"), transport->chat_response.body);
            EXPECT_EQ(raw.at("usage").at("total_tokens"), 8);
            return nlohmann::json{
                {"total_tokens", raw.at("usage").at("total_tokens")}
            };
        }
        ADD_FAILURE() << "模型详情脱敏器收到了未约定的数据类别";
        return nlohmann::json::object();
    };

    aiSDK::TraceSession session = client.startTrace(options);
    aiSDK::ChatRequest request;
    request.messages.push_back(aiSDK::UserMessage("模型脱敏请求秘密"));
    request.max_tokens = 321;
    const aiSDK::ChatResponse response = client.chat(request, session);

    EXPECT_EQ(response.content, "模型脱敏响应秘密");
    EXPECT_EQ(sanitizer_calls, 2);
    EXPECT_TRUE(saw_request);
    EXPECT_TRUE(saw_response);

    // 请求详情在 Provider 调用前生成，响应详情只在完整解析成功后生成。
    // 两类详情都属于公开模型根步骤，不能复制到 Provider 或 HTTP 子步骤。
    // recorded 状态表示 value 完全来自脱敏器返回对象，而非 SDK 自动摘取。
    // request.value 只保留消息数量，不应混入消息数组或采样参数。
    // response.value 只保留总 Token 数，不应混入 content 或 raw_response。
    // 三步链仍保持模型、Provider、HTTP 的原始层级与成功状态。
    // 读取 snapshot 后再检查详情，避免测试依赖 JSON 文本键顺序。
    // 数值断言同时锁定脱敏返回对象在 Trace 状态中的值语义副本。
    // 如果未来增加模型详情类别，必须为新类别定义独立槽位和输入协议。
    // 如果 Provider 解析失败，则不得伪造本用例所验证的响应详情。
    // 本段断言关注结构化详情，敏感原文隔离由后续整体导出扫描兜底。
    // 子步骤的 details 保持空对象，证明详情授权没有跨职责层扩散。
    const aiSDK::Trace trace = session.snapshot();
    ASSERT_EQ(trace.steps.size(), 3U);
    const aiSDK::TraceStep& model_step = trace.steps.front();
    EXPECT_EQ(model_step.details.at("request").at("status"), "recorded");
    EXPECT_EQ(model_step.details.at("request").at("value").at("message_count"), 1U);
    EXPECT_EQ(model_step.details.at("response").at("status"), "recorded");
    EXPECT_EQ(model_step.details.at("response").at("value").at("total_tokens"), 8);
    EXPECT_TRUE(trace.steps[1].details.empty());
    EXPECT_TRUE(trace.steps[2].details.empty());

    // 最终导出只允许出现脱敏后的计数，四类原始敏感来源仍遵守默认禁止规则。
    const std::string exported = session.toJson().dump();
    EXPECT_EQ(exported.find("模型脱敏请求秘密"), std::string::npos);
    EXPECT_EQ(exported.find("模型脱敏响应秘密"), std::string::npos);
    EXPECT_EQ(exported.find("测试密钥-不应记录"), std::string::npos);
    EXPECT_EQ(exported.find("fake.local"), std::string::npos);
}

// 模型详情脱敏器异常由 Trace 层隔离，同步返回和流式回调均保持成功。
// 诊断标记写入根步骤，不泄露 sanitizer 的异常文本或原始模型数据。
// 同一个会话先执行同步 chat，再执行流式 chat，覆盖脱敏器多次复用。
// 同步请求会触发 ModelRequest 与 ModelResponse 两个详情阶段。
// 流式请求当前只触发 ModelRequest 阶段，不逐事件调用脱敏器。
// 脱敏器每次都抛异常，业务操作仍必须完整进入 Provider 与 HTTP。
// 同步 ChatResponse 保留真实正文，证明异常没有替换返回值。
// 流式回调仍收到 Done，证明异常没有跳过或中断回调链。
// 根步骤 details 为每个阶段保存固定诊断对象。
// Provider、HTTP 和 SSE 子步骤不复制根步骤的原始详情。
// 诊断对象不包含异常 what()，避免调用方脱敏逻辑再次泄密。
// 最终 JSON 同时扫描脱敏器异常、请求消息和响应正文哨兵。
// 步骤数量下限覆盖同步三步加流式四步，不依赖未来安全元数据扩展。
// 详情失败不会把成功步骤状态改成 Error，因为业务契约仍成功。
// TraceSession 的同一 options 在会话创建后保持只读。
// 并发调用时闭包同步由调用方负责，本例只验证异常隔离。
TEST(TraceIntegrationTest, IsolatesSanitizerFailureFromChatAndStream) {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->chat_response = {200, validChatBody("脱敏聊天响应秘密")};
    transport->stream_chunks = {"data: [DONE]\n\n"};
    transport->stream_response = {200, transport->stream_chunks.front()};

    aiSDK::AIClient client(traceConfig());
    attachDeepSeekProvider(client, transport);
    aiSDK::TraceOptions options;
    options.detail_sanitizer = [](const aiSDK::TraceDetailContext&, const nlohmann::json&) -> nlohmann::json {
        throw std::runtime_error("模型脱敏器异常秘密");
    };
    aiSDK::TraceSession session = client.startTrace(options);
    aiSDK::ChatRequest request;
    request.messages.push_back(aiSDK::UserMessage("脱敏请求秘密"));

    const aiSDK::ChatResponse response = client.chat(request, session);
    EXPECT_EQ(response.content, "脱敏聊天响应秘密");
    std::vector<aiSDK::StreamEvent> events;
    EXPECT_NO_THROW(client.streamChat(
        request, [&](const aiSDK::StreamEvent& event) { events.push_back(event); }, session));
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, aiSDK::StreamEventType::Done);

    const aiSDK::Trace trace = session.snapshot();
    ASSERT_GE(trace.steps.size(), 7U);
    EXPECT_EQ(trace.steps[0].details.at("request").at("status"), "sanitizer_failed");
    EXPECT_TRUE(trace.steps[0].details.at("request").at("value").empty());
    EXPECT_EQ(trace.steps[0].details.at("response").at("status"), "sanitizer_failed");
    EXPECT_TRUE(trace.steps[0].details.at("response").at("value").empty());
    EXPECT_EQ(trace.steps[3].details.at("request").at("status"), "sanitizer_failed");
    EXPECT_TRUE(trace.steps[3].details.at("request").at("value").empty());
    const std::string exported = session.toJson().dump();
    EXPECT_EQ(exported.find("模型脱敏器异常秘密"), std::string::npos);
    EXPECT_EQ(exported.find("脱敏请求秘密"), std::string::npos);
    EXPECT_EQ(exported.find("脱敏聊天响应秘密"), std::string::npos);
}

}  // namespace
