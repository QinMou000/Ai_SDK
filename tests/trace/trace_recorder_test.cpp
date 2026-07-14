#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "AIClient.h"
#include "trace/TraceRecorder.h"

namespace {

// 本文件验证 Trace 核心状态机，不依赖具体 Provider 或 HTTP 实现。
// 每个测试都使用真实 AIClient::startTrace 创建会话，避免绕过总开关。
// 工具执行只承担离线业务载体，不把 ToolRegistry 并发安全纳入 Trace 承诺。
// 敏感哨兵使用唯一中文文本，最终 JSON 扫描可以定位意外原文复制。
// 耗时只断言非负，开始时间验证固定 UTC 形状，不依赖机器速度。
// 并发断言以最终值为准，不依赖线程调度的具体交错顺序。
// 所有 ID 断言关注公开格式和样本唯一性，不假设随机引擎内部实现。
// 详情测试分别覆盖空回调、正常对象、异常和非对象四种策略。
// RAII 测试保证异常离开时不会遗留 Running 状态。
// 快照测试保证调用方得到的是隔离副本而非内部引用。
// JSON 测试保证固定字段与空值表达稳定。
// 属性测试同时约束键与值类型，复杂对象不能绕过详情脱敏通道。
// 错误码测试保证成功和 RAII 失败均使用稳定机器值。
// 跨层步骤与 HTTP/SSE 行为由同目录集成测试单独负责。
// traceConfig 只控制 Trace 总开关，Provider 保持默认空配置。
// 这些测试均离线运行，不会依赖 API Key 或远端服务。
// 属性写入只使用 TraceAttributeKey，测试不会引入白名单外自由键。
// 详情槽位使用 TraceDetailSlot，防止测试本身绕过公开安全边界。
// 详情状态以 status/value 结构断言，不依赖可能变化的诊断文案。
// 失败断言同时检查 TraceFailure 与导出的 error_code 机器码。
// 成功步骤的 error_code 固定为 none，便于消费端统一解析。
// UTC 时间戳验证完整毫秒格式和 Z 后缀，不接受本地时区文本。
// 并发读取会检查每个中间快照，而不只检查写入完成后的最终值。
// 脱敏器重入覆盖完成状态与详情提交之间的生命周期竞争窗口。
// 工具失败覆盖未知名称和处理函数异常两种注册表收敛路径。
// 空批次覆盖零输入时的批次统计，不允许省略可观测根步骤。
// 标准输出与错误输出仅在关闭总开关边界捕获，避免干扰其他测试。
// 所有捕获都在断言前结束，测试失败也不会污染后续用例的输出状态。
// 会话快照按值返回，旧快照在后续追加步骤后仍必须保持不变。
// RAII 遗留步骤使用固定 step_abandoned 机器码收敛，不保留 Running。
// ID 唯一性只在有限样本内验证，不把随机实现细节写入公开契约。
// 父节点始终通过 stepId 显式传递，不使用线程局部或全局当前步骤。
// duration_ms 来自单调时钟，只要求非负，不通过睡眠制造特定耗时。
// 脱敏器返回数组时必须被拒绝，避免任意形状覆盖 details 槽位协议。
// 脱敏器异常与返回类型错误使用不同状态，但都保持 value 为空对象。
// 工具原始 arguments 与 raw_arguments 使用不同哨兵，防止来源混淆。
// 并发写入不使用自定义属性键，确保压力测试也遵守元数据白名单。
// 工作线程不调用 GTest 宏，所有异常和结构错误通过原子标志汇总。
// 起跑屏障使用原子变量而非固定延时，避免机器负载造成偶发失败。
// 读取迭代次数必须大于零，确保并发测试不会在未观察状态时通过。
// 每个中间快照的 sequence 都严格递增，验证追加顺序与可见性一致。
// Scope 移动语义由生产接入间接覆盖，本文件不跨线程移动同一 Scope。
// Trace 总开关以当前客户端配置为准，不信任会话来源客户端的状态。
// 测试工具全部离线同步执行，不引入网络、文件或系统环境副作用。
// 最终 JSON 扫描覆盖 attributes、details、摘要及未来新增固定字段。
// 公开枚举用于构造输入，字符串仅用于检查稳定的 JSON 输出协议。
aiSDK::Config traceConfig(bool enabled) {
    aiSDK::Config config;
    config.enable_trace = enabled;
    return config;
}

// makeLocalTool 构造无网络副作用的本地工具。
// 参数 Schema 保持最小对象形式，测试重点放在 Trace 而非业务校验。
aiSDK::Tool makeLocalTool(const std::string& name) {
    return aiSDK::Tool{
        name,
        "用于 Trace 测试的本地工具",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}},
        aiSDK::ToolRiskLevel::Low,
    };
}

