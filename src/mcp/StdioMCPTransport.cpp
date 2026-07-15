#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "mcp/detail/MCPTransportFactory.h"
#include "mcp/detail/Process.h"

namespace aiSDK::detail {
namespace {

// 本实现只承担 stdio 传输语义，不解释 Tools、Catalog 或 JSON-RPC 方法。
// 公开请求是否允许提交由 MCPClient 在自己的状态锁下二次校验。
// Transport 的原子边界是有界队列插入成功，而不是管道物理写入成功。
// 进入队列后的工具调用可能已经执行，因此任何后续失败都必须异步关联。
// 进程启动参数始终保持结构化，字符串内容不会重新进入命令解释器。
// stdout 是唯一协议输入；stderr 即使包含 JSON 也只作为不可信诊断字节。
// 固定四个 Worker 避免按请求或按消息创建线程导致资源无界增长。
// 写 Worker 串行拥有 stdin，stdout 与 stderr 则各自只有一个读者。
// 监视 Worker 只观察根进程退出，不保存或公开可能敏感的退出上下文。
// 所有 Worker 都通过同一个停止标志收敛，终命原因只允许发布一次。
// Transport 锁只保护内存状态，不跨越平台 I/O、等待或 Client 回调。
// Client 回调可能立即获取自身状态锁，因此锁外调用也是固定锁序要求。
// close 撤销回调早于取消 I/O，迟到完成不会在关闭路径重入上层状态机。
// 正常关闭优先给 Server 一个 stdin EOF 窗口，超时后才终止完整进程树。
// 所有动态错误文本固定为中文分类，不拼接命令、环境、stderr 或路径。

// StdioPreparedMessage 保存已经完成严格 UTF-8 序列化的单条协议消息。
// 换行在准备阶段一次性追加，写 Worker 只需执行不可分割的完整写入。
// 准备对象由创建它的 Transport 独占解释，不提供正文公开访问接口。
// 对象销毁会同时释放序列化正文，避免超时或二次校验失败后残留数据。
// dispatch_id 只用于本地完成事件，绝不会写入或改写 JSON-RPC 正文。
// deadline 在提交点改写为请求段截止时间，但超时状态仍由 MCPClient 统一裁决。
class StdioPreparedMessage final : public IMCPPreparedMessage {
   public:
    StdioPreparedMessage(std::string line, MCPTransportRequestContext context)
        : line_(std::move(line)), context_(context) {}

