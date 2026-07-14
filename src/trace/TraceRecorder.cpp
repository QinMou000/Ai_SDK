#include "trace/TraceRecorder.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <locale>
#include <mutex>
#include <random>
#include <sstream>
#include <utility>

namespace aiSDK {

// 本实现遵循“Trace 永远不能成为业务失败源”的总原则。
// 除显式会话创建与导出外，所有记录路径都在内部收敛异常。
// 公共快照采用值语义，任何内部指针和互斥对象都不会越过实现边界。
// 默认属性和值类型形成双重白名单，复杂对象只能走显式脱敏详情通道。
// 失败机器码与摘要集中映射，接入层不能把底层异常字符串写入状态。
// 线程安全只覆盖共享 Trace 状态，不替代调用方业务对象的同步。
// 共享锁只保护步骤容器、序号和步骤字段，不包围用户回调。
// 所有原始详情均保持在调用栈上，只有脱敏后的对象进入 State。
// 快照复制与 JSON 导出分离，避免大对象序列化长时间占锁。
// 状态容器只追加不删除，因此开始下标是可靠的 O(1) 更新键。
// 公开 sequence 是确定性顺序，随机 step_id 只承担关联身份。
// UTC 时间适合展示，steady_clock 耗时适合性能分析。
// 禁用 Session 通过空 shared_ptr 表达，不额外分配哨兵状态。
// Scope 的所有写方法保持 noexcept，异常展开期间不会二次失败。
// 任何新增字段都必须先确认默认安全性，再进入固定白名单。
// 任何新增回调都必须继续遵守锁外执行和异常隔离边界。
// TraceSessionState 只存在于实现文件，公共头文件不会暴露互斥锁和容器布局。
// options 在会话创建后保持只读，脱敏器因此可以在不持有步骤锁时复制和调用。
// steps 只追加不删除，创建时下标在整个会话生命周期内保持稳定。
// next_sequence 与追加共用 mutex，保证序号和容器顺序完全一致。
// 会话副本和在途 Scope 共同持有该状态，任一对象销毁都不会制造悬空引用。
struct TraceSessionState {
    TraceSessionState(std::string id, TraceOptions trace_options) : trace_id(std::move(id)), options(std::move(trace_options)) {}

    std::mutex mutex;
    std::string trace_id;
    TraceOptions options;
    std::uint64_t next_sequence = 1;
    std::vector<TraceStep> steps;
};

namespace {

// generateHexId 生成 W3C Trace Context 形状的小写十六进制 ID。
// 引擎按线程持有，避免并发创建会话时共享伪随机状态产生数据竞争。
// ID 不编码用户、主机、线程、时间或请求内容，避免形成旁路信息泄露。
// trace_id 使用 32 个十六进制字符，step_id 使用 16 个字符，长度由调用方明确传入。
// random_device 只在线程首次使用时参与播种，正常步骤创建不反复访问系统源。
std::string generateHexId(std::size_t length) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    thread_local std::mt19937_64 engine([] {
        std::random_device device;
        std::seed_seq seed{
            device(),
            device(),
            static_cast<unsigned int>(std::chrono::high_resolution_clock::now().time_since_epoch().count()),
        };
        return std::mt19937_64(seed);
    }());
    std::uniform_int_distribution<int> distribution(0, 15);