// 关闭总开关必须同时禁用会话创建和显式 Trace 重载，
// 即使传入另一个客户端创建的有效会话也不能绕过当前客户端配置。
// 前置条件：客户端自身 enable_trace 为 false，工具处理函数可以离线成功。
// 输入同时覆盖本客户端创建的禁用会话和其他客户端创建的有效会话。
// 核心结果仍以 ToolExecutionResult 为准，Trace 只能作为独立旁路输出。
// 禁用句柄不得产生 32 个十六进制字符的 ID，否则关闭路径仍会访问随机源和分配状态。
// 禁用句柄快照必须是可安全读取的空值，而不是抛出生命周期异常。
// 传入 options 不应触发脱敏器，因为根本没有允许写入的步骤。
// 第一次工具执行证明 Trace 重载没有破坏原有成功数据。
// 第二次工具执行证明总开关判断属于当前 AIClient，而不是会话来源。
// 外部会话保持零步骤，说明当前客户端没有写入任何隐藏诊断。
// JSON 固定结构让上层无需在关闭分支改用另一套序列化逻辑。
// 本用例不访问 Provider，因此失败不能被网络环境或 API Key 掩盖。
// 若新增 Trace 入口，必须继续把同样的总开关语义扩展到该入口。
// 该边界同时验证关闭 Trace 不会产生任何 SDK 标准输出。
TEST(TraceRecorderTest, DisabledTraceKeepsToolExecutionContract) {
    aiSDK::AIClient disabled_client(traceConfig(false));
    int sanitizer_calls = 0;
    aiSDK::TraceOptions options;
    options.detail_sanitizer = [&](const aiSDK::TraceDetailContext&, const nlohmann::json&) {
        ++sanitizer_calls;
        return nlohmann::json::object();
    };

    // startTrace 在关闭状态下不生成 ID 或共享状态。
    aiSDK::TraceSession disabled_session = disabled_client.startTrace(options);
    EXPECT_FALSE(disabled_session.enabled());
    EXPECT_TRUE(disabled_session.traceId().empty());
    EXPECT_TRUE(disabled_session.snapshot().steps.empty());

    disabled_client.tools().registerTool(makeLocalTool("offline"), [](const nlohmann::json&) {
        return aiSDK::ToolResult::successResult({
            {"value", 42}
        });
    });
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    const std::vector<aiSDK::ToolExecutionResult> results = disabled_client.executeToolCalls(
        {
            {"call_disabled", "offline", nlohmann::json::object(), "{}"}
    },
        disabled_session);
    const std::string captured_stdout = testing::internal::GetCapturedStdout();
    const std::string captured_stderr = testing::internal::GetCapturedStderr();

    // Trace 关闭不能改变工具返回值，也不能调用脱敏器。
    ASSERT_EQ(results.size(), 1U);
    EXPECT_TRUE(results.front().result.success);
    EXPECT_EQ(results.front().result.data.at("value"), 42);
    EXPECT_EQ(sanitizer_calls, 0);
    EXPECT_TRUE(captured_stdout.empty());
    EXPECT_TRUE(captured_stderr.empty());

    // 外部有效会话同样受当前客户端总开关约束。
    aiSDK::AIClient enabled_client(traceConfig(true));
    aiSDK::TraceSession external_session = enabled_client.startTrace();
    const auto repeated_results = disabled_client.executeToolCalls(
        {
            {"call_external", "offline", nlohmann::json::object(), "{}"}
    },
        external_session);
    ASSERT_EQ(repeated_results.size(), 1U);
    EXPECT_TRUE(repeated_results.front().result.success);
    EXPECT_TRUE(external_session.snapshot().steps.empty());

    // 禁用会话仍导出固定 JSON 结构，方便调用方统一处理。
    const nlohmann::json disabled_json = disabled_session.toJson();
    EXPECT_EQ(disabled_json.at("trace_id"), "");
    EXPECT_TRUE(disabled_json.at("steps").empty());
}