    // line 包含且仅包含一条紧凑 JSON 与结尾 LF。
    std::string line_;
    // context 只用于发送完成关联，不参与协议正文。
    MCPTransportRequestContext context_;
};

// OutboundMessage 是进入有界队列后的不可变发送单元。
// 单写者模型保证不同请求、控制响应和取消通知不会发生字节交错。
// 队列按提交顺序保持 FIFO，确保 initialized 等控制消息不会被后发请求越过。
// 正文已经包含 LF，Worker 不再分配或拼接 framing 字节。
// 消息离开队列后由单写者局部变量持有，关闭清空队列不会破坏活动写入。
struct OutboundMessage {
    std::string line;
    MCPTransportRequestContext context;
};

// TransportLifecycle 只描述本地资源所有权，不复制 MCPClient 的协议状态机。
// Fault 通过闭合事件上报，最终资源仍统一经过 close 的幂等清理边界。
// Created 与 Opening 都不允许提交，只有全部 Worker 创建成功后才进入 Open。
// Closing 是资源清理的单一所有权标记，其他关闭者只等待而不重复回收。
// Closed 保证回调已撤销、线程已收敛且 Process 所有权已经释放。
enum class TransportLifecycle { Created, Opening, Open, Closing, Closed };

// remainingMilliseconds 把关闭总截止时间转换为 Process 的相对等待参数。
// 向下取整后的零值表示立即检查，防止任何阶段重新扩张总关闭预算。
// 此函数不抛异常，便于 noexcept close 在每个可能失败的阶段重复取剩余值。
// 单调时钟不受系统时间调整影响，保证超时判断不会因壁钟跳变而倒退。
std::chrono::milliseconds remainingMilliseconds(std::chrono::steady_clock::time_point deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if(now >= deadline) {
        return std::chrono::milliseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

// StdioMCPTransport 负责进程管道、逐行 framing 和有界物理发送。
// JSON-RPC 业务关联留给 MCPClient，平台句柄则完全封闭在 Process 实现中。
// config 与 limits 在构造后保持只读，Server 输出无法改变启动或资源策略。
// 实现不依赖 cpr、HTTP 或 Provider 层，保持 ai_sdk_mcp 内部职责可替换。
// Process 负责平台差异，本类只使用跨平台的完整写、分块读和进程树接口。
// 任何终命路径都唤醒写队列，避免请求线程一直等待永远不会发生的写完成。
// stdin 写失败既关联具体 dispatch，也发布连接级故障以关闭协议连续性。
// stdout framing 失败不尝试跳过坏行，因为后续消息边界与可信性已经丢失。
// stderr 缓存仅用于内部资源约束，不提供日志旁路或隐式秘密输出通道。
class StdioMCPTransport final : public IMCPTransport {
   public:
    StdioMCPTransport(MCPStdioServerConfig config, MCPCommonLimits limits)
        : config_(std::move(config)), limits_(std::move(limits)) {}

    ~StdioMCPTransport() noexcept override {
        // 析构没有资格生成新的宽限预算；正常 MCPClient 生命周期会先传入其唯一截止点。
        close(std::chrono::steady_clock::now());
    }

    // open 是唯一启动子进程和固定 Worker 的入口。
    // 构造阶段因此保持纯配置行为，创建 Client 本身不会产生外部副作用。
    // Process::start 在发布对象前完成平台管道和进程树约束，失败不会留下半对象。
    // 状态锁覆盖从 Created 到 Worker 创建完成的窗口，阻止并发 close 接管半资源。
    // 回调先于进程安装，可保证任何成功启动后的事件都有明确的接收目标。
    // Worker 创建期间仍持有状态锁，已经启动的线程无法观察不完整线程集合。
    // 线程创建失败走与正常 close 等价的取消顺序，但保留原始创建异常。
    // open 不等待初始化响应；协议握手时间由 MCPClient 的公开操作截止控制。
    // 环境映射复制只存在于启动边界，任何异常都不会把映射值拼入错误文本。
    // 本方法不允许重开，避免复用旧回调、旧队列或旧进程代次。
    void open(MCPTransportCallbacks callbacks, std::chrono::steady_clock::time_point absolute_deadline) override {
        if(std::chrono::steady_clock::now() >= absolute_deadline) {
            throw MCPException(MCPErrorCode::RequestTimeout, MCPClientState::Connecting,
                               "stdio MCP Transport 打开前已超时");
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if(lifecycle_ != TransportLifecycle::Created) {
            throw std::logic_error("stdio MCP Transport 只能打开一次");
        }

        // 先复制回调再进入 Opening，分配失败时不会留下半初始化生命周期。
        callbacks_ = std::move(callbacks);
        lifecycle_ = TransportLifecycle::Opening;
        stop_requested_ = false;
        terminal_event_sent_ = false;

        try {
            // 参数逐项传给 Process；任何平台都不会构造 shell 命令字符串。
            // executable 已由配置层校验为绝对真实可执行路径。
            // arguments 保持原始元素边界，空格和元字符都只是普通参数内容。
            // working_directory 为空时由 Process 使用应用当前明确环境，不做扫描。
            // inherit_parent_environment 默认关闭，防止意外把宿主秘密传给 Server。
            ProcessOptions options;
            options.executable = config_.executable;
            options.arguments = config_.arguments;
            options.working_directory = config_.working_directory;
            options.environment = config_.environment;
            options.inherit_parent_environment = config_.inherit_parent_environment;
            process_ = std::make_unique<Process>(Process::start(std::move(options), absolute_deadline));

            // 进程创建成功不代表启动预算仍有效；超时对象不得进入可提交状态。
            if(std::chrono::steady_clock::now() >= absolute_deadline) {
                throw MCPException(MCPErrorCode::RequestTimeout, MCPClientState::Connecting,
                                   "stdio MCP Transport 打开期间超时");
            }

            // Worker 在获取本对象状态锁后才能继续，因此线程创建失败不会泄漏回调。
            lifecycle_ = TransportLifecycle::Open;
            stdout_worker_ = std::thread([this] { stdoutLoop(); });
            stderr_worker_ = std::thread([this] { stderrLoop(); });
            writer_worker_ = std::thread([this] { writerLoop(); });
            monitor_worker_ = std::thread([this] { monitorLoop(); });
        } catch(...) {
            // 部分线程或进程创建失败时，先禁止回调，再在锁外取消全部 I/O。
            // 清空出站队列保证没有准备消息在失败的进程代次中继续提交。
            // closeStdin 先唤醒潜在写者，cancelIo 再唤醒两个独立读者。
            // terminateTree 覆盖根进程已经派生子孙但 Worker 尚未齐备的窗口。
            // joinWorkers 可安全处理未创建的默认 thread，不需要记录创建计数。
            // Process::close 是最终 noexcept 兜底，随后才释放唯一所有权。
            // Closed 在原异常重抛前发布，使并发 close 不会等待失效对象。
            lifecycle_ = TransportLifecycle::Closing;
            stop_requested_ = true;
            callbacks_ = {};
            outbound_queue_.clear();
            queue_cv_.notify_all();
            Process* process = process_.get();
            lock.unlock();

            if(process != nullptr) {
                process->closeStdin();
                process->cancelIo();
                try {
                    process->terminateTree();
                } catch(...) {
                    // open 的原始失败优先，清理错误不能替换根因。
                }
            }
            joinWorkers();
            if(process != nullptr) {
                process->close();
            }

            lock.lock();
            process_.reset();
            lifecycle_ = TransportLifecycle::Closed;
            close_cv_.notify_all();
            throw;
        }
    }

    // prepareMessage 只执行确定性序列化和大小检查，不接触进程或发送队列。
    // nlohmann 的 strict 模式同时拒绝无法表示为合法 UTF-8 JSON 的字符串。
    // dump 使用无缩进紧凑形式，避免空白字节影响消息上限和测试确定性。
    // ensure_ascii 保持 false，使合法中文按 UTF-8 原样传输而不是强制转义。
    // 二进制或非法字符串类型的序列化异常统一归为本地协议构造失败。
    // 正文大小在追加 LF 前检查，发送与接收对同一 JSON 负载使用一致口径。
    // 此阶段允许在 open 之前执行，因为它不读取任何可变传输状态。
    // 返回对象未提交时可直接析构，绝不会留下进程写入或完成回调。
    std::unique_ptr<IMCPPreparedMessage> prepareMessage(const nlohmann::json& message,
                                                        const MCPTransportRequestContext& context) override {
        std::string payload;
        try {
            payload = message.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
        } catch(...) {
            throw MCPException(MCPErrorCode::ProtocolViolation, MCPClientState::Disconnected,
                               "stdio MCP 消息无法序列化为合法 UTF-8 JSON");
        }

        // max_message_bytes 只统计 JSON 正文，传输 framing 的单个 LF 不计入协议上限。
        if(payload.size() > limits_.max_message_bytes) {
            throw MCPException(MCPErrorCode::MessageLimitExceeded, MCPClientState::Disconnected,
                               "stdio MCP 消息超过配置的单条大小上限");
        }
        payload.push_back('\n');
        return std::make_unique<StdioPreparedMessage>(std::move(payload), context);
    }

    // commitPrepared 的成功返回就是 Submitted 线性化点。
    // 本方法只持有短生命周期队列锁，不写管道，也绝不同步调用 Client 回调。
    // 空指针与跨 Transport 对象属于调用方接口错误，使用 invalid_argument 明确拒绝。
    // 生命周期在队列锁内检查，避免通过检查后被 close 插入队列。
    // stop_requested 覆盖已发生终命事件但尚未显式 close 的 Faulted 窗口。
    // 队列满时没有任何字节进入 stdin，因此失败仍处于 NotSubmitted 一侧。
    // push_back 是唯一提交点；只有它成功后消息才归物理发送 Worker 所有。
    // 唤醒发生在解锁之后，Writer 不会因条件变量调度而与提交者争用同一锁。
    // Writer 出队与后续提交可以并行，但实际管道写始终只有一个线程。
    // 发送队列只保存紧凑正文与非敏感关联上下文，不保存 Server 配置。
    void commitPrepared(std::unique_ptr<IMCPPreparedMessage> prepared,
                        std::chrono::steady_clock::time_point request_deadline) override {
        if(!prepared) {
            throw std::invalid_argument("stdio MCP 准备消息不能为空");
        }
        auto* stdio_prepared = dynamic_cast<StdioPreparedMessage*>(prepared.get());
        if(stdio_prepared == nullptr) {
            throw std::invalid_argument("stdio MCP 收到了其他 Transport 的准备消息");
        }

        std::unique_lock<std::mutex> lock(mutex_);
        if(lifecycle_ != TransportLifecycle::Open || stop_requested_) {
            throw MCPException(MCPErrorCode::OperationCancelled, MCPClientState::Closed,
                               "stdio MCP Transport 已停止接收消息");
        }
        if(outbound_queue_.size() >= limits_.max_pending_messages) {
            throw MCPException(MCPErrorCode::MessageQueueOverflow, MCPClientState::Faulted, "stdio MCP 发送队列已满");
        }

        // deque 插入失败不会改变现有队列；只有成功插入后才发布唤醒信号。
        // 请求段只在此提交点开始，Writer 取得的是不可变的已激活上下文。
        stdio_prepared->context_.deadline = request_deadline;
        outbound_queue_.push_back(OutboundMessage{std::move(stdio_prepared->line_), stdio_prepared->context_});
        lock.unlock();
        queue_cv_.notify_one();
    }

    // stdio framing 不携带协议版本头，协商结果只由 MCPClient 保存。
    // 保持显式空操作可让两个 Transport 共享同一初始化调用序列。
    void completeInitialization(const std::string&) override {}

    // stdio 没有后台 GET Listener；Client 已将其状态初始化为 NotApplicable。
    // 不发事件可避免误把 stdio 的固定状态当成一次异步 Listener 状态迁移。
    void startListener(std::chrono::steady_clock::time_point) override {}

    // close 先禁止新写入并关闭 stdin，再等待正常退出，最后终止完整进程树。
    // 回调在进入 Closing 时立即撤销，全部 Worker 收敛后才发布 Closed。
    // Created 可直接关闭，因为构造阶段没有启动线程、进程或管道。
    // 并发关闭只有首个调用者拥有清理权，防止重复 terminate 与重复 join。
    // 清空待发送消息不会发 SendFailed；Client 的 Closing 状态已经确定完成等待。
    // closeStdin 既表达正常 EOF，也负责取消可能卡在平台写调用中的 Writer。
    // Writer 和 Monitor 先退出，保证正常等待期间不会再产生新写或退出事件。
    // stdout 与 stderr 在优雅等待期间继续排空，避免 Server 因输出管道反压挂起。
    // shutdown_timeout 受 close_timeout 剩余预算约束，配置不能扩大总关闭上限。
    // 正常等待失败与超时都进入相同进程树终止路径，不泄漏平台异常文本。
    // terminateTree 针对完整 Job Object 或进程组，不只处理根进程 PID。
    // 强制终止后仍尝试 wait，确保 POSIX 根进程被回收而不是形成僵尸。
    // cancelIo 放在退出判定后，避免过早丢弃 Server 正常关闭阶段的输出。
    // 两个读 Worker join 后才调用 Process::close，保证原生句柄没有并发使用者。
    // stderr 尾部随关闭清除，秘密字节不会跨越 Transport 生命周期保留。
    // Closed 与条件变量通知是最后动作，等待者观察到的对象已经完全收敛。
    void close(std::chrono::steady_clock::time_point absolute_deadline) noexcept override {
        std::unique_lock<std::mutex> lock(mutex_);
        if(lifecycle_ == TransportLifecycle::Closed) {
            return;
        }
        if(lifecycle_ == TransportLifecycle::Created) {
            callbacks_ = {};
            lifecycle_ = TransportLifecycle::Closed;
            close_cv_.notify_all();
            return;
        }
        if(lifecycle_ == TransportLifecycle::Closing) {
            // 并发关闭者不接管资源，只在共同总预算内等待所有者完成。
            close_cv_.wait_until(lock, close_deadline_, [this] { return lifecycle_ == TransportLifecycle::Closed; });
            return;
        }

        lifecycle_ = TransportLifecycle::Closing;
        stop_requested_ = true;
        callbacks_ = {};
        outbound_queue_.clear();
        Process* process = process_.get();
        // 首个关闭所有者固定调用方截止点；后续阶段和并发关闭者只能消费剩余预算。
        close_deadline_ = absolute_deadline;
        const auto close_deadline = close_deadline_;
        queue_cv_.notify_all();
        lock.unlock();

        if(process != nullptr) {
            // closeStdin 会取消正在进行的唯一写操作，使 writer 能够确定收敛。
            process->closeStdin();
        }
        joinThread(writer_worker_);
        joinThread(monitor_worker_);

        bool exited = process == nullptr;
        int ignored_exit_code = 0;
        if(process != nullptr) {
            try {
                const auto graceful_wait = std::min(config_.shutdown_timeout, remainingMilliseconds(close_deadline));
                exited = process->waitForExit(graceful_wait, ignored_exit_code);
            } catch(...) {
                // 等待失败直接进入强制终止，不把平台诊断传播出 noexcept 边界。
                exited = false;
            }

            if(!exited) {
                try {
                    process->terminateTree();
                } catch(...) {
                    // Process::close 仍会通过 Job Object 或进程组执行最后兜底。
                }
                try {
                    process->waitForExit(remainingMilliseconds(close_deadline), ignored_exit_code);
                } catch(...) {
                    // 关闭合同只保证尽力清理，不公开平台异常或敏感路径。
                }
            }

            // stdout 与 stderr 在正常退出期间持续排空，强制阶段才统一取消阻塞读。
            process->cancelIo();
        }
        joinThread(stdout_worker_);
        joinThread(stderr_worker_);
        if(process != nullptr) {
            process->close();
        }

        lock.lock();
        process_.reset();
        stderr_tail_.clear();
        lifecycle_ = TransportLifecycle::Closed;
        close_cv_.notify_all();
    }

   private:
    // isStopping 是 Worker 的统一快速退出判定。
    // 所有调用都很短，避免在平台读写或回调期间持有 Transport 锁。
    // Opening 期间 Worker 尚未获得锁，Closed 以后也不会再次进入任何循环。
    // 终命事件把 stop_requested 置位，但保留 Open 供首个终命回调完成投递。
    bool isStopping() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return lifecycle_ != TransportLifecycle::Open || stop_requested_;
    }

    // emitEvent 先复制回调再解锁执行，隔离 Client 回调与 Transport 内部锁序。
    // Closing 撤销回调后，即使迟到平台操作完成也不会继续进入 MCPClient。
    // std::function 复制本身可能分配，因此也封闭在 noexcept 的异常隔离范围内。
    // 回调副本拥有独立生命周期，调用期间不需要保持 callbacks_ 成员有效。
    // 事件对象只含闭合错误码和内部 dispatch，不携带 stderr 或平台诊断。
    // Client 回调异常不会改变管道状态，也不会导致 std::terminate 结束宿主进程。
    void emitEvent(MCPTransportEvent event) noexcept {
        std::function<void(MCPTransportEvent)> callback;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if(lifecycle_ != TransportLifecycle::Open) {
                return;
            }
            callback = callbacks_.on_event;
        } catch(...) {
            return;
        }
        if(callback) {
            try {
                callback(std::move(event));
            } catch(...) {
                // Transport 回调边界不得因上层异常终止 Worker 或宿主进程。
            }
        }
    }

    // emitMessage 与事件采用相同锁外规则，JSON 值按所有权移动给 MCPClient。
    // 协议关联和 Server 请求响应由上层执行，stdout Worker 不调用 ToolHandler。
    // stop_requested 检查阻止终命原因选定后的后续粘包消息继续进入协议层。
    // nlohmann::json 的移动避免为大 Schema 再复制一次完整消息树。
    // 上层若拒绝消息会自行发布 Client 故障，Transport 不猜测业务状态。
    // 回调返回后 stdout Worker 才处理下一行，保持同一流内的严格到达顺序。
    void emitMessage(nlohmann::json message) noexcept {
        std::function<void(nlohmann::json)> callback;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if(lifecycle_ != TransportLifecycle::Open || stop_requested_) {
                return;
            }
            callback = callbacks_.on_message;
        } catch(...) {
            return;
        }
        if(callback) {
            try {
                callback(std::move(message));
            } catch(...) {
                // MCPClient 自身会归一化协议异常；额外异常不允许越过线程入口。
            }
        }
    }

    // signalTerminal 只允许首个不可恢复原因获得终命事件所有权。
    // 它会停止出站队列并取消管道等待，但资源析构仍由 close 统一完成。
    // 首因优先可避免 ProtocolViolation 被随后进程退出覆盖为 ServerExited。
    // 队列在状态锁下同时清空，终命事件之后不会再有新物理写启动。
    // 活动写若已离开队列，由 cancelIo 和 Process 的完整写合同确定结束。
    // 回调先于 cancelIo 执行，使等待请求尽快取得稳定失败分类。
    // cancelIo 是 noexcept 唤醒机制，不执行超时等待或 Worker join。
    // Server 收到 stdin 关闭后可自行退出；忽略 EOF 的进程由显式 close 终止。
    void signalTerminal(MCPTransportEventType type, MCPErrorCode code) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(lifecycle_ != TransportLifecycle::Open || terminal_event_sent_) {
                return;
            }
            terminal_event_sent_ = true;
            stop_requested_ = true;
            outbound_queue_.clear();
        }
        queue_cv_.notify_all();

