#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "mcp/MCPClient.h"
#include "mcp/MCPToolAdapter.h"

namespace {

using namespace std::chrono_literals;

// 本文件只验证 MCPToolAdapter 的转换与注册边界，不访问真实网络或子进程。
// 假传输仍遵守 prepare/commit 两阶段合同，commit 不同步回调 Client。
// 单 Worker 按提交顺序处理 initialize、initialized、tools/list 与 tools/call。
// 工具目录固定提供文本、富媒体和业务失败三种结果，避免测试共享可变脚本。
// 每个测试重新建立 Client 和 Catalog，目录令牌不会跨测试复用。
// 调用计数只统计真正提交到传输的 tools/call，用于证明本地参数拒绝零写入。
// Listener 固定模拟 HTTP 405 降级，不占前台公开操作槽，也不引入重连时序。
// 测试不比较远端错误正文之外的内部异常，公开行为只通过 ToolResult 观察。
// 弱引用用例销毁 Client 后保留 Binding，证明注册表不会延长连接生命周期。
// 富媒体用例验证现有 ToolResult 无法无损承接的内容会得到固定失败。
// 业务失败用例验证 isError=true 的文本进入失败结果，而不是伪装成功 JSON。
// 注册冲突用例把冲突放在整批第二项，证明预检失败不会部分写入第一项。
// 极小文本上限使用一个字节，确保失败消息非空、有效且不突破调用方上限。
// 所有测试均离线、无环境变量依赖，并可在 Windows 与 POSIX 重复运行。
// 假配置仍通过公开 validateMCPServerConfig，避免测试绕过构造期安全合同。
// Endpoint 使用字面量回环和显式明文开关，但不会真正建立套接字。
// 请求超时使用秒级预算，既避免正常机器抖动也不会让失败长期挂起。
// close_timeout 大于凭据超时，满足 HTTP 配置的静态不变量。
// open 只允许调用一次，重复打开代表被测 Client 生命周期错误。
// open 安装回调后启动 Worker，保证后续提交总有唯一消费者。
// PreparedMessage 保存 JSON 与请求上下文，不提前增加工具调用计数。
// prepareMessage 只复制值，验证 Client 的 prepare 阶段不会产生提交副作用。
// commitPrepared 在一个锁区间内把完整值移入队列，模拟原子 Submitted 点。
// commit 在锁外通知 Worker，避免唤醒线程立即争抢同一临界区。
// close 设置终止标记并唤醒 Worker，空队列也能有界退出。
// close 先 join 再清回调，保证 Client 析构时没有后台访问。
// 析构再次调用 close，覆盖显式 close 后的幂等资源清理。
// Worker 等待条件同时观察停止和队列，防止丢失唤醒。
// Worker 在停止时不继续处理剩余脚本，符合关闭取消在途工作的语义。
// 每次循环只弹出一条完整消息，提交顺序就是响应顺序。
// processMessage 捕获所有异常，并转换成闭合 SendFailed 事件。
// initialize 响应使用 Client 期望的 2025-11-25 协议版本。
// initialize capabilities 明确包含 Tools，确保目录列举属于已协商能力。
// Tools 能力使用空对象，证明 Adapter 测试不依赖 listChanged 通知。
// initialized 通知以 SendCompleted 完成，不生成错误的 JSON-RPC 响应。
// list 响应不分页，避免 Adapter 用例混入 Client 分页行为。
// 每个工具都带对象 inputSchema，满足本地 ToolRegistry 注册合同。
// 三个工具描述相同，测试只关心名称到结果路径的映射。
// text 工具返回文本、结构化对象与 isError=false 的完整正常结果。
// rich 工具返回合法 image 内容块，协议层应保留，Adapter 应拒绝映射。
// business 工具返回合法 text 内容块与 isError=true 的业务失败。
// tool_call_count 在收到 tools/call 后、发送响应前增加，代表真正远端提交。
// 工具调用计数由互斥锁保护，测试线程读取不会与 Worker 写入竞争。
// 回调快照在锁内复制、锁外调用，遵守 Transport 禁止锁内回调的合同。
// ListenerUnsupported 使用 405 与 Unsupported 状态，模拟首版正常降级。
// Listener 事件 dispatch_id 为零，因为它不属于任何前台公开操作。
// SendCompleted 只用于无需 JSON 响应的 initialized 通知。
// 带 response 的 initialize、list 和 call 由 on_message 完成等待操作。
// SendFailed 使用 ProtocolViolation，只有假脚本自身出现异常才会触发。
// 测试不会依据假传输的动态异常文本判断行为。
// Fixture SetUp 按 connect、listTools 顺序取得真实签发 Catalog。
// Catalog 不是手工构造，Adapter 的 ownsCatalog 校验走真实 Client 路径。
// Fixture TearDown 只在 Client 尚存时 close，弱引用用例可提前销毁对象。
// transport 独立持有至 Fixture 销毁，便于 Client reset 后读取调用计数。
// 非对象参数使用数组，覆盖 ToolRegistry 允许但 MCP 禁止的顶层类型。
// 非对象用例先记录计数，失败后比较确保没有 tools/call 被提交。
// 非对象用例比较完整固定中文文本，防止再次被映射为连接不可用。
// 非对象用例不通过 ToolRegistry execute，直接锁定 Handler 自身语义。
// weak 用例先正常生成 Binding，确保 weak 引用来自有效 Client。
// weak 用例显式 close 再 reset，排除仍存活但 Closed 与真正过期的混淆。
// weak Handler 使用合法对象参数，失败必须来自 Client 生命周期而非输入。
// weak 用例比较固定连接文本，证明 lock 失败被安全收敛。
// 富媒体与业务失败放在同一 Client 会话，证明一次失败不污染后续调用。
// 富媒体断言不接触图片 data，Adapter 不应解码或回显媒体正文。
// 富媒体结果必须 success=false，不能把不支持内容当成空成功。
// 业务失败必须 success=false，不能只依赖 JSON-RPC error 表达失败。
// 业务失败应使用远端文本内容，在正常上限下保持完整可读。
// business 文本不含控制字符，本用例聚焦结果类别而非净化细节。
// 注册冲突用例生成两个 Binding，把无冲突项放在第一位置。
// 冲突名称 existing 由应用工具预先注册，模拟真实合并注册表。
// 已有工具使用合法对象 Schema 与可执行 Handler，不是畸形占位。
// registerBindings 应在扫描第二项时发现冲突并在首次写入前抛出。
// first_new 不存在证明批处理保持原子预检语义。
// existing 仍存在证明失败没有误删或覆盖应用已有工具。
// 注册冲突只要求 std::invalid_argument，调用方不应解析完整 what 文本。
// 极小上限用 rich 结果触发固定 Adapter 失败，而不是 Client 异常路径。
// max_error_text_bytes=1 是公开配置允许的最小正值。
// 一个字节不足以形成中文 UTF-8 码点，空字符串会丢失失败可见性。
// 单字符感叹号是中性受限标记，不携带远端正文或敏感信息。
// 极小用例同时断言非空具体值与字节上限，避免只满足其中一个条件。
// AdapterOptions 为每批 Binding 固定，Handler 执行时不读取可变外部配置。
// 目录共享优化通过代码结构保证，行为测试不依赖内部 shared_ptr 引用计数。
// 若未来增加内存分配探针，应另建性能测试而非暴露 Catalog 私有实现。
// 测试工具名称使用可移植 ASCII，避免名称校验掩盖被测结果映射。
// 本地别名也满足 64 字节上限，注册冲突只来自明确同名条件。
// risk_level 使用 std::nullopt，顺带走默认 High 但本文件不重复已有断言。
// 测试不触发 catalog stale，目录失效映射由 Client 集成测试单独覆盖。
// 测试不触发 OutcomeUnknown，提交后断链属于 Client 状态机集成范围。
// 测试不触发凭据 Provider，假 Transport 让 Adapter 结果完全确定。
// 测试不共享全局端口、文件、环境或静态可变计数。
// Worker 线程只在单个 Fixture 内存在，不跨测试保留回调。
// nlohmann JSON 字段使用 MCP 规范英文，说明和诊断保持简体中文。
// 所有测试名称直接描述公开行为，失败报告不依赖内部函数名称。
// 正常关闭由 TearDown 自动执行，即使 ASSERT 提前返回也会回收 Worker。
// EXPECT 失败不会阻止 TearDown，便于同一用例报告多个相关断言。
// 测试文件不定义 main，复用项目既有 gtest_main 链接约定。
// 文件只需链接 ai_sdk_mcp、GTest 与线程库，不引入 cpr 后端专用 API。
// 该测试可与核心 MCP 单元同进程运行，不需要 RUN_SERIAL。
// 假传输的队列是有界场景：每次公开操作等待完成后才提交下一条。
// 测试不覆盖队列溢出，资源上限属于 Transport 与 Client 专属测试。
// 测试结果只读取公开 ToolResult、ToolRegistry 与传输测试计数器。
// Catalog 私有令牌和 Client Impl 都不会通过测试辅助接口暴露。
// 若 Handler 意外抛异常，测试会失败而不是由假传输吞掉执行边界问题。
// 如果 fake 脚本字段漂移，ProtocolViolation 事件会让公开操作确定失败。
// 这些约束让 Adapter 回归能够在没有真实 MCP Server 时快速定位。
// 测试断言不依赖线程调度顺序，公开操作返回即代表对应响应已经完成。
// 每个 Fixture 的队列和回调均为实例成员，测试并行执行也不会共享状态。

// AdapterPreparedMessage 保存一条尚未提交给假 Worker 的完整消息。
// 值对象让 commit 可以原子移动正文，同时不依赖调用方 JSON 生命周期。
class AdapterPreparedMessage final : public aiSDK::IMCPPreparedMessage {
   public:
    AdapterPreparedMessage(nlohmann::json value, aiSDK::MCPTransportRequestContext request_context)
        : message(std::move(value)), context(request_context) {}