    std::string id(length, '0');
    bool has_non_zero_digit = false;
    for(char& digit : id) {
        digit = kHexDigits[distribution(engine)];
        has_non_zero_digit = has_non_zero_digit || digit != '0';
    }
    // 全零 ID 在 W3C 语义中无效；极小概率命中时固定最后一位即可。
    if(!has_non_zero_digit && !id.empty()) {
        id.back() = '1';
    }
    return id;
}

// formatUtcTimestamp 使用系统时钟生成可导出的 UTC 时间，
// 耗时计算另用 steady_clock，避免系统时间调整造成负耗时。
// UTC 字符串固定保留毫秒和 Z 后缀，便于日志系统或前端直接解析。
// Windows 与 POSIX 的线程安全时间转换接口在这里统一封装。
// 墙钟只用于展示和跨步骤对照，绝不参与步骤排序或超时判断。
std::string formatUtcTimestamp(std::chrono::system_clock::time_point time_point) {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(time_point.time_since_epoch()) % 1000;
    const std::time_t raw_time = std::chrono::system_clock::to_time_t(time_point);
    std::tm utc_time{};
#ifdef _WIN32
    if(gmtime_s(&utc_time, &raw_time) != 0) {
        return "";
    }
#else
    if(gmtime_r(&raw_time, &utc_time) == nullptr) {
        return "";
    }
#endif

    std::ostringstream output;
    // 固定 classic locale，避免进程区域设置改变公开时间字符串。
    output.imbue(std::locale::classic());
    output << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << milliseconds.count() << 'Z';
    return output.str();
}

// findStepLocked 要求调用方已经持有 state.mutex。
// 步骤只追加不删除，创建时保存的下标可直接提供常量复杂度更新。
// step_id 二次校验能防止未来容器策略调整后误写错误下标。
// 找不到步骤时返回空指针，调用方把 Trace 内部不一致收敛为无操作。
// 函数不获取锁，避免在已持锁更新路径中发生递归锁定。
TraceStep* findStepLocked(TraceSessionState& state, std::size_t step_index, const std::string& step_id) {
    if(step_index >= state.steps.size() || state.steps[step_index].step_id != step_id) {
        return nullptr;
    }
    return &state.steps[step_index];
}

// 属性值类型也是公开白名单的一部分，不能只限制键名。
// 字符串、布尔值和非负/有符号整数覆盖当前全部安全元数据。
// 非法强转得到的未知枚举或 JSON 对象、数组、浮点值都会被直接丢弃。
bool isAllowedAttributeValue(TraceAttributeKey key, const nlohmann::json& value) noexcept {
    // 字符串组只承载已确认可公开的名称或固定 HTTP 方法。
    // 布尔组表达分支事实，禁止用字符串“true/false”造成类型漂移。
    // 数值组允许有符号与无符号整数，以兼容配置值和 size_t 计数。
    // 浮点值不属于当前元数据协议，避免不同 JSON 消费端精度差异。
    // 对象与数组只能经过详情脱敏器进入 details，不能混入 attributes。
    switch(key) {
        case TraceAttributeKey::Provider:
        case TraceAttributeKey::Model:
        case TraceAttributeKey::HttpMethod:
        case TraceAttributeKey::ToolName:
            return value.is_string();
        case TraceAttributeKey::Stream:
        case TraceAttributeKey::CallbackPresent:
        case TraceAttributeKey::Success:
            return value.is_boolean();
        case TraceAttributeKey::MessageCount:
        case TraceAttributeKey::ToolDefinitionCount:
        case TraceAttributeKey::ToolCallCount:
        case TraceAttributeKey::PromptTokens:
        case TraceAttributeKey::CompletionTokens:
        case TraceAttributeKey::TotalTokens:
        case TraceAttributeKey::TimeoutMs:
        case TraceAttributeKey::HttpStatusCode:
        case TraceAttributeKey::ResponseBytes:
        case TraceAttributeKey::ChunkCount:
        case TraceAttributeKey::DeltaCount:
        case TraceAttributeKey::ToolCallDeltaCount:
        case TraceAttributeKey::DoneCount:
        case TraceAttributeKey::ErrorCount:
        case TraceAttributeKey::EventCount:
        case TraceAttributeKey::ToolCount:
        case TraceAttributeKey::SuccessCount:
        case TraceAttributeKey::FailureCount:
        case TraceAttributeKey::ToolIndex:
            return value.is_number_integer() || value.is_number_unsigned();
    }
    return false;
}

// 详情类别和槽位保持一一对应，避免未来接入层把错误协议写入合法键。
// operation_name 只帮助调用方选择脱敏策略，不参与默认 Trace 数据存储。
bool matchesDetailSlot(TraceDetailSlot slot, TraceDetailKind kind) noexcept {
    switch(slot) {
        case TraceDetailSlot::Request:
            return kind == TraceDetailKind::ModelRequest;
        case TraceDetailSlot::Response:
            return kind == TraceDetailKind::ModelResponse;
        case TraceDetailSlot::Arguments:
            return kind == TraceDetailKind::ToolArguments;
        case TraceDetailSlot::Result:
            return kind == TraceDetailKind::ToolResult;
    }
    return false;
}

}  // namespace

// 私有构造只接收已经完整初始化的共享状态。
// 默认构造仍保留为空句柄，不会通过本构造隐式创建 Trace。
// shared_ptr 按值传入并移动，避免额外增加一次引用计数操作。
TraceSession::TraceSession(std::shared_ptr<TraceSessionState> state) : state_(std::move(state)) {}

// createEnabled 是唯一启用会话工厂，由 AIClient 在检查总开关后调用。
// Trace ID 在状态发布前生成，其他线程不会观察到半初始化对象。
// 显式创建失败可以由 startTrace 调用方感知，不会发生在业务步骤埋点中。
TraceSession TraceSession::createEnabled(TraceOptions options) {
    return TraceSession(std::make_shared<TraceSessionState>(generateHexId(32), std::move(options)));
}