// 会话和步骤 ID 使用固定长度小写十六进制格式，
// 多次创建必须互不重复且父子关系只能来自显式传入的步骤 ID。
// 会话 ID 采用 32 个十六进制字符，步骤 ID 采用 16 个十六进制字符，二者用途不能混淆。
// 测试不要求密码学强度，只验证公开形状、非空和样本内不重复。
// 64 个会话足以发现固定值、未播种引擎或错误复用同一状态的问题。
// 父步骤先创建但后完成，序号仍必须反映开始顺序而非完成顺序。
// 子步骤只接收 parent.stepId()，Recorder 不应读取任何隐式当前步骤。
// 根步骤的 parent_step_id 必须没有值，不能用自身或上一会话步骤填充。
// 两个步骤在同一 TraceSession 中共享 trace_id，但 step_id 必须独立。
// sequence 从 1 开始，便于按 sequence-1 定位只追加容器中的元素。
// Scope 完成顺序故意与开始顺序相反，用于防止结束时重新排序。
// 正则只允许小写十六进制，排除前缀、连字符和大写输出漂移。
// 有效 ID 不能是禁用会话使用的空字符串。
// 父子关系断言同时约束 JSON 导出前的公开值模型。
// 若未来接入外部分布式上下文，仍必须保持本地 step_id 的稳定格式。
TEST(TraceRecorderTest, GeneratesUniqueIdsAndExplicitParentRelationship) {
    aiSDK::AIClient client(traceConfig(true));
    const std::regex trace_id_pattern("^[0-9a-f]{32}$");
    const std::regex step_id_pattern("^[0-9a-f]{16}$");
    std::set<std::string> trace_ids;

    // 多次创建用于验证格式和低成本唯一性保障。
    for(int index = 0; index < 64; ++index) {
        aiSDK::TraceSession session = client.startTrace();
        EXPECT_TRUE(std::regex_match(session.traceId(), trace_id_pattern));
        trace_ids.insert(session.traceId());
    }
    EXPECT_EQ(trace_ids.size(), 64U);

    aiSDK::TraceSession session = client.startTrace();
    aiSDK::TraceRecorder recorder(session);
    aiSDK::TraceStepScope parent = recorder.startStep(aiSDK::TraceStepType::ModelRequest);
    aiSDK::TraceStepScope child = recorder.startStep(aiSDK::TraceStepType::ProviderRequest, parent.stepId());
    child.succeed();
    parent.succeed();

    const aiSDK::Trace trace = session.snapshot();
    ASSERT_EQ(trace.steps.size(), 2U);
    EXPECT_TRUE(std::regex_match(trace.steps[0].step_id, step_id_pattern));
    EXPECT_TRUE(std::regex_match(trace.steps[1].step_id, step_id_pattern));
    EXPECT_NE(trace.steps[0].step_id, trace.steps[1].step_id);
    EXPECT_FALSE(trace.steps[0].parent_step_id.has_value());
    ASSERT_TRUE(trace.steps[1].parent_step_id.has_value());
    EXPECT_EQ(*trace.steps[1].parent_step_id, trace.steps[0].step_id);
    EXPECT_EQ(trace.steps[0].sequence, 1U);
    EXPECT_EQ(trace.steps[1].sequence, 2U);
}

