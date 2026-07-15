#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

#include "mcp/MCPClient.h"

namespace {

using namespace std::chrono_literals;

// 本文件从公开 MCPClient 入口启动真实夹具进程，不注入脚本 Transport。
// 用例因此同时验证配置校验、平台进程、stdio 分帧、协议关联和关闭收敛。
// 所有路径来自 CTest 传参或临时目录，测试不依赖 PATH 搜索和当前工作目录。
// 临时根目录刻意包含中文与空格，覆盖 Windows 宽字符和 POSIX UTF-8 路径。
// 夹具可执行文件复制到临时目录，证明启动逻辑使用配置绝对路径而非原位置。
// argv 标签包含中文与空格，预言机从 tools/call 结果读取 Server 实际观察值。
// 子进程环境通过显式键值传入，结果必须与配置完全一致。
// 工作目录预言机来自 Server 的 current_path，不接受 Client 自己回显配置。
// stderr 回调保存原始分块后故意抛错，证明诊断回调不能破坏协议结果。
// Server 在 initialized 后反向 ping，证明后台请求响应与前台操作可并存。
// tools/list 返回两页，目录数量和顺序断言覆盖 cursor 分页通路。
// tools/call 创建的长期后代 PID 用平台 API 独立检查，防止只验证父进程退出。
// close 耗时有明确上限，避免不合作 Server 把测试进程永久挂起。
// 提前退出用例要求主状态 Faulted，随后 close 仍必须幂等进入 Closed。
// 资源用例预热运行时后重复二十五轮，降低动态库和线程首次初始化噪声。
// Windows 以当前进程句柄数作为资源预言机，Linux 使用 /proc/self/fd。
// 资源样本不能严格单调增长，最终值最多比基线多两个瞬时平台资源。
// 测试没有跳过分支；缺少夹具路径或平台预言机都会明确失败。
// 等待循环使用 steady_clock，系统时间调整不会改变存活判定截止时间。
// 轮询间隔固定为二十毫秒，在关闭上限内提供足够观测机会且避免忙等。
// 公开状态与 Listener 状态分别断言，stdio 必须始终报告 NotApplicable。
// 所有失败断言只检查稳定状态或结构字段，不解析平台错误正文。
// 临时目录析构清理使用 error_code，不能覆盖更早、更有价值的协议断言。
// 测试对象均在单个用例内持有，避免连接和回调跨用例泄漏共享状态。
// fixture_path 只在 main 初始化一次，此后作为只读绝对路径使用。
// 每个 Client 只发起一个公开操作，集成用例不绕过单操作在途限制。
// Listener 状态的 NotApplicable 断言把 stdio 与 HTTP 后台监听语义明确隔离。
// stderr 回调异常只验证隔离边界，测试不依赖回调被调用的具体分块次数。
// 分页目录在调用前完整获取，工具调用不会隐式刷新或替换 Catalog。
// 强制关闭用例把后代 PID 从 Server 结果带回，避免依赖平台进程枚举权限。
// 提前退出后的 lastFailureCode 只要求存在，允许平台层保留准确的闭合分类。
// 资源测试先在静止点采样，活动 Client 和夹具子进程不会进入基线。
// 二十五轮数量足以暴露线性泄漏，同时保持本地验证时长可控。
// 测试入口拒绝缺失夹具参数，确保 CTest 注册错误不会产生假通过。
// 正常、异常和强制终止三条关闭路径均通过同一公开 close 接口验证。
// 测试不直接访问 Transport 实现对象，资源行为只能从公开状态和平台预言机观察。
// 所有超时断言保留调度余量，不把机器瞬时负载误判为协议语义失败。

// main 在 GTest 启动前保存 CTest 传入的夹具绝对路径，测试不依赖进程环境变量。
std::filesystem::path fixture_path;

// TemporaryTree 为每个用例创建包含中文和空格的独立目录，并在所有退出路径清理。
// 清理失败不覆盖原测试断言，避免 Windows 杀进程后的短暂文件占用掩盖根因。
class TemporaryTree {
   public:
    TemporaryTree() {
        // steady_clock 计数只用于降低并发测试目录碰撞概率，不参与超时判断。
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        // 一次创建完整工作目录层级，失败时让 filesystem 异常明确终止用例。
        root_ = std::filesystem::temp_directory_path() / ("ai_sdk MCP 中文 空格 " + std::to_string(unique));
        std::filesystem::create_directories(root_ / "工作 目录");
    }