        MCPTransportEvent event;
        event.type = type;
        event.error_code = code;
        emitEvent(std::move(event));

        // 关闭管道可唤醒其他 Worker，并让正常 Server 通过 stdin EOF 自行退出。
        Process* process = process_.get();
        if(process != nullptr) {
            process->cancelIo();
        }
    }

    // writerLoop 是 stdin 的唯一拥有者，逐条执行完整行写入。
    // SendCompleted 只说明物理写成功，不代表 Server 已返回 JSON-RPC 终局结果。
    // 条件变量谓词同时覆盖队列数据、终命停止和生命周期关闭，避免丢唤醒。
    // 出队发生在状态锁内，完整写发生在锁外，不阻塞 commitPrepared 或 close 判定。
    // Process::writeStdin 要么写完全部字节要么抛出，不会报告部分成功状态。
    // 每条完成事件沿用准备阶段 dispatch_id，JSON-RPC request id 仍留在正文内。
    // 写失败后连接不能安全跳过消息继续发送，因此立即选择传输终命原因。
    // SendFailed 先行可让控制消息等待者关联具体发送，再由 Fault 关闭全局状态。
    void writerLoop() noexcept {
        while(true) {
            OutboundMessage outbound;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                queue_cv_.wait(lock, [this] {
                    return stop_requested_ || lifecycle_ != TransportLifecycle::Open || !outbound_queue_.empty();
                });
                if(stop_requested_ || lifecycle_ != TransportLifecycle::Open) {
                    return;
                }
                outbound = std::move(outbound_queue_.front());
                outbound_queue_.pop_front();
            }

            try {
                process_->writeStdin(outbound.line);
                MCPTransportEvent event;
                event.type = MCPTransportEventType::SendCompleted;
                event.dispatch_id = outbound.context.dispatch_id;
                emitEvent(std::move(event));
            } catch(...) {
                // 先关联当前发送失败，再用终命传输事件关闭整个 stdio 连续性。
                MCPTransportEvent event;
                event.type = MCPTransportEventType::SendFailed;
                event.dispatch_id = outbound.context.dispatch_id;
                event.error_code = MCPErrorCode::TransportFailure;
                emitEvent(std::move(event));
                signalTerminal(MCPTransportEventType::TransportFault, MCPErrorCode::TransportFailure);
                return;
            }
        }
    }

    // stdoutLoop 跨平台读分块累计行，既不假设一次读取对应一条消息，也不丢粘包。
    // 完整行兼容 LF/CRLF；空行、非法 JSON、超限与 EOF 残尾都是协议违规。
    // 固定 8 KiB 栈缓冲限制单次读取内存，pending 只增长到单消息上限附近。
    // reserve 仅采用较小值，超大配置不会在连接建立时一次性分配全部上限。
    // readStdout 返回零同时覆盖 EOF 与协作取消，停止标志负责区分关闭路径。
    // EOF 残尾即使恰好是合法 JSON 也不能执行，因为 stdio framing 尚未完成。
    // 没有残尾的意外 EOF 归类 ServerExited，退出码不会进入公开事件。
    // 每个 LF 先截取视图，CR 只在紧邻 LF 时作为 CRLF framing 被剥离。
    // JSON 内真实换行不合法，合法字符串换行必须由反斜杠转义且不会影响分帧。
    // 空行不能当作保活；stdio 合同要求 stdout 上每一行都是协议消息。
    // 大小检查发生在 JSON 解析前，恶意大行不会触发昂贵语法树分配。
    // JSON 解析器负责 UTF-8 与完整语法验证，标量合法性由 JSON-RPC 层再判断。
    // 多条粘包消息依次回调，前一条导致 Fault 后会在下一条前检查停止状态。
    // 已消费前缀按读取块批量删除，避免逐行 erase 造成平方级复制。
    // 尚未收到 LF 时允许正文上限后紧跟一个 CR，以兼容跨块切分的 CRLF。
    // 任何额外字节都会立刻超过上限，不等待 Server 无限继续输出。
    // 分配或平台读取异常使用 TransportFailure，而语法和 framing 使用 ProtocolViolation。
    void stdoutLoop() noexcept {
        std::array<char, 8192U> chunk{};
        std::string pending;
        pending.reserve(std::min<std::size_t>(limits_.max_message_bytes, chunk.size()));

        try {
            while(!isStopping()) {
                const std::size_t read_size = process_->readStdout(chunk.data(), chunk.size());
                if(read_size == 0U) {
                    if(isStopping()) {
                        return;
                    }
                    if(!pending.empty()) {
                        signalTerminal(MCPTransportEventType::TransportFault, MCPErrorCode::ProtocolViolation);
                    } else {
                        signalTerminal(MCPTransportEventType::ServerExited, MCPErrorCode::ServerExited);
                    }
                    return;
                }

                pending.append(chunk.data(), read_size);
                std::size_t line_start = 0U;
                while(true) {
                    const std::size_t line_end = pending.find('\n', line_start);
                    if(line_end == std::string::npos) {
                        break;
                    }

                    std::string_view line(pending.data() + line_start, line_end - line_start);
                    if(!line.empty() && line.back() == '\r') {
                        line.remove_suffix(1U);
                    }
                    if(line.empty() || line.size() > limits_.max_message_bytes) {
                        signalTerminal(MCPTransportEventType::TransportFault, MCPErrorCode::ProtocolViolation);
                        return;
                    }

                    try {
                        // 这里只验证 JSON 与 UTF-8；JSON-RPC 形状、ID 和方法由协议层严格检查。
                        nlohmann::json message = nlohmann::json::parse(line.begin(), line.end());
                        emitMessage(std::move(message));
                    } catch(...) {
                        signalTerminal(MCPTransportEventType::TransportFault, MCPErrorCode::ProtocolViolation);
                        return;
                    }
                    if(isStopping()) {
                        return;
                    }
                    line_start = line_end + 1U;
                }

                // 每个网络分块最多执行一次前缀删除，避免多消息粘包形成平方复制。
                if(line_start != 0U) {
                    pending.erase(0U, line_start);
                }
                const bool valid_pending_cr = pending.size() > limits_.max_message_bytes &&
                                              pending.size() - limits_.max_message_bytes == 1U &&
                                              pending.back() == '\r';
                if(pending.size() > limits_.max_message_bytes && !valid_pending_cr) {
                    signalTerminal(MCPTransportEventType::TransportFault, MCPErrorCode::ProtocolViolation);
                    return;
                }
            }
        } catch(...) {
            if(!isStopping()) {
                signalTerminal(MCPTransportEventType::TransportFault, MCPErrorCode::TransportFailure);
            }
        }
    }

    // appendStderrTail 只保存最近的有界诊断字节，既不解析也不执行外部回调。
    // 大于容量的单块直接截取尾部，避免先创建超过配置上限的临时缓存。
    // 更新使用同一状态锁，关闭清空缓存时不会与 stderr Worker 发生数据竞争。
    // 容量由配置校验保证非零，因此尾部指针和差值运算始终有效。
    // 小块溢出时先删除最旧前缀，缓存大小在 append 后恰好不超过上限。
    // 原始字节不要求 UTF-8，因为 stderr 不参与协议，也不会进入公开文本。
    void appendStderrTail(const char* data, std::size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t capacity = config_.max_stderr_tail_bytes;
        if(size >= capacity) {
            stderr_tail_.assign(data + (size - capacity), capacity);
            return;
        }
        if(size > capacity - stderr_tail_.size()) {
            stderr_tail_.erase(0U, size - (capacity - stderr_tail_.size()));
        }
        stderr_tail_.append(data, size);
    }

    // stderrLoop 与 stdout 完全分离，持续排空日志管道以防 Server 因缓冲区写满阻塞。
    // stderr 内容属于潜在秘密，读取错误和原始字节均不进入协议或公开异常。
    // 固定 4 KiB 分块在高吞吐日志下保持有界栈内存和稳定系统调用规模。
    // stderr EOF 不代表协议 stdout 已关闭，因此不单独发布 ServerExited。
    // 显式回调在状态锁外执行，收到的 view 仅覆盖当前栈内分块；调用方负责复制和脱敏。
    // 回调异常按块隔离，不能停止后续排空，也不能覆盖 stdout 或进程退出根因。
    // 关闭期间返回的最后一块不会再缓存，缩短秘密在内存中的驻留时间。
    // 平台读取异常被隔离，根进程监视与 stdout 通道仍能选择准确终命原因。
    void stderrLoop() noexcept {
        std::array<char, 4096U> chunk{};
        try {
            while(!isStopping()) {
                const std::size_t read_size = process_->readStderr(chunk.data(), chunk.size());
                if(read_size == 0U || isStopping()) {
                    return;
                }
                appendStderrTail(chunk.data(), read_size);
                if(config_.stderr_callback) {
                    try {
                        config_.stderr_callback(std::string_view(chunk.data(), read_size));
                    } catch(...) {
                        // 外部诊断回调不属于协议控制面，异常只能影响当前诊断分块。
                    }
                }
            }
        } catch(...) {
            // stderr 不是协议通道，其读取失败不能覆盖 stdout 或进程退出根因。
        }
    }

    // monitorLoop 发现根进程退出时发布稳定 ServerExited，而不暴露退出码。
    // 短轮询使 close 能在固定时间内收敛，同时避免为每次等待新建线程。
    // 50 毫秒只影响故障发现延迟，不改变任何公开请求自身的截止时间。
    // waitForExit 在 POSIX 内部负责 waitpid 回收，在 Windows 读取稳定进程状态。
    // stdout 同时观察 EOF，两条路径由 signalTerminal 的首因门消除重复事件。
    // 根进程退出后短暂让 stdout 优先排空，确保 EOF 残尾仍归类 ProtocolViolation。
    // 排空窗口有界，子孙错误持有 stdout 时仍会及时发布 ServerExited。
    // close 先设置停止标志，Monitor 下一次轮询后无事件退出。
    // 等待系统调用异常代表进程状态不可可靠判断，因此提升为 TransportFailure。
    void monitorLoop() noexcept {
        try {
            while(!isStopping()) {
                int ignored_exit_code = 0;
                if(process_->waitForExit(std::chrono::milliseconds(50), ignored_exit_code)) {
                    // stdout EOF 通常紧随根进程退出；先给读取者一次有界的残留分类机会。
                    std::unique_lock<std::mutex> lock(mutex_);
                    queue_cv_.wait_for(lock, std::chrono::milliseconds(100),
                                       [this] { return stop_requested_ || lifecycle_ != TransportLifecycle::Open; });
                    if(stop_requested_ || lifecycle_ != TransportLifecycle::Open) {
                        return;
                    }
                    lock.unlock();
                    signalTerminal(MCPTransportEventType::ServerExited, MCPErrorCode::ServerExited);
                    return;
                }
            }
        } catch(...) {
            if(!isStopping()) {
                signalTerminal(MCPTransportEventType::TransportFault, MCPErrorCode::TransportFailure);
            }
        }
    }

    // joinThread 只用于外部关闭所有者；Worker 回调不得重入 Transport::close。
    // MCPClient 的回调实现遵守该约束，因此这里不需要分离仍访问对象的线程。
    // 默认构造或创建失败的 thread 不可 join，joinable 检查覆盖这些路径。
    // 不使用 detach，保证 Transport 销毁前没有任何线程继续访问成员。
    static void joinThread(std::thread& worker) noexcept {
        if(worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
            worker.join();
        }
    }

    // joinWorkers 用于 open 的部分失败路径，此时 cancelIo 已唤醒全部平台等待。
    // 顺序先处理写和监视，再处理两个读者，与正常 close 的所有权顺序一致。
    // 每个 join 都只执行一次，后续析构看到的 thread 已不再 joinable。
    void joinWorkers() noexcept {
        joinThread(writer_worker_);
        joinThread(monitor_worker_);
        joinThread(stdout_worker_);
        joinThread(stderr_worker_);
    }

    // 配置构造后只读，避免运行期命令、环境或上限被 Server 消息修改。
    // Transport 自己持有值副本，调用方后续修改原始配置不会形成竞态。
    MCPStdioServerConfig config_;
    MCPCommonLimits limits_;

    // mutex_ 同时保护生命周期、回调、发送队列和有界 stderr 尾部。
    // queue_cv_ 只唤醒唯一 Writer，close_cv_ 只协调多个并发关闭调用者。
    // terminal_event_sent_ 是首因门，不需要单独原子变量或第二套锁序。
    // outbound_queue_ 的容量在每次提交时检查，绝不依赖容器隐式扩容上限。
    mutable std::mutex mutex_;
    std::condition_variable queue_cv_;
    std::condition_variable close_cv_;
    TransportLifecycle lifecycle_ = TransportLifecycle::Created;
    bool stop_requested_ = false;
    bool terminal_event_sent_ = false;
    std::chrono::steady_clock::time_point close_deadline_{};
    MCPTransportCallbacks callbacks_;
    std::deque<OutboundMessage> outbound_queue_;
    std::string stderr_tail_;

    // Process 在全部 Worker join 后才释放，线程可安全使用稳定裸指针。
    // 四个 thread 是固定成员，消息数量不会增加线程或平台等待对象数量。
    // 成员声明顺序不承担清理语义，显式 close 始终在析构前完成 join。
    std::unique_ptr<Process> process_;
    std::thread writer_worker_;
    std::thread stdout_worker_;
    std::thread stderr_worker_;
    std::thread monitor_worker_;
};

}  // namespace

// 工厂只分配对象，不启动进程；实际 I/O 严格延迟到 open。
// 返回窄接口隐藏具体类、线程模型和 Process 平台实现。
// 配置按值复制，使工厂返回后对象拥有完整且稳定的启动合同。
std::shared_ptr<IMCPTransport> createStdioMCPTransport(const MCPStdioServerConfig& config,
                                                       const MCPCommonLimits& limits) {
    return std::make_shared<StdioMCPTransport>(config, limits);
}

}  // namespace aiSDK::detail