// 快照必须与后续追加隔离，未显式完成的 scope 则由 RAII 兜底为错误。
// JSON 固定字段也在这里集中验证，防止序列化协议无意漂移。
// 第一个步骤正常结束并立即取快照，建立后续隔离断言的基准值。
// 第二个步骤故意依赖析构结束，模拟异常离开或接入层漏写完成调用。
// 旧快照按值持有，后续共享状态追加不能改变其 vector 或状态内容。
// 当前快照必须把遗留 Running 转换为 Error，避免监控端永久等待。
// 错误摘要使用固定中文文本，不包含当前异常、属性或工具名称原文。
// duration_ms 必须非负，即使系统墙钟在步骤期间发生调整。
// started_at 同时验证存在和完整 UTC 毫秒格式，避免区域设置造成协议漂移。
// 根步骤 JSON 使用 null 父节点，不能省略字段或写空字符串。
// type 与 status 使用稳定小写协议值，不暴露 C++ 枚举拼写。
// attributes 与 details 始终是对象，消费端可直接按对象处理。
// duration_ms 和 error_summary 即使为默认值也必须保留。
// 测试不依赖实际睡眠，避免毫秒精度下的时间抖动造成不稳定。
// 若未来增加字段，应保持现有字段类型并补充迁移说明。
TEST(TraceRecorderTest, ReturnsStableSnapshotsAndFinalizesAbandonedScope) {
    aiSDK::AIClient client(traceConfig(true));
    aiSDK::TraceSession session = client.startTrace();
    aiSDK::TraceRecorder recorder(session);

    aiSDK::TraceStepScope completed = recorder.startStep(aiSDK::TraceStepType::ToolBatch);
    completed.setAttribute(aiSDK::TraceAttributeKey::ToolCount, 0);
    // 合法键的错误值类型和非法强转键都必须被 Recorder 拒绝。
    completed.setAttribute(aiSDK::TraceAttributeKey::ToolCount, nlohmann::json{
                                                                    {"secret", "属性对象秘密"}
    });
    completed.setAttribute(static_cast<aiSDK::TraceAttributeKey>(999), "未知属性秘密");
    completed.succeed();
    const aiSDK::Trace old_snapshot = session.snapshot();

    {
        // 故意不调用 succeed/fail，验证析构不会留下永久 Running 状态。
        aiSDK::TraceStepScope abandoned = recorder.startStep(aiSDK::TraceStepType::ToolExecution, completed.stepId());
        abandoned.setAttribute(aiSDK::TraceAttributeKey::ToolName, "未完成工具");
    }

    const aiSDK::Trace current_snapshot = session.snapshot();
    ASSERT_EQ(old_snapshot.steps.size(), 1U);
    ASSERT_EQ(current_snapshot.steps.size(), 2U);
    EXPECT_EQ(old_snapshot.steps.front().status, aiSDK::TraceStepStatus::Success);
    EXPECT_EQ(current_snapshot.steps[1].status, aiSDK::TraceStepStatus::Error);
    EXPECT_EQ(current_snapshot.steps[1].error_summary, "步骤未正常结束");
    EXPECT_GE(current_snapshot.steps[1].duration_ms, 0);
    EXPECT_FALSE(current_snapshot.steps[1].started_at.empty());
    EXPECT_TRUE(std::regex_match(current_snapshot.steps[1].started_at, std::regex("^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3}Z$")));

    // 固定字段始终存在，根步骤 parent_step_id 使用 null。
    const nlohmann::json output = session.toJson();
    ASSERT_EQ(output.at("steps").size(), 2U);
    const nlohmann::json& first = output.at("steps").at(0);
    EXPECT_TRUE(first.at("parent_step_id").is_null());
    EXPECT_EQ(first.at("type"), "tool_batch");
    EXPECT_EQ(first.at("status"), "success");
    EXPECT_TRUE(first.at("attributes").is_object());
    EXPECT_EQ(first.at("attributes").at("tool_count"), 0);
    EXPECT_TRUE(first.at("details").is_object());
    EXPECT_TRUE(first.contains("duration_ms"));
    EXPECT_TRUE(first.contains("error_summary"));
    EXPECT_EQ(first.at("error_code"), "none");
    EXPECT_EQ(output.at("steps").at(1).at("error_code"), "step_abandoned");
    EXPECT_EQ(output.dump().find("属性对象秘密"), std::string::npos);
    EXPECT_EQ(output.dump().find("未知属性秘密"), std::string::npos);
}