    ~TemporaryTree() {
        // 析构不能抛异常；Windows 进程回收延迟只会留下本次临时目录供诊断。
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    const std::filesystem::path& root() const noexcept {
        // 返回借用引用，生命周期受 TemporaryTree 限定且不会跨用例保存。
        return root_;
    }

    std::filesystem::path workingDirectory() const {
        // 按值返回便于直接写入配置，路径仍保持中文和空格组成。
        return root_ / "工作 目录";
    }

   private:
    std::filesystem::path root_;
};

std::filesystem::path copyFixture(const std::filesystem::path& source, const TemporaryTree& tree) {
    // 保留源扩展名使 Windows 能识别 .exe，文件主名刻意覆盖 Unicode 路径。
    const std::filesystem::path destination = tree.root() / ("stdio 服务器" + source.extension().u8string());
    // 目标目录每个用例唯一，overwrite 仅让失败重跑具备确定行为。
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
#ifndef _WIN32
    // POSIX copy_file 不保证目标保留执行位，显式补齐拥有者执行权限。
    std::filesystem::permissions(destination, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
#endif
    return destination;
}

// isProcessAlive 只用于测试预言机，不向生产代码传播平台进程类型。
// Windows 通过同步句柄判断，POSIX 用信号 0 区分不存在与无权限。
bool isProcessAlive(std::uint64_t pid) {
#ifdef _WIN32
    // 无效范围在调用 Win32 API 前拒绝，避免截断为另一个进程标识。
    if(pid == 0U || pid > static_cast<std::uint64_t>(std::numeric_limits<DWORD>::max())) {
        return false;
    }
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    // 打不开句柄按“已退出”处理；测试进程查询自己的后代不应遇到权限拒绝。
    if(process == nullptr) {
        return false;
    }
    const bool alive = WaitForSingleObject(process, 0U) == WAIT_TIMEOUT;
    // 每次探测立即关闭临时句柄，资源增长用例不会被预言机自身污染。
    CloseHandle(process);
    return alive;
#else
    // pid_t 范围校验防止 uint64_t 转换回绕到其他系统进程。
    if(pid == 0U || pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
        return false;
    }
    errno = 0;
    // EPERM 表示目标存在但不可发信号，仍应判定为存活。
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

bool waitForProcessExit(std::uint64_t pid, std::chrono::milliseconds timeout) {
    // 单一绝对截止点防止每轮轮询重新获得完整 timeout。
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while(std::chrono::steady_clock::now() < deadline) {
        if(!isProcessAlive(pid)) {
            // 首次观察到不存在即可成功，后续 PID 复用概率在该短窗口内可忽略。
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    // 截止点后再检查一次，覆盖进程恰好在最后一次 sleep 期间退出。
    return !isProcessAlive(pid);
}

// currentProcessResourceCount 为重复关闭提供平台原生资源预言机。
// Windows 统计当前进程句柄；Linux 统计 /proc/self/fd，其他 POSIX 平台明确失败。
std::size_t currentProcessResourceCount() {
#ifdef _WIN32
    // 句柄计数是进程级总量，预热与容差用于隔离测试框架的短暂活动。
    DWORD handle_count = 0U;
    if(!GetProcessHandleCount(GetCurrentProcess(), &handle_count)) {
        throw std::runtime_error("无法读取当前进程句柄数量");
    }
    return static_cast<std::size_t>(handle_count);
#else
    // /proc/self/fd 列举包含目录迭代自身 fd，因此最终比较保留小幅容差。
    const std::filesystem::path fd_directory("/proc/self/fd");
    if(!std::filesystem::is_directory(fd_directory)) {
        throw std::runtime_error("当前 POSIX 平台缺少 /proc/self/fd 资源预言机");
    }
    return static_cast<std::size_t>(
        std::distance(std::filesystem::directory_iterator(fd_directory), std::filesystem::directory_iterator{}));
#endif
}

aiSDK::MCPServerConfig makeStdioConfig(const std::filesystem::path& executable,
                                       const std::filesystem::path& working_directory,
                                       std::vector<std::string> arguments) {
    // 所有用例共享同一组短而有界的时间参数，失败不会拖长整个本地矩阵。
    aiSDK::MCPServerConfig config;
    config.server_id = "local_stdio";
    config.limits.request_timeout = 2s;
    config.limits.absolute_request_timeout = 8s;
    config.limits.close_timeout = 2s;

    aiSDK::MCPStdioServerConfig stdio;
    // executable 和 working_directory 都由临时树提供绝对路径。
    stdio.executable = executable;
    // arguments 按值移入配置，保留每一项原始边界且不经过命令字符串。
    stdio.arguments = std::move(arguments);
    stdio.working_directory = working_directory;
    stdio.environment.emplace("MCP_FIXTURE_ENV", "环境 中文 值");
    // 继承父环境仅为让系统运行时依赖保持可用；显式键必须覆盖为测试值。
    stdio.inherit_parent_environment = true;
    stdio.startup_timeout = 3s;
    stdio.shutdown_timeout = 300ms;
    // 赋给 variant 后配置完整，MCPClient 构造才执行确定性静态校验。
    config.transport = std::move(stdio);
    return config;
}

TEST(MCPStdioIntegrationTest, 真实子进程支持Unicode路径参数环境分页调用与强制关闭) {
    // current_test_info 证明当前代码确实位于 GTest 用例上下文，避免误直接调用。
    const auto* test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    ASSERT_NE(test_info, nullptr);
    // CTest 把夹具绝对路径放在 argv[1]，main 入口会先保存到全局测试环境变量。
    ASSERT_FALSE(fixture_path.empty());

    TemporaryTree tree;
    // 复制后的 executable 与原夹具内容相同，但路径形态覆盖目标边界。
    const auto executable = copyFixture(fixture_path, tree);
    std::mutex stderr_mutex;
    std::condition_variable stderr_cv;
    // stderr_text 只在 mutex 保护下读写，回调与测试线程不会形成数据竞争。
    std::string stderr_text;
    auto config = makeStdioConfig(executable, tree.workingDirectory(),
                                  {"--label", "参数 中文 空格", "--hang-on-eof", "--spawn-child"});
    auto& stdio = std::get<aiSDK::MCPStdioServerConfig>(config.transport);
    stdio.stderr_callback = [&](std::string_view chunk) {
        // 回调复制原始 view 后故意抛出，验证 Transport 隔离异常并继续完成协议交互。
        {
            std::lock_guard<std::mutex> lock(stderr_mutex);
            stderr_text.append(chunk.data(), chunk.size());
        }
        // 先通知再抛错，测试既能观察字节也能验证异常隔离。
        stderr_cv.notify_all();
        throw std::runtime_error("测试回调异常");
    };
    aiSDK::MCPClient client(std::move(config));

    // connect 必须完成握手和 initialized 通知后才返回 Ready。
    client.connect();
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Ready);
    EXPECT_EQ(client.listenerState(), aiSDK::MCPListenerState::NotApplicable);
    {
        // stderr Worker 独立调度，使用有界等待而不是依赖 connect 返回时序。
        std::unique_lock<std::mutex> lock(stderr_mutex);
        ASSERT_TRUE(stderr_cv.wait_for(
            lock, 1s, [&] { return stderr_text.find("stdio MCP 测试夹具已启动") != std::string::npos; }));
    }
    // ping 在反向 Server 请求之后执行，验证请求关联没有被字符串 id 干扰。
    client.ping();

    // 分页结果按 Server 页顺序合并，两个名称构成完整目录预言机。
    const auto catalog = client.listTools();
    ASSERT_EQ(catalog.tools().size(), 2U);
    EXPECT_EQ(catalog.tools()[0].name, "inspect");
    EXPECT_EQ(catalog.tools()[1].name, "second");

    // callTool 使用刚签发 Catalog，避免把目录过期行为混入 stdio 传输用例。
    const auto result = client.callTool(catalog, "inspect",
                                        {
                                            {"value", "调用 中文"}
    });
    // 下列断言分别对应 JSON 参数、argv、环境、工作目录四条输入链路。
    EXPECT_EQ(result.structured_content.at("arguments").at("value"), "调用 中文");
    EXPECT_EQ(result.structured_content.at("label"), "参数 中文 空格");
    EXPECT_EQ(result.structured_content.at("environment"), "环境 中文 值");
    EXPECT_EQ(std::filesystem::u8path(result.structured_content.at("workingDirectory").get<std::string>()),
              tree.workingDirectory());
    const std::uint64_t child_pid = result.structured_content.at("childPid").get<std::uint64_t>();
    // close 前后分别观察存活与退出，防止夹具根本没有成功创建后代的假阳性。
    ASSERT_TRUE(isProcessAlive(child_pid));

    // 计时从 close 调用前开始，断言覆盖 EOF 等待与强制终止总耗时。
    const auto close_start = std::chrono::steady_clock::now();
    client.close();
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Closed);
    EXPECT_LT(std::chrono::steady_clock::now() - close_start, 3s);
    EXPECT_TRUE(waitForProcessExit(child_pid, 2s));
}

TEST(MCPStdioIntegrationTest, Server提前退出使连接确定失败且可继续清理) {
    // 该用例不创建后代，专注握手前 EOF 如何映射为主状态故障。
    ASSERT_FALSE(fixture_path.empty());
    TemporaryTree tree;
    const auto executable = copyFixture(fixture_path, tree);
    aiSDK::MCPClient client(makeStdioConfig(executable, tree.workingDirectory(), {"--exit-immediately"}));

    // 具体底层根因允许按平台归类，但公开异常类型和 Faulted 终态必须稳定。
    EXPECT_THROW(client.connect(), aiSDK::MCPException);
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Faulted);
    ASSERT_TRUE(client.lastFailureCode().has_value());
    // 故障态 close 仍应释放残留管道和 Worker，并线性化到 Closed。
    client.close();
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Closed);
}

TEST(MCPStdioIntegrationTest, CRLF与stderr洪泛不阻塞握手且argv保持独立边界) {
    // 该用例把三种底层风险组合在一次真实握手中：CRLF 分帧、满 stderr 管道和非 Shell argv。
    // 标签含有命令解释器元字符与双引号；夹具回显必须逐字相同，不能出现二次解释或拆分。
    ASSERT_FALSE(fixture_path.empty());
    TemporaryTree tree;
    const auto executable = copyFixture(fixture_path, tree);
    std::mutex stderr_mutex;
    std::condition_variable stderr_cv;
    std::string stderr_text;
    const std::string label = "中文 空格 ; & | < > $() ^ % ! \"双引号\"";
    auto config =
        makeStdioConfig(executable, tree.workingDirectory(), {"--stdout-crlf", "--stderr-flood", "--label", label});
    auto& stdio = std::get<aiSDK::MCPStdioServerConfig>(config.transport);
    stdio.stderr_callback = [&](std::string_view chunk) {
        // 回调只复制测试预言机；生产 Transport 仍必须持续排空，而非依赖回调返回值。
        {
            std::lock_guard<std::mutex> lock(stderr_mutex);
            stderr_text.append(chunk.data(), chunk.size());
        }
        stderr_cv.notify_all();
    };
    aiSDK::MCPClient client(std::move(config));

    // 夹具会先写满两 MiB stderr；若没有独立 Worker，此处无法收到 initialize 成功响应。
    client.connect();
    {
        // 完成标记来自夹具真实输出，条件变量避免以任意休眠猜测 stderr 排空进度。
        std::unique_lock<std::mutex> lock(stderr_mutex);
        ASSERT_TRUE(stderr_cv.wait_for(lock, 1s,
                                       [&] { return stderr_text.find("STDERR_FLOOD_COMPLETE") != std::string::npos; }));
    }

    // tools/list 和 tools/call 的响应均采用 CRLF；目录与调用结果同时覆盖持续分帧。
    const auto catalog = client.listTools();
    ASSERT_EQ(catalog.tools().size(), 2U);
    const auto result = client.callTool(catalog, "inspect",
                                        {
                                            {"value", "CRLF 调用"}
    });
    EXPECT_EQ(result.structured_content.at("label"), label);
    EXPECT_EQ(result.structured_content.at("arguments").at("value"), "CRLF 调用");
    client.close();
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Closed);
}

TEST(MCPStdioIntegrationTest, stdout非法帧在初始化期确定故障且不执行后续请求) {
    // 四种模式分别覆盖空白保活、诊断噪声、完整行语法错误与 EOF 残帧。
    // 它们都发生在 initialize 请求已写出之后，不能被误归类为本地启动参数错误。
    ASSERT_FALSE(fixture_path.empty());
    const std::vector<std::string> modes = {
        "--stdout-empty-line",
        "--stdout-noise",
        "--stdout-malformed-json",
        "--stdout-partial-eof",
    };
    for(const std::string& mode : modes) {
        SCOPED_TRACE(mode);
        TemporaryTree tree;
        const auto executable = copyFixture(fixture_path, tree);
        aiSDK::MCPClient client(makeStdioConfig(executable, tree.workingDirectory(), {mode}));

        // Transport 应把 stdout 违规升级为 MCPException，连接不能短暂报告 Ready。
        EXPECT_THROW(client.connect(), aiSDK::MCPException);
        EXPECT_EQ(client.state(), aiSDK::MCPClientState::Faulted);
        ASSERT_TRUE(client.lastFailureCode().has_value());
        // 即使 stdout Worker 已发布终命事件，close 也必须可重复回收全部子进程资源。
        client.close();
        EXPECT_EQ(client.state(), aiSDK::MCPClientState::Closed);
    }
}

TEST(MCPStdioIntegrationTest, 初始化后关闭stdin会让后续公开请求稳定失败并可强制关闭) {
    // 夹具先发送合法 initialize 响应，再关闭读端并保持根进程存活。
    // 这样失败来源只能是 Client 的后续 stdin 写入，而不是协商响应或进程提前退出。
    ASSERT_FALSE(fixture_path.empty());
    TemporaryTree tree;
    const auto executable = copyFixture(fixture_path, tree);
    aiSDK::MCPClient client(
        makeStdioConfig(executable, tree.workingDirectory(), {"--close-stdin-after-initialize-response"}));

    // initialized 通知可能已首先触发写错误；无论竞态先后，新的公开请求都不得成功返回。
    try {
        client.connect();
    } catch(const aiSDK::MCPException&) {
        // 若异步 Writer 已先检测到关闭，connect 直接失败同样满足该传输终态合同。
    }
    EXPECT_THROW(client.ping(), aiSDK::MCPException);
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Faulted);
    ASSERT_TRUE(client.lastFailureCode().has_value());
    // 夹具故意无限等待，close 必须走终止路径而不能遗留后台子进程。
    client.close();
    EXPECT_EQ(client.state(), aiSDK::MCPClientState::Closed);
}