    nlohmann::json message;
    aiSDK::MCPTransportRequestContext context;
};

// AdapterTransport 提供 Adapter 测试所需的最小完整 MCP 会话。
// 回调总在 Worker 或 startListener 调用栈执行，并且都发生在内部锁外。
// Fake 只模拟协议时序，不复制 stdio 或 HTTP 的平台资源实现。
// open 仅创建一个固定 Worker，使测试调度不随工具数量增加线程。
// 准备阶段只构造值对象，提交前发生的目录变化必须保持零写入。
// commit 是唯一提交线性化点，消息进入队列后才允许 Worker 观察。
// Worker 串行处理初始化、目录和调用请求，响应顺序因此完全确定。
// Listener 走 405 正常降级事件，避免 Adapter 测试依赖远程网络。
// 回调副本在锁内取得、锁外执行，保持与生产 Transport 相同的锁序。
// close 先发布停止标志再唤醒 Worker，确保空队列等待能够退出。
// close 返回前必须 join Worker，保证 Client 析构后没有迟到回调。
// 重复 close 只观察停止标志，不会再次回收线程或清空活动对象。
// Fake 不解释工具 Schema，相关校验仍由 Adapter 与 ToolRegistry 承担。
// 所有错误文本固定且不含工具参数，测试失败不会泄露调用正文。
class AdapterTransport final : public aiSDK::IMCPTransport {
   public:
    ~AdapterTransport() override {
        close(std::chrono::steady_clock::now());
    }

