#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "trace/Trace.h"

namespace aiSDK {

// Trace 公开控制面分成 Session、Recorder 和 Scope 三层。
// Session 属于调用方，Recorder 属于接入层，Scope 属于单次局部操作。
// 三层都不使用全局变量或线程局部“当前会话”。
// 这种显式传递让并发任务和跨请求链路保持可审计。
// 默认禁用对象始终可安全调用，避免业务代码维护两套控制流。
class AIClient;
struct TraceSessionState;

// TraceDetailContext 描述本次脱敏输入所属的协议位置。
// operation_name 对模型详情表示 Provider 名，对工具详情表示工具名。
// 字符串视图只在同步回调期间有效，脱敏器不得跨调用保存该引用。
struct TraceDetailContext {
    TraceDetailKind kind = TraceDetailKind::ModelRequest;
    std::string_view operation_name;
};

// 脱敏器在原始值进入 Trace 前执行。
// 返回值只有在顶层为 JSON 对象时才会保存，抛出的异常会被 Trace 层隔离。
using TraceDetailSanitizer = std::function<nlohmann::json(const TraceDetailContext& context, const nlohmann::json& raw_value)>;

struct TraceOptions {
    // detail_sanitizer 默认为空，此时模型消息、工具参数和结果均不会进入 Trace。
    // 同一会话并发使用时，调用方需要保证自定义函数自身可并发调用。
    TraceDetailSanitizer detail_sanitizer;
};

// TraceSession 是可复制的线程安全共享句柄。
// 默认构造得到禁用会话，适合在 enable_trace 关闭时作为空操作上下文传递。
// 会话没有 close()，因为调用方可以在任意多个公开操作之间长期复用。
// 复制会话不会复制步骤，所有副本观察同一条显式链路。
class TraceSession {
   public:
    TraceSession() = default;

    // enabled 只判断是否存在共享状态，不触发 ID、时间戳或步骤分配。
    bool enabled() const noexcept;
    // traceId 返回副本，避免向调用方暴露内部状态的引用生命周期。
    std::string traceId() const;
    // snapshot 在同一把互斥锁内复制 ID 和步骤，返回一致时刻的稳定视图。
    Trace snapshot() const;
    // toJson 基于 snapshot 序列化，不会在持锁期间执行 JSON 构造。
    nlohmann::json toJson() const;

   private:
    explicit TraceSession(std::shared_ptr<TraceSessionState> state);
    // createEnabled 只由 AIClient 在总开关开启时调用。
    static TraceSession createEnabled(TraceOptions options);

    std::shared_ptr<TraceSessionState> state_;

    friend class AIClient;
    friend class TraceRecorder;
};

// TraceStepScope 负责单个步骤的生命周期。
// 对象离开作用域前若未显式结束，会自动记录为错误，避免遗留 Running 步骤。
// 单个 scope 只应在创建它的调用流程内使用，不承诺自身并发安全。
// 移动会转移完成责任，源对象析构不会再次更新步骤。
class TraceStepScope {
   public:
    TraceStepScope() = default;
    ~TraceStepScope() noexcept;

    TraceStepScope(const TraceStepScope&) = delete;
    TraceStepScope& operator=(const TraceStepScope&) = delete;
    TraceStepScope(TraceStepScope&& other) noexcept;
    TraceStepScope& operator=(TraceStepScope&& other) noexcept;

    bool enabled() const noexcept;
    bool wantsDetails() const noexcept;
    const std::string& stepId() const noexcept;

    // setAttribute 仅供 SDK 各层写入固定白名单元数据。
    // Trace 内部故障会被吞掉，不能改变主请求或工具执行结果。
    template <typename Value>
    void setAttribute(TraceAttributeKey key, Value&& value) noexcept {
        if(!enabled()) {
            return;
        }
        try {
            // JSON 构造也放在 noexcept 边界内，避免分配失败穿透业务路径。
            setAttributeJson(key, nlohmann::json(std::forward<Value>(value)));
        } catch(...) {
            // Trace 属性属于旁路元数据，构造失败时直接丢弃本字段。
        }
    }
    // setSanitizedDetail 在锁外调用脱敏器，只保存其返回的结构化对象。
    // 脱敏器抛错或返回非对象时会写入安全诊断标记，不保存 raw_value。
    void setSanitizedDetail(TraceDetailSlot slot, const TraceDetailContext& context, const nlohmann::json& raw_value) noexcept;

    // succeed / fail 都是幂等结束操作，第一次调用决定最终状态和耗时。
    void succeed() noexcept;
    void fail(TraceFailure failure) noexcept;

   private:
    TraceStepScope(std::shared_ptr<TraceSessionState> state, std::string step_id, std::size_t step_index,
                   std::chrono::steady_clock::time_point started_steady) noexcept;
    void setAttributeJson(TraceAttributeKey key, nlohmann::json value) noexcept;
    void finish(TraceStepStatus status, TraceFailure failure) noexcept;

    std::shared_ptr<TraceSessionState> state_;
    std::string step_id_;
    std::size_t step_index_ = 0;
    std::chrono::steady_clock::time_point started_steady_{};
    bool finished_ = true;

    friend class TraceRecorder;
};

// TraceRecorder 是 SDK 各层共享的步骤创建入口。
// 它只复制 TraceSession 的 shared_ptr，不拥有全局或线程局部“当前 Trace”。
// Recorder 本身无可变成员，可在需要记录的局部作用域临时创建。
class TraceRecorder {
   public:
    explicit TraceRecorder(const TraceSession& session) noexcept;

    // parent_step_id 为空表示公开操作根步骤；嵌套层必须显式传入父步骤 ID。
    // 任何 Trace 内部异常都会退化为禁用 scope，保证不影响业务控制流。
    TraceStepScope startStep(TraceStepType type, std::string_view parent_step_id = {}) const noexcept;

   private:
    std::shared_ptr<TraceSessionState> state_;
};

}  // namespace aiSDK