// enabled 只检查 shared_ptr，不访问锁和时钟。
// 该常量级判断用于让关闭路径跳过属性 JSON 与详情构造。
// 它不表示会话中已经存在步骤，只表示允许记录。
bool TraceSession::enabled() const noexcept {
    return static_cast<bool>(state_);
}

// traceId 按值返回，调用方持有的字符串不会随会话销毁而失效。
// 禁用会话返回空字符串，与 W3C 形状的有效 ID 明确区分。
// 读取与快照使用相同互斥边界，便于未来状态扩展时保持一致。
std::string TraceSession::traceId() const {
    if(!state_) {
        return "";
    }
    // trace_id 创建后不再修改，但仍统一在状态锁内复制，维持一致访问边界。
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->trace_id;
}

// snapshot 是 TraceSession 唯一公开状态读取入口。
// 锁内只做值复制，排序在锁外执行，降低对并发步骤更新的阻塞时间。
// 返回对象与后续会话变化隔离，调用方可以长期持有或跨线程传递。
Trace TraceSession::snapshot() const {
    if(!state_) {
        return Trace{};
    }

    Trace trace;
    {
        // 在同一临界区复制 ID 与全部步骤，避免并发更新产生撕裂快照。
        std::lock_guard<std::mutex> lock(state_->mutex);
        trace.trace_id = state_->trace_id;
        trace.steps = state_->steps;
    }
    // 序号在步骤开始时分配；排序后不受并发任务完成顺序影响。
    std::sort(trace.steps.begin(), trace.steps.end(), [](const TraceStep& left, const TraceStep& right) { return left.sequence < right.sequence; });
    return trace;
}

// toJson 是调用方主动触发的读取操作，不属于请求过程中的旁路记录。
// 它先取得按值快照，再调用统一映射生成稳定 JSON，避免在会话锁内分配复杂对象。
// 显式导出允许把内存分配或序列化异常交给调用方处理，业务埋点仍保持 noexcept。
// 禁用会话沿用同一结构，输出空 trace_id 与空 steps，消费端无需维护另一套协议。
// 所有字段名称继续由 Trace.cpp 集中维护，本层不复制或扩展序列化规则。
nlohmann::json TraceSession::toJson() const {
    // JSON 构造可能分配较多内存，必须在 snapshot 释放锁后执行。
    // 显式导出失败由调用方处理，不属于业务请求旁路记录失败。
    // 统一复用 traceToJson，避免会话和手工快照形成两套协议。
    return traceToJson(snapshot());
}

// Scope 构造只保存共享状态、稳定下标与两种标识。
// started_steady 已在步骤公开写入时获取，保证耗时覆盖完整步骤生命周期。
// finished_ 设为 false 后，当前对象承担且只承担一次结束责任。
TraceStepScope::TraceStepScope(std::shared_ptr<TraceSessionState> state, std::string step_id, std::size_t step_index,
                               std::chrono::steady_clock::time_point started_steady) noexcept
    : state_(std::move(state)), step_id_(std::move(step_id)), step_index_(step_index), started_steady_(started_steady), finished_(false) {}

// 析构兜底只用于漏掉显式结束或异常离开作用域的路径。
// 固定摘要不读取当前异常，因此不会把业务数据复制进 Trace。
// fail 是 noexcept 且幂等，已完成步骤的正常析构不会再次更新。
TraceStepScope::~TraceStepScope() noexcept {
    // 未显式完成通常意味着异常离开作用域；只记录安全摘要，不猜测或复制原始异常文本。
    fail(TraceFailure::StepAbandoned);
}

// 移动构造转移 shared_ptr、ID、下标、时钟和完成状态。
// 源对象立即失去结束责任，避免两个析构函数竞争写同一步骤。
// 该操作不触碰共享状态锁，因此可安全用于函数返回值优化之外的移动。
TraceStepScope::TraceStepScope(TraceStepScope&& other) noexcept
    : state_(std::move(other.state_)),
      step_id_(std::move(other.step_id_)),
      step_index_(other.step_index_),
      started_steady_(other.started_steady_),
      finished_(other.finished_) {
    other.finished_ = true;
}