// 详情只有经过调用方脱敏器返回对象后才会保存。
// 脱敏器内部读取同一会话快照可证明回调确实运行在 Trace 锁外。
// 工具参数、raw_arguments 和工具结果分别放入不同哨兵文本。
// 若实现误把任一原始对象直接保存，最终 dump 字符串断言会立即失败。
// 脱敏器对参数只返回字段数量，不返回名称、值或原始 JSON。
// 脱敏器对结果只返回 success 标志，不返回 data 内容。
// active_session 在执行前赋值，回调读取的是正在写入的同一共享状态。
// snapshot 若在互斥锁内回调会自锁，本测试会直接暴露死锁回归。
// 两次回调分别对应 ToolArguments 和 ToolResult，不允许遗漏任一阶段。
// 工具处理函数仍收到完整参数并返回完整业务结果。
// Trace 中的 tool_name 属于默认白名单，不影响参数内容保密性。
// details.arguments 与 details.result 使用不同键，避免后写覆盖先写。
// 返回对象由 Trace 层复制，脱敏器局部 JSON 生命周期结束后仍可读取。
// 测试不允许 raw_arguments 作为参数详情来源，因为它保留供应商原文。
// 并发场景下脱敏器自身同步仍由调用方负责，本用例只验证锁边界。
TEST(TraceRecorderTest, RecordsOnlySanitizedToolDetailsOutsideLock) {
    aiSDK::AIClient client(traceConfig(true));
    client.tools().registerTool(makeLocalTool("mask"), [](const nlohmann::json&) {
        return aiSDK::ToolResult::successResult({
            {"secret_result", "输出秘密-456"}
        });
    });

    aiSDK::TraceSession* active_session = nullptr;
    int sanitizer_calls = 0;
    aiSDK::TraceOptions options;
    options.detail_sanitizer = [&](const aiSDK::TraceDetailContext& context, const nlohmann::json& raw) {
        ++sanitizer_calls;
        // 若实现错误地持锁调用脱敏器，这里的 snapshot 会产生自锁。
        const aiSDK::Trace nested_snapshot = active_session->snapshot();
        EXPECT_FALSE(nested_snapshot.trace_id.empty());
        if(context.kind == aiSDK::TraceDetailKind::ToolArguments) {
            EXPECT_EQ(context.operation_name, "mask");
            return nlohmann::json{
                {"argument_field_count", raw.size()}
            };
        }
        return nlohmann::json{
            {"success", raw.at("success")}
        };
    };

    aiSDK::TraceSession session = client.startTrace(options);
    active_session = &session;
    const auto results = client.executeToolCalls(
        {
            {"call_mask", "mask", nlohmann::json{{"secret_input", "输入秘密-123"}}, "原始参数秘密"}
    },
        session);

    ASSERT_EQ(results.size(), 1U);
    EXPECT_TRUE(results.front().result.success);
    EXPECT_EQ(sanitizer_calls, 2);
    const aiSDK::Trace trace = session.snapshot();
    ASSERT_EQ(trace.steps.size(), 2U);
    const aiSDK::TraceStep& tool_step = trace.steps[1];
    EXPECT_EQ(tool_step.details.at("arguments").at("status"), "recorded");
    EXPECT_EQ(tool_step.details.at("arguments").at("value").at("argument_field_count"), 1U);
    EXPECT_EQ(tool_step.details.at("result").at("status"), "recorded");
    EXPECT_TRUE(tool_step.details.at("result").at("value").at("success"));

    // 默认导出不能出现原始参数、raw_arguments 或工具结果内容。
    const std::string exported = session.toJson().dump();
    EXPECT_EQ(exported.find("输入秘密-123"), std::string::npos);
    EXPECT_EQ(exported.find("原始参数秘密"), std::string::npos);
    EXPECT_EQ(exported.find("输出秘密-456"), std::string::npos);
}