    // Fake 接受 Client 生成的绝对打开截止点，但确定性内存启动不会主动消耗它。
    // 签名仍保留截止参数，以便测试覆盖与生产 Transport 完全相同的抽象合同。
    void open(aiSDK::MCPTransportCallbacks callbacks, std::chrono::steady_clock::time_point) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if(opened_) {
            throw std::logic_error("Adapter 假传输不能重复打开");
        }
        callbacks_ = std::move(callbacks);
        opened_ = true;
        worker_ = std::thread([this] { workerLoop(); });
    }

    std::unique_ptr<aiSDK::IMCPPreparedMessage> prepareMessage(
        const nlohmann::json& message, const aiSDK::MCPTransportRequestContext& context) override {
        // 准备阶段只分配本地值，不改变提交计数或触发 Client 回调。
        return std::make_unique<AdapterPreparedMessage>(message, context);
    }

    void commitPrepared(std::unique_ptr<aiSDK::IMCPPreparedMessage> prepared,
                        std::chrono::steady_clock::time_point request_deadline) override {
        auto* adapter_message = dynamic_cast<AdapterPreparedMessage*>(prepared.get());
        if(adapter_message == nullptr) {
            throw std::invalid_argument("Adapter 假传输收到未知准备消息");
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(stopping_) {
                throw aiSDK::MCPException(aiSDK::MCPErrorCode::OperationCancelled, aiSDK::MCPClientState::Closing,
                                          "Adapter 假传输已关闭");
            }
            // 与生产 Transport 一样，Fake 只在提交点激活该消息的请求段截止时间。
            adapter_message->context.deadline = request_deadline;
            queue_.push_back({std::move(adapter_message->message), adapter_message->context});
        }
        cv_.notify_one();
    }