TEST(MCPStdioIntegrationTest, 重复二十五轮连接关闭不持续增长本进程资源) {
    // 每轮使用同一只读夹具副本，但 Client、Transport 和线程资源均重新创建。
    ASSERT_FALSE(fixture_path.empty());
    TemporaryTree tree;
    const auto executable = copyFixture(fixture_path, tree);

    // 先预热一次 CRT、线程和动态库路径，避免一次性惰性资源污染增长判断。
    {
        aiSDK::MCPClient warmup(makeStdioConfig(executable, tree.workingDirectory(), {}));
        warmup.connect();
        warmup.close();
    }
    const std::size_t baseline = currentProcessResourceCount();
    // 保存每轮终值既能检查最终容差，也能识别持续单调泄漏趋势。
    std::vector<std::size_t> samples;
    samples.reserve(25U);
    for(std::size_t round = 0U; round < 25U; ++round) {
        // 空参数让夹具在 EOF 后合作退出，隔离正常关闭路径的资源所有权。
        aiSDK::MCPClient client(makeStdioConfig(executable, tree.workingDirectory(), {}));
        client.connect();
        client.close();
        EXPECT_EQ(client.state(), aiSDK::MCPClientState::Closed);
        samples.push_back(currentProcessResourceCount());
    }

    bool strictly_increasing = true;
    // 真正的句柄或 fd 泄漏通常表现为每轮至少增加一个资源。
    for(std::size_t index = 1U; index < samples.size(); ++index) {
        strictly_increasing = strictly_increasing && samples[index] > samples[index - 1U];
    }
    EXPECT_FALSE(strictly_increasing);
    ASSERT_FALSE(samples.empty());
    // 两个资源容差覆盖测试框架和平台枚举的瞬时句柄，不掩盖线性增长。
    EXPECT_LE(samples.back(), baseline + 2U);
}

}  // namespace

int main(int argc, char** argv) {
    // 先让 GTest 消费自身参数，再读取 CTest 追加的夹具路径。
    ::testing::InitGoogleTest(&argc, argv);
    if(argc < 2) {
        // 缺少夹具路径时明确失败，不能把集成用例悄悄降级成跳过。
        return 2;
    }
    // u8path 在 Windows 构造宽字符路径，在 POSIX 保留 UTF-8 字节序列。
    fixture_path = std::filesystem::u8path(argv[1]);
    // 所有用例共享只读路径，但各自复制和管理独立进程实例。
    return RUN_ALL_TESTS();
}