// 脱敏器异常和非法返回值都只能形成固定诊断标记，
// 不能阻止工具执行、改变结果或把异常文本与原始值写入 Trace。
// 第一阶段让脱敏器在参数和结果两次调用中都抛标准异常。
// Trace 层不得区分或复制 what()，否则异常秘密会出现在导出文本。
// handler_calls 证明脱敏失败没有提前跳过工具，也没有重复执行工具。
// 成功 ToolResult 必须原样返回，工具步骤状态仍由业务结果决定。
// 参数与结果详情各自保存同一固定诊断，但不保存 raw_value。
// 批次步骤仍成功，因为脱敏只是旁路诊断失败而非工具业务失败。
// 第二阶段返回 JSON 数组，覆盖“正常返回但协议类型非法”的分支。
// 非对象不允许直接进入 details，防止调用方覆盖保留字段结构。
// 非对象诊断与异常诊断使用不同固定文案，便于安全定位配置问题。
// 第二次 handler_calls 增量证明非法返回同样不会短路主流程。
// 两个会话完全隔离，前一次诊断不能泄漏到后一次快照。
// 输入与输出哨兵同时扫描，避免只保护其中一个方向。
// 此契约同样适用于模型请求和响应，跨层测试会继续覆盖。
TEST(TraceRecorderTest, IsolatesSanitizerFailuresFromToolExecution) {
    aiSDK::AIClient client(traceConfig(true));
    int handler_calls = 0;
    client.tools().registerTool(makeLocalTool("stable"), [&](const nlohmann::json&) {
        ++handler_calls;
        return aiSDK::ToolResult::successResult({
            {"value", "工具结果秘密"}
        });
    });

    aiSDK::TraceOptions throwing_options;
    throwing_options.detail_sanitizer = [](const aiSDK::TraceDetailContext&, const nlohmann::json&) -> nlohmann::json {
        throw std::runtime_error("脱敏器异常秘密");
    };
    aiSDK::TraceSession throwing_session = client.startTrace(throwing_options);
    const auto throwing_results = client.executeToolCalls(
        {
            {"call_throw", "stable", nlohmann::json{{"value", "工具参数秘密"}}, "{}"}
    },
        throwing_session);

    ASSERT_EQ(throwing_results.size(), 1U);
    EXPECT_TRUE(throwing_results.front().result.success);
    EXPECT_EQ(handler_calls, 1);
    const aiSDK::Trace throwing_trace = throwing_session.snapshot();
    ASSERT_EQ(throwing_trace.steps.size(), 2U);
    EXPECT_EQ(throwing_trace.steps[1].details.at("arguments").at("status"), "sanitizer_failed");
    EXPECT_TRUE(throwing_trace.steps[1].details.at("arguments").at("value").empty());
    EXPECT_EQ(throwing_trace.steps[1].details.at("result").at("status"), "sanitizer_failed");
    EXPECT_TRUE(throwing_trace.steps[1].details.at("result").at("value").empty());
    const std::string throwing_export = throwing_session.toJson().dump();
    EXPECT_EQ(throwing_export.find("脱敏器异常秘密"), std::string::npos);
    EXPECT_EQ(throwing_export.find("工具参数秘密"), std::string::npos);
    EXPECT_EQ(throwing_export.find("工具结果秘密"), std::string::npos);

    // 顶层非对象返回值同样被拒绝，但第二次工具执行仍然成功。
    aiSDK::TraceOptions invalid_options;
    invalid_options.detail_sanitizer = [](const aiSDK::TraceDetailContext&, const nlohmann::json&) { return nlohmann::json::array({"不应直接保存"}); };
    aiSDK::TraceSession invalid_session = client.startTrace(invalid_options);
    const auto invalid_results = client.executeToolCalls(
        {
            {"call_invalid", "stable", nlohmann::json::object(), "{}"}
    },
        invalid_session);
    ASSERT_EQ(invalid_results.size(), 1U);
    EXPECT_TRUE(invalid_results.front().result.success);
    EXPECT_EQ(handler_calls, 2);
    const aiSDK::Trace invalid_trace = invalid_session.snapshot();
    ASSERT_EQ(invalid_trace.steps.size(), 2U);
    EXPECT_EQ(invalid_trace.steps[1].details.at("arguments").at("status"), "rejected");
    EXPECT_TRUE(invalid_trace.steps[1].details.at("arguments").at("value").empty());
}

// 脱敏器在锁外运行时可能重入并结束当前步骤。
// 结束动作一旦生效，原脱敏调用返回后不得再把详情写入已经完成的快照。
// 本用例直接使用 Recorder 精确控制同一个 Scope，避免工具业务逻辑干扰时序。
// 回调返回合法对象，用于证明丢弃原因是生命周期变化而非类型拒绝。
// 最终步骤应保持 Success，details 仍为空且没有隐藏诊断标记。
// 再次调用 succeed 只验证结束接口幂等，不应改变第一次结束结果。
TEST(TraceRecorderTest, DoesNotWriteDetailAfterSanitizerReentrantlyFinishesStep) {
    aiSDK::AIClient client(traceConfig(true));
    aiSDK::TraceStepScope* active_scope = nullptr;
    aiSDK::TraceOptions options;
    options.detail_sanitizer = [&](const aiSDK::TraceDetailContext&, const nlohmann::json&) {
        // 该结束动作发生在脱敏器回调期间，模拟调用方重入同一 Trace 上下文。
        active_scope->succeed();
        return nlohmann::json{
            {"safe_value", true}
        };
    };

    aiSDK::TraceSession session = client.startTrace(options);
    aiSDK::TraceRecorder recorder(session);
    aiSDK::TraceStepScope step = recorder.startStep(aiSDK::TraceStepType::ToolExecution);
    active_scope = &step;
    step.setSanitizedDetail(aiSDK::TraceDetailSlot::Arguments,
                            aiSDK::TraceDetailContext{
                                aiSDK::TraceDetailKind::ToolArguments, "reentrant_tool"
    },
                            nlohmann::json{{"secret", "重入详情秘密"}});
    step.succeed();

    const aiSDK::Trace trace = session.snapshot();
    ASSERT_EQ(trace.steps.size(), 1U);
    EXPECT_EQ(trace.steps.front().status, aiSDK::TraceStepStatus::Success);
    EXPECT_TRUE(trace.steps.front().details.empty());
    EXPECT_EQ(session.toJson().dump().find("重入详情秘密"), std::string::npos);
}