    void completeInitialization(const std::string&) override {
        // 测试只要求 Client 完成版本协商，不需要在传输层再次解释版本。
    }

    void startListener(std::chrono::steady_clock::time_point) override {
        const aiSDK::MCPTransportCallbacks callbacks = callbacksSnapshot();
        // 405 是受支持的正常降级；该事件不改变 Ready 主状态。
        callbacks.on_event({aiSDK::MCPTransportEventType::ListenerUnsupported, 0U, aiSDK::MCPErrorCode::HttpStatusError,
                            405, aiSDK::MCPListenerState::Unsupported});
    }

    // Fake 关闭没有网络或进程等待，因此只需接受并透传统一截止语义。
    // Worker 必须在返回前 join，测试不会通过 detach 绕过对象生命周期安全。
    void close(std::chrono::steady_clock::time_point) noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(stopping_) {
                return;
            }
            stopping_ = true;
        }
        cv_.notify_all();
        if(worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
            worker_.join();
        }
        // join 后清空回调，保证 close 返回后不再访问已析构 Client。
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_ = {};
    }

    std::size_t toolCallCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tool_call_count_;
    }

   private:
    struct QueuedMessage {
        nlohmann::json message;
        aiSDK::MCPTransportRequestContext context;
    };

    void workerLoop() noexcept {
        while(true) {
            QueuedMessage queued;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if(stopping_) {
                    return;
                }
                queued = std::move(queue_.front());
                queue_.pop_front();
            }
            processMessage(queued);
        }
    }

    void processMessage(const QueuedMessage& queued) noexcept {
        try {
            const std::string method = queued.message.value("method", std::string{});
            if(method == "initialize") {
                emitInitialize(queued);
            } else if(method == "notifications/initialized") {
                emitCompleted(queued.context.dispatch_id);
            } else if(method == "tools/list") {
                emitTools(queued);
            } else if(method == "tools/call") {
                emitToolResult(queued);
            }
        } catch(...) {
            emitFailure(queued.context.dispatch_id);
        }
    }

    void emitInitialize(const QueuedMessage& queued) {
        emitMessage({
            {"jsonrpc", "2.0"                                                                                       },
            {"id",      queued.message.at("id")                                                                     },
            {"result",  {{"protocolVersion", "2025-11-25"}, {"capabilities", {{"tools", nlohmann::json::object()}}}}}
        });
    }

    void emitTools(const QueuedMessage& queued) {
        // 三个远端工具共享对象 Schema，但保留独立名称以选择不同结果路径。
        const nlohmann::json tools = nlohmann::json::array({makeTool("text"), makeTool("rich"), makeTool("business")});
        emitMessage({
            {"jsonrpc", "2.0"                  },
            {"id",      queued.message.at("id")},
            {"result",  {{"tools", tools}}     }
        });
    }

    void emitToolResult(const QueuedMessage& queued) {
        const std::string name = queued.message.at("params").at("name").get<std::string>();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++tool_call_count_;
        }

        nlohmann::json result;
        if(name == "rich") {
            result = {
                {"content",
                 nlohmann::json::array({{{"type", "image"}, {"data", "aW1hZ2U="}, {"mimeType", "image/png"}}})},
                {"isError", false                                                                             }
            };
        } else if(name == "business") {
            result = {
                {"content", nlohmann::json::array({{{"type", "text"}, {"text", "远端业务拒绝"}}})},
                {"isError", true                                                                       }
            };
        } else {
            result = {
                {"content",           nlohmann::json::array({{{"type", "text"}, {"text", "调用成功"}}})},
                {"structuredContent", {{"accepted", true}}                                                 },
                {"isError",           false                                                                }
            };
        }
        emitMessage({
            {"jsonrpc", "2.0"                  },
            {"id",      queued.message.at("id")},
            {"result",  std::move(result)      }
        });
    }

    static nlohmann::json makeTool(const std::string& name) {
        return {
            {"name",        name                        },
            {"description", "Adapter 边界测试工具"},
            {"inputSchema", {{"type", "object"}}        }
        };
    }

    aiSDK::MCPTransportCallbacks callbacksSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return callbacks_;
    }

    void emitMessage(nlohmann::json message) {
        const aiSDK::MCPTransportCallbacks callbacks = callbacksSnapshot();
        callbacks.on_message(std::move(message));
    }

    void emitCompleted(std::uint64_t dispatch_id) {
        const aiSDK::MCPTransportCallbacks callbacks = callbacksSnapshot();
        callbacks.on_event({aiSDK::MCPTransportEventType::SendCompleted, dispatch_id,
                            aiSDK::MCPErrorCode::TransportFailure, 0, aiSDK::MCPListenerState::NotApplicable});
    }

    void emitFailure(std::uint64_t dispatch_id) {
        const aiSDK::MCPTransportCallbacks callbacks = callbacksSnapshot();
        callbacks.on_event({aiSDK::MCPTransportEventType::SendFailed, dispatch_id,
                            aiSDK::MCPErrorCode::ProtocolViolation, 0, aiSDK::MCPListenerState::NotApplicable});
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    aiSDK::MCPTransportCallbacks callbacks_;
    std::deque<QueuedMessage> queue_;
    std::thread worker_;
    bool opened_ = false;
    bool stopping_ = false;
    std::size_t tool_call_count_ = 0U;
};