// 移动赋值先关闭目标对象原来负责的步骤，再接管来源步骤。
// “步骤作用域被替换”只描述生命周期事实，不包含任何业务输入。
// 自赋值直接返回，避免误把当前有效 Scope 标记错误。
TraceStepScope& TraceStepScope::operator=(TraceStepScope&& other) noexcept {
    if(this == &other) {
        return *this;
    }

    // 覆盖仍在运行的 scope 前先关闭旧步骤，避免留下永久 Running 状态。
    fail(TraceFailure::ScopeReplaced);
    state_ = std::move(other.state_);
    step_id_ = std::move(other.step_id_);
    step_index_ = other.step_index_;
    started_steady_ = other.started_steady_;
    finished_ = other.finished_;
    other.finished_ = true;
    return *this;
}

// Scope 只有在持有状态且尚未完成时才接受更新。
// 已完成后的属性或详情写入被忽略，最终快照不会出现结束后漂移。
// 禁用 Scope 与已结束 Scope 共用同一空操作语义。
bool TraceStepScope::enabled() const noexcept {
    return state_ && !finished_;
}

// wantsDetails 让接入层在构造完整请求或响应 JSON 前做廉价判断。
// 这保证没有脱敏器时不会为了 Trace 复制原始消息或响应正文。
// options 创建后只读，因此读取 std::function 有效性不需要步骤锁。
bool TraceStepScope::wantsDetails() const noexcept {
    return enabled() && static_cast<bool>(state_->options.detail_sanitizer);
}

// stepId 返回 Scope 自身持有的稳定字符串引用。
// 调用方只在 Scope 生命周期内把它传给嵌套层作为 parent_step_id。
// 禁用 Scope 返回空字符串，Recorder 会把空父 ID 解释为根步骤。
const std::string& TraceStepScope::stepId() const noexcept {
    return step_id_;
}

// 属性更新只允许发生在步骤运行期间，并在同一会话锁内完成。
// 接入层负责选择固定白名单键，Recorder 不读取或解释属性内容。
// nlohmann::json 分配失败被捕获，业务路径不会因观测字段丢失而失败。
void TraceStepScope::setAttributeJson(TraceAttributeKey key, nlohmann::json value) noexcept {
    if(!enabled() || !isAllowedAttributeValue(key, value)) {
        return;
    }

    try {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if(TraceStep* step = findStepLocked(*state_, step_index_, step_id_)) {
            step->attributes[traceAttributeKeyToString(key)] = std::move(value);
        }
    } catch(...) {
        // Trace 是旁路能力，分配或序列化失败不能改变主流程。
    }
}

// 详情处理分成“锁外脱敏”和“锁内保存”两个阶段。
// 这样调用方回调可以安全读取快照，也不会长时间阻塞其他步骤更新。
// raw_value 的生命周期只覆盖本次调用，任何路径都不会把它放入共享状态。
void TraceStepScope::setSanitizedDetail(TraceDetailSlot slot, const TraceDetailContext& context, const nlohmann::json& raw_value) noexcept {
    if(!wantsDetails() || !matchesDetailSlot(slot, context.kind)) {
        return;
    }

    nlohmann::json detail_entry;
    try {
        // 脱敏器可能执行任意调用方逻辑，必须在会话互斥锁外运行。
        nlohmann::json sanitized = state_->options.detail_sanitizer(context, raw_value);
        if(!sanitized.is_object()) {
            detail_entry = nlohmann::json{
                {"status", traceDetailStatusToString(TraceDetailStatus::Rejected)},
                {"value",  nlohmann::json::object()                              }
            };
        } else {
            detail_entry = nlohmann::json{
                {"status", traceDetailStatusToString(TraceDetailStatus::Recorded)},
                {"value",  std::move(sanitized)                                  }
            };
        }
    } catch(...) {
        // 状态只说明脱敏阶段失败，绝不把异常文本或原始值带入 Trace。
        try {
            detail_entry = nlohmann::json{
                {"status", traceDetailStatusToString(TraceDetailStatus::SanitizerFailed)},
                {"value",  nlohmann::json::object()                                     }
            };
        } catch(...) {
            // 连固定状态都无法分配时直接放弃详情，主流程继续。
            return;
        }
    }

    // 脱敏器允许重入；若它已经结束当前 Scope，回调返回后不能再漂移详情。
    if(!enabled()) {
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if(TraceStep* step = findStepLocked(*state_, step_index_, step_id_); step && step->status == TraceStepStatus::Running) {
            step->details[traceDetailSlotToString(slot)] = std::move(detail_entry);
        }
    } catch(...) {
        // 保存诊断失败同样不能影响业务返回值或异常。
    }
}

// succeed 是成功路径的显式结束点。
// 空摘要由 finish 写入固定字段，JSON 消费者无需判断字段缺失。
// 重复调用会由 enabled 判断收敛为无操作。
void TraceStepScope::succeed() noexcept {
    finish(TraceStepStatus::Success, TraceFailure::None);
}