// 单个工具失败必须收敛为结果并继续执行同批后续调用。
// 成功、未知名称、处理函数异常、再次成功四种输入按原顺序返回。
// Trace 为每个调用创建独立子步骤，批次汇总成功与失败数量。
// 失败摘要与错误码都来自固定枚举，不能复制处理函数异常秘密。
// 空批次仍形成一个成功根步骤，计数为零且不会创建伪造子步骤。
// 两个会话独立验证，避免前一批次状态影响空批次结论。
TEST(TraceRecorderTest, ContinuesToolBatchAfterFailuresAndHandlesEmptyBatch) {
    aiSDK::AIClient client(traceConfig(true));
    int successful_handler_calls = 0;
    client.tools().registerTool(makeLocalTool("stable"), [&](const nlohmann::json& arguments) {
        ++successful_handler_calls;
        return aiSDK::ToolResult::successResult({
            {"order", arguments.at("order")}
        });
    });
    client.tools().registerTool(makeLocalTool("throwing"), [](const nlohmann::json&) -> aiSDK::ToolResult { throw std::runtime_error("工具处理异常秘密"); });

    aiSDK::TraceSession session = client.startTrace();
    const auto results = client.executeToolCalls(
        {
            {"call_1", "stable",   nlohmann::json{{"order", 1}}, "{}"},
            {"call_2", "missing",  nlohmann::json::object(),     "{}"},
            {"call_3", "throwing", nlohmann::json::object(),     "{}"},
            {"call_4", "stable",   nlohmann::json{{"order", 4}}, "{}"},
    },
        session);

    ASSERT_EQ(results.size(), 4U);
    EXPECT_TRUE(results[0].result.success);
    EXPECT_FALSE(results[1].result.success);
    EXPECT_FALSE(results[2].result.success);
    EXPECT_TRUE(results[3].result.success);
    EXPECT_EQ(results[3].result.data.at("order"), 4);
    EXPECT_EQ(successful_handler_calls, 2);

    // 批次根步骤之后按输入顺序排列四个工具子步骤。
    const aiSDK::Trace trace = session.snapshot();
    ASSERT_EQ(trace.steps.size(), 5U);
    EXPECT_EQ(trace.steps[0].status, aiSDK::TraceStepStatus::Error);
    EXPECT_EQ(trace.steps[0].failure, aiSDK::TraceFailure::ToolBatchPartialFailure);
    EXPECT_EQ(trace.steps[0].attributes.at("success_count"), 2U);
    EXPECT_EQ(trace.steps[0].attributes.at("failure_count"), 2U);
    EXPECT_EQ(trace.steps[1].status, aiSDK::TraceStepStatus::Success);
    EXPECT_EQ(trace.steps[2].failure, aiSDK::TraceFailure::ToolExecutionFailed);
    EXPECT_EQ(trace.steps[3].failure, aiSDK::TraceFailure::ToolExecutionFailed);
    EXPECT_EQ(trace.steps[4].status, aiSDK::TraceStepStatus::Success);
    EXPECT_EQ(session.toJson().dump().find("工具处理异常秘密"), std::string::npos);

    // 空批次只记录自身，并以零计数正常完成。
    aiSDK::TraceSession empty_session = client.startTrace();
    const auto empty_results = client.executeToolCalls({}, empty_session);
    EXPECT_TRUE(empty_results.empty());
    const aiSDK::Trace empty_trace = empty_session.snapshot();
    ASSERT_EQ(empty_trace.steps.size(), 1U);
    EXPECT_EQ(empty_trace.steps.front().status, aiSDK::TraceStepStatus::Success);
    EXPECT_EQ(empty_trace.steps.front().attributes.at("tool_count"), 0U);
    EXPECT_EQ(empty_trace.steps.front().attributes.at("success_count"), 0U);
    EXPECT_EQ(empty_trace.steps.front().attributes.at("failure_count"), 0U);
}