// makeAdapterConfig 只用于注入假传输，Endpoint 不会被访问。
// 回环 HTTP 明确开启，保证公共配置校验仍走真实安全规则。
aiSDK::MCPServerConfig makeAdapterConfig() {
    aiSDK::MCPServerConfig config;
    config.server_id = "adapter";
    config.limits.request_timeout = 1s;
    config.limits.absolute_request_timeout = 3s;
    config.limits.close_timeout = 1s;
    aiSDK::MCPStreamableHttpConfig http;
    http.endpoint = "http://127.0.0.1:65529/mcp";
    http.allow_loopback_http = true;
    http.credential_timeout = 100ms;
    config.transport = std::move(http);
    return config;
}

// MCPAdapterTest 为每个测试签发独立目录并在结束时显式关闭 Client。
// 个别弱引用测试会提前 reset Client，TearDown 对空指针保持幂等。
class MCPAdapterTest : public ::testing::Test {
   protected:
    void SetUp() override {
        transport = std::make_shared<AdapterTransport>();
        client = std::make_shared<aiSDK::MCPClient>(makeAdapterConfig(), transport);
        client->connect();
        catalog = client->listTools();
    }

    void TearDown() override {
        if(client) {
            client->close();
        }
    }

    std::shared_ptr<AdapterTransport> transport;
    std::shared_ptr<aiSDK::MCPClient> client;
    aiSDK::MCPToolCatalog catalog;
};

// 非对象参数是本地调用合同错误，不能被 std::invalid_argument 的继承关系误报成连接不可用。
// 失败发生在 Client 锁定和传输提交之前，因此远端工具调用计数必须保持不变。
TEST_F(MCPAdapterTest, 非对象参数返回准确固定失败且不调用远端) {
    const auto bindings = aiSDK::MCPToolAdapter::adaptTools(client, catalog,
                                                            {
                                                                {"text", "local_text", std::nullopt}
    });
    const std::size_t calls_before = transport->toolCallCount();

    const aiSDK::ToolResult result = bindings.front().handler(nlohmann::json::array({1, 2}));
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_message, "MCP 工具参数必须是 JSON 对象");
    EXPECT_EQ(transport->toolCallCount(), calls_before);
}

// Binding 只持有 weak Client；连接对象销毁后 Handler 必须稳定失败而不是悬空访问。
TEST_F(MCPAdapterTest, Binding弱引用不会延长Client生命周期) {
    const auto bindings = aiSDK::MCPToolAdapter::adaptTools(client, catalog,
                                                            {
                                                                {"text", "weak_text", std::nullopt}
    });
    client->close();
    client.reset();

    const aiSDK::ToolResult result = bindings.front().handler(nlohmann::json::object());
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_message, "该 MCP 连接不可用");
}