// fail 只接受固定失败枚举，不允许接入层传入任意异常文本。
// 机器码和中文摘要都由 Trace 核心集中映射，避免各层协议漂移。
// 状态、失败码和摘要在同一锁内更新，快照不会看到不一致组合。
void TraceStepScope::fail(TraceFailure failure) noexcept {
    // None 只属于成功步骤；误用 fail(None) 时转为明确的记录失败分类。
    if(failure == TraceFailure::None) {
        failure = TraceFailure::TraceRecordingFailed;
    }
    finish(TraceStepStatus::Error, failure);
}

// finish 统一成功、已知失败与析构兜底的状态更新。
// steady_clock 只计算相对时长，系统墙钟回拨不会产生负耗时。
// finished_ 在加锁前设置，任何内部失败都不会导致析构重复尝试。
void TraceStepScope::finish(TraceStepStatus status, TraceFailure failure) noexcept {
    if(!enabled()) {
        return;
    }
    // 先关闭本地状态，保证即使后续 Trace 更新失败也不会在析构时重复写入。
    finished_ = true;
    const auto elapsed = std::chrono::steady_clock::now() - started_steady_;
    const long long duration_ms = std::max<long long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

    try {
        // 摘要先在锁外完成分配，再以移动方式与其他字段一起提交。
        // 先写摘要和辅助字段，最后写 status，使快照中的完成状态尽量完整。
        // std::string 的分配不占用会话锁，降低并发快照等待时间。
        std::string safe_summary = traceFailureToSummary(failure);
        std::lock_guard<std::mutex> lock(state_->mutex);
        if(TraceStep* step = findStepLocked(*state_, step_index_, step_id_)) {
            step->error_summary = std::move(safe_summary);
            step->duration_ms = duration_ms;
            step->failure = failure;
            step->status = status;
        }
    } catch(...) {
        // 摘要分配失败时仍尽力结束步骤；空摘要优先于永久 Running 状态。
        // 回退路径不再进行任何动态字符串分配，只写固定枚举与数值。
        // 若互斥锁本身也失败，只能丢弃旁路更新，业务异常仍保持原样。
        try {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if(TraceStep* step = findStepLocked(*state_, step_index_, step_id_)) {
                step->error_summary.clear();
                step->duration_ms = duration_ms;
                step->failure = failure;
                step->status = status;
            }
        } catch(...) {
            // Trace 更新失败不能覆盖业务异常，也不能让 noexcept 析构终止进程。
        }
    }
}

// Recorder 只复制共享状态句柄，不复制 TraceOptions 或步骤容器。
// 从禁用会话构造时 state_ 为空，后续 startStep 立即返回空 Scope。
// 构造不读取时钟、随机源或锁，适合每个接入函数按需创建。
TraceRecorder::TraceRecorder(const TraceSession& session) noexcept : state_(session.state_) {}

// startStep 是序号分配、ID 创建和初始 Running 快照的原子边界。
// 随机与时间格式化先在锁外完成，临界区只负责序号和 vector 追加。
// 父 ID 必须由调用层显式传入，Recorder 不维护“最近步骤”隐式状态。
TraceStepScope TraceRecorder::startStep(TraceStepType type, std::string_view parent_step_id) const noexcept {
    if(!state_) {
        return TraceStepScope{};
    }

    try {
        std::string step_id = generateHexId(16);
        const auto started_system = std::chrono::system_clock::now();
        const auto started_steady = std::chrono::steady_clock::now();

        TraceStep step;
        step.step_id = step_id;
        if(!parent_step_id.empty()) {
            step.parent_step_id = std::string(parent_step_id);
        }
        step.type = type;
        step.status = TraceStepStatus::Running;
        step.started_at = formatUtcTimestamp(started_system);
        // 时间转换失败时放弃本步骤，不能向公开快照写入不符合契约的空时间。
        if(step.started_at.empty()) {
            return TraceStepScope{};
        }

        std::size_t step_index = 0;
        {
            // 序号分配和步骤插入共用同一临界区，保证开始顺序不会丢失或重复。
            std::lock_guard<std::mutex> lock(state_->mutex);
            step.sequence = state_->next_sequence;
            step_index = state_->steps.size();
            state_->steps.push_back(std::move(step));
            ++state_->next_sequence;
        }
        return TraceStepScope(state_, std::move(step_id), step_index, started_steady);
    } catch(...) {
        // 若 Trace 内部无法创建步骤，主调用仍继续执行，只丢弃本次旁路记录。
        return TraceStepScope{};
    }
}

}  // namespace aiSDK