// 同一会话的副本允许多线程追加，同时另一个线程持续读取快照和 JSON。
// 最终步骤数、序号和 ID 集合必须完整且稳定。
// 六个写线程各自复制 TraceSession，验证 shared_ptr 共享而非深拷贝步骤。
// 每个线程创建四十个短步骤，足以频繁触发 vector 扩容和锁竞争。
// 起跑屏障确保读写线程真正重叠，避免写线程在读取线程调度前全部结束。
// 单个 Scope 始终由创建线程完成，本测试不承诺 Scope 自身跨线程安全。
// 读取线程同时调用 snapshot 和 toJson，覆盖复制与序列化交错。
// 读取线程不调用 GTest 宏，避免测试框架自身成为数据竞争来源。
// 任意读取异常或结构破坏都会通过原子标志回传主测试线程。
// yield 只让调度器有机会交错执行，不承担正确性同步职责。
// 主线程确认读取至少执行一次后才设置完成标志，避免空转用例产生假阳性。
// 最终数量等于线程数乘单线程步骤数，任何丢写都会被发现。
// sequence 必须连续递增，不能因 atomic 分配和 vector 插入分离而乱序。
// step_id 集合大小等于步骤数，验证线程局部随机引擎没有复用序列。
// 所有步骤成功结束，证明移动与析构没有并发误标错误。
TEST(TraceRecorderTest, SupportsConcurrentWritersAndReaders) {
    constexpr std::size_t kWriterCount = 6;
    constexpr std::size_t kStepsPerWriter = 40;
    aiSDK::AIClient client(traceConfig(true));
    aiSDK::TraceSession session = client.startTrace();
    std::atomic<bool> writers_done{false};
    std::atomic<bool> reader_failed{false};
    std::atomic<bool> start_all{false};
    std::atomic<std::size_t> ready_count{0};
    std::atomic<std::size_t> reader_iterations{0};

    // 读取线程不使用 GTest 宏，避免从工作线程写测试框架状态。
    std::thread reader([&] {
        ready_count.fetch_add(1U);
        while(!start_all.load()) {
            std::this_thread::yield();
        }
        try {
            while(!writers_done.load()) {
                const aiSDK::Trace snapshot = session.snapshot();
                const nlohmann::json output = session.toJson();
                if(snapshot.trace_id.empty() || !output.at("steps").is_array()) {
                    reader_failed.store(true);
                    return;
                }
                // 任意中间快照都必须维持严格递增序号，不能只保证最终排序。
                std::uint64_t previous_sequence = 0;
                for(const auto& step : snapshot.steps) {
                    if(step.sequence <= previous_sequence) {
                        reader_failed.store(true);
                        return;
                    }
                    previous_sequence = step.sequence;
                }
                reader_iterations.fetch_add(1U);
                std::this_thread::yield();
            }
        } catch(...) {
            reader_failed.store(true);
        }
    });

    std::vector<std::thread> writers;
    writers.reserve(kWriterCount);
    for(std::size_t writer_number = 0; writer_number < kWriterCount; ++writer_number) {
        // 每个线程复制会话句柄，共享同一受互斥保护的内部状态。
        writers.emplace_back([session, &ready_count, &start_all, kStepsPerWriter]() mutable {
            ready_count.fetch_add(1U);
            while(!start_all.load()) {
                std::this_thread::yield();
            }
            aiSDK::TraceRecorder recorder(session);
            for(std::size_t step_index = 0; step_index < kStepsPerWriter; ++step_index) {
                aiSDK::TraceStepScope step = recorder.startStep(aiSDK::TraceStepType::ToolExecution);
                step.succeed();
            }
        });
    }
    // 所有工作线程到达屏障后同时放行，确保本用例覆盖真实并发交错。
    while(ready_count.load() != kWriterCount + 1U) {
        std::this_thread::yield();
    }
    start_all.store(true);
    for(auto& writer : writers) {
        writer.join();
    }
    while(reader_iterations.load() == 0U && !reader_failed.load()) {
        std::this_thread::yield();
    }
    writers_done.store(true);
    reader.join();

    EXPECT_FALSE(reader_failed.load());
    EXPECT_GT(reader_iterations.load(), 0U);
    const aiSDK::Trace trace = session.snapshot();
    ASSERT_EQ(trace.steps.size(), kWriterCount * kStepsPerWriter);
    std::set<std::string> step_ids;
    for(std::size_t index = 0; index < trace.steps.size(); ++index) {
        EXPECT_EQ(trace.steps[index].sequence, index + 1U);
        EXPECT_EQ(trace.steps[index].status, aiSDK::TraceStepStatus::Success);
        step_ids.insert(trace.steps[index].step_id);
    }
    EXPECT_EQ(step_ids.size(), trace.steps.size());
}

}  // namespace