// 富媒体无法无损映射到现有 ToolResult，业务失败则应保留 Server 返回的文本原因。
TEST_F(MCPAdapterTest, 富媒体与业务失败使用不同稳定映射) {
    const auto bindings = aiSDK::MCPToolAdapter::adaptTools(
        client, catalog,
        {
            {"rich",     "local_rich",     std::nullopt},
            {"business", "local_business", std::nullopt}
    });

    const aiSDK::ToolResult rich = bindings[0].handler(nlohmann::json::object());
    EXPECT_FALSE(rich.success);
    EXPECT_EQ(rich.error_message, "暂不支持 MCP 富媒体工具结果");

    const aiSDK::ToolResult business = bindings[1].handler(nlohmann::json::object());
    EXPECT_FALSE(business.success);
    EXPECT_EQ(business.error_message, "远端业务拒绝");
}

// 第二个 Binding 与已有工具冲突时，整批注册必须在任何写入前失败。
TEST_F(MCPAdapterTest, 注册冲突不会留下部分Binding) {
    const auto bindings =
        aiSDK::MCPToolAdapter::adaptTools(client, catalog,
                                          {
                                              {"text", "first_new", std::nullopt},
                                              {"rich", "existing",  std::nullopt}
    });
    aiSDK::Tool existing;
    existing.name = "existing";
    existing.description = "既有本地工具";
    existing.parameters = nlohmann::json::object();
    aiSDK::ToolRegistry registry;
    registry.registerTool(
        existing, [](const nlohmann::json&) { return aiSDK::ToolResult::successResult(nlohmann::json::object()); });

    EXPECT_THROW(aiSDK::MCPToolAdapter::registerBindings(registry, bindings), std::invalid_argument);
    EXPECT_FALSE(registry.hasTool("first_new"));
    EXPECT_TRUE(registry.hasTool("existing"));
}

// 一个字节放不下中文码点，Adapter 仍需返回非空、受限且可安全传递的失败标记。
TEST_F(MCPAdapterTest, 极小错误文本上限仍返回非空失败) {
    aiSDK::MCPToolAdapterOptions options;
    options.max_error_text_bytes = 1U;
    const auto bindings = aiSDK::MCPToolAdapter::adaptTools(client, catalog,
                                                            {
                                                                {"rich", "tiny_rich", std::nullopt}
    },
                                                            options);

    const aiSDK::ToolResult result = bindings.front().handler(nlohmann::json::object());
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_message, "!");
    EXPECT_LE(result.error_message.size(), options.max_error_text_bytes);
}

// Adapter 构造期异常也可能处理 Server 提供的名称，不能回显控制字符或敏感哨兵。
// 固定诊断同时避免超长远端名称绕过运行期 max_error_text_bytes 边界。
TEST_F(MCPAdapterTest, 构造与注册校验异常不回显不可信工具名称) {
    const std::string untrusted_name = "敏感哨兵\r\n" + std::string(8192U, 'x');

    try {
        static_cast<void>(aiSDK::MCPToolAdapter::adaptTools(client, catalog,
                                                            {
                                                                {untrusted_name, std::nullopt, std::nullopt}
        }));
        FAIL() << "不存在的远端名称必须被拒绝";
    } catch(const std::invalid_argument& exception) {
        const std::string message = exception.what();
        EXPECT_EQ(message.find("敏感哨兵"), std::string::npos);
        EXPECT_EQ(message.find('\r'), std::string::npos);
        EXPECT_EQ(message.find('\n'), std::string::npos);
        EXPECT_LT(message.size(), 256U);
    }

    const auto bindings = aiSDK::MCPToolAdapter::adaptTools(client, catalog,
                                                            {
                                                                {"text", "safe_alias", std::nullopt}
    });
    aiSDK::ToolRegistry registry;
    registry.registerTool(bindings.front().tool, bindings.front().handler);
    try {
        aiSDK::MCPToolAdapter::registerBindings(registry, bindings);
        FAIL() << "注册名称冲突必须被拒绝";
    } catch(const std::invalid_argument& exception) {
        EXPECT_EQ(std::string(exception.what()).find("safe_alias"), std::string::npos);
    }
}

}  // namespace
