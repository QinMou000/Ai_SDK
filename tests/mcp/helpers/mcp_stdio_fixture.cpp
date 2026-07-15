#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

// 该可执行文件是真实 stdio MCP Server 夹具，不是链接到 Client 的内存替身。
// 测试通过平台进程 API 启动它，从而覆盖 argv、环境、工作目录和管道继承。
// stdin 与 stdout 独占换行分隔 JSON-RPC，任何诊断都只能进入 stderr。
// 每次 stdout 写入都会刷新，避免标准库缓冲把协议时序误判成 Client 超时。
// initialize 固定协商 2025-11-25，并声明首版所需的 Tools 能力。
// initialized 通知触发反向 ping，用于证明 Client 能处理 Server 发起的请求。
// tools/list 故意分成两页，验证 cursor 被原样回送且顺序稳定。
// 第一页包含 nextCursor，第二页省略它作为分页终止条件。
// tools/call 回显参数、标签、环境、工作目录和后代 PID 作为测试预言机。
// 工具元数据包含 Unicode 和未知扩展，验证目录解析不会丢弃 raw 字段。
// --exit-immediately 在握手前退出，覆盖 ServerExited 或传输故障收敛路径。
// --hang-on-eof 模拟不合作 Server，要求 close 超时后执行强制终止。
// --spawn-child 创建长期后代，证明关闭动作回收整个进程树而非只杀父进程。
// --child-sleeper 是后代专用模式，不读取协议管道，也不会自行正常退出。
// --stderr-flood 在读取 initialize 前写满多轮典型管道容量，验证独立排空线程。
// --stdout-crlf 让所有 Server 消息使用 CRLF，验证接收端兼容合同。
// --stdout-empty-line 输出单个空行，验证 stdout 不能承载保活或诊断空白。
// --stdout-noise 输出非 JSON 文本，验证日志不得混入协议通道。
// --stdout-malformed-json 输出带换行的半截对象，验证语法错误稳定归类。
// --stdout-partial-eof 输出无换行尾帧后退出，验证残缺消息不会被执行。
// --close-stdin-after-initialize-response 在成功响应后关闭输入并保持进程存活。
// --label 后的值保持单个 argv 项，用于验证空格和中文未被 Shell 拆分。
// MCP_FIXTURE_ENV 由测试显式写入，回显值用于验证子进程环境构造。
// 工作目录从进程实际状态读取，不能仅回显配置输入造成假阳性。
// 夹具对空行直接失败，因为空行不是合法的单条 stdio JSON-RPC 消息。
// 非 method 消息被视为 Client 对反向请求的响应，随后协议仍应继续工作。
// 未识别 method 不生成响应，便于 Client 的超时路径保持真实网络语义。
// 固定错误文本不包含临时路径、环境值或请求正文，避免测试日志泄密。
// 平台分支只处理后代创建和环境读取，其余协议行为在两端完全一致。
// Windows 后代继承 Transport 创建的 Job，父句柄关闭后仍可由 Job 统一终止。
// POSIX 后代继承 Server 进程组，Transport 对进程组发信号完成树回收。
// 所有失败都返回确定退出码或抛标准异常，测试无需解析不稳定系统文本。
// 夹具只实现首版用例需要的方法，未实现能力不能被误判为生产 Server 行为。
// 反向 ping 使用字符串 ID，刻意覆盖 Client 同时接受字符串与整数关联标识。
// 分页工具名称固定且互不重复，目录合并断言无需依赖散列容器迭代顺序。
// structuredContent 中的 childPid 仅是资源预言机，不参与工具业务结果判断。
// 合作关闭与强制关闭共享同一协议循环，差异只发生在 stdin 到达 EOF 之后。
// 子进程创建不继承额外显式句柄，测试关注 Transport 已声明的进程树所有权。
// main 的返回码区分参数模式和协议输入故障，便于本地复现失败分支。
// 夹具不会调用 Client 代码，避免集成测试把同一实现同时放在请求和响应两端。
// JSON-RPC 消息按到达顺序处理，单进程夹具不引入无关的业务并发。
// stdout 只写紧凑 JSON，协议行长度完全由固定测试数据和调用参数决定。
// stderr 启动消息包含标签但不包含环境值，便于确认回调又不泄露配置内容。
// 环境变量缺失时使用空字符串，让不同父进程环境下的预言机保持一致。
// 工具扩展字段固定保留在 raw 元数据中，测试可区分解析和传输层责任。
// 第二页不返回 nextCursor，Client 必须据此结束列表循环而非猜测页数。
// 对 initialized 的反向请求发生在前台 ping 之前，专门覆盖关联 ID 隔离。
// 非法 JSON 由解析异常终止夹具，使 Client 观察真实的 Server 异常退出。
// stdin EOF 是唯一正常协议循环出口，合作 Server 不依赖额外关闭通知。
// hang-on-eof 只延迟进程退出，不再读取或写入已经关闭的协议管道。
// 临时后代没有业务输出，任何存活状态都只反映进程树回收是否完整。
// stderr 洪泛固定为两 MiB，明显超过 Windows 与 Linux 的典型匿名管道容量。
// 洪泛结束标记让测试通过条件变量等待真实完成点，不依赖调度延迟或 sleep。
// stdout 故障都在收到 initialize 后触发，保证父进程已经完成一次真实写入。
// 关闭 stdin 模式先刷新合法初始化响应，再关闭读端，确保下一次写稳定失败。
// 根进程 PID 与后代 PID 都来自操作系统，关闭测试不会依赖进程名枚举。

constexpr std::size_t kStderrFloodBytes = 2U * 1024U * 1024U;
constexpr std::string_view kStderrFloodMarker = "STDERR_FLOOD_COMPLETE";

// InitialOutputMode 把正常换行策略与四种故障帧集中到单一初始化边界。
// 每次 fixture 进程只允许选择一种模式，避免组合参数产生含糊预言机。
enum class InitialOutputMode { JsonLf, JsonCrlf, EmptyLine, Noise, MalformedJson, PartialEof };

// writeMessage 保证一条 JSON-RPC 消息对应 stdout 的一行并立即刷新。
// 测试夹具的诊断只能写 stderr，避免把日志混入 stdio 协议通道。
void writeMessage(const nlohmann::json& message, std::string_view line_ending) {
    // line_ending 只能来自 main 中固定的 LF/CRLF 选择，Server 数据不能控制 framing。
    std::cout << message.dump() << line_ending << std::flush;
}

// writeStderrFlood 使用固定小块持续写入，避免 fixture 自己申请一份两 MiB 临时串。
// 只有 Transport 并行排空 stderr 时本函数才能到达完成标记并继续协议握手。
void writeStderrFlood() {
    constexpr std::size_t chunk_bytes = 16U * 1024U;
    const std::string chunk(chunk_bytes, 'S');
    for(std::size_t written = 0U; written < kStderrFloodBytes; written += chunk.size()) {
        std::cerr.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }
    // 标记单独成行，测试回调允许按任意物理分块拼接后再检测它。
    std::cerr << '\n' << kStderrFloodMarker << '\n' << std::flush;
}

// writeInitialFaultFrame 在 initialize 请求之后生成单一、确定的 stdout 违规。
// 返回零表示正常 JSON 模式；非零值由 main 直接作为 fixture 退出码。
int writeInitialFaultFrame(InitialOutputMode mode) {
    switch(mode) {
        case InitialOutputMode::EmptyLine:
            std::cout << '\n' << std::flush;
            return 10;
        case InitialOutputMode::Noise:
            std::cout << "这是一条错误写入 stdout 的夹具日志\n" << std::flush;
            return 11;
        case InitialOutputMode::MalformedJson:
            std::cout << "{\"jsonrpc\":\"2.0\",\"id\":\n" << std::flush;
            return 12;
        case InitialOutputMode::PartialEof:
            std::cout << "{\"jsonrpc\":\"2.0\",\"id\":" << std::flush;
            return 13;
        case InitialOutputMode::JsonLf:
        case InitialOutputMode::JsonCrlf:
            return 0;
    }
    return 14;
}

// closeStandardInput 关闭 Server 持有的管道读端，使 Client 的下一次真实写确定失败。
// Windows 关闭 CRT 描述符并同步标准句柄表；POSIX 直接关闭文件描述符零。
void closeStandardInput() {
#ifdef _WIN32
    if(::_close(::_fileno(stdin)) != 0) {
        throw std::runtime_error("stdio 测试夹具无法关闭标准输入");
    }
    ::SetStdHandle(STD_INPUT_HANDLE, INVALID_HANDLE_VALUE);
#else
    if(::close(STDIN_FILENO) != 0) {
        throw std::runtime_error("stdio 测试夹具无法关闭标准输入");
    }
#endif
}

// currentFixtureProcessId 为资源测试提供真实根进程预言机，不依赖进程名称扫描。
std::uint64_t currentFixtureProcessId() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::string environmentValue(const char* name) {
    // 环境读取只用于构造回显结果；变量缺失统一映射为空字符串。
    // Windows 的 _dupenv_s 返回堆内存，成功路径必须立即复制并释放。
    // POSIX getenv 返回借用指针，构造 std::string 后不跨调用保存原地址。
#ifdef _WIN32
    char* raw_value = nullptr;
    std::size_t value_size = 0U;
    if(_dupenv_s(&raw_value, &value_size, name) != 0 || raw_value == nullptr) {
        return {};
    }
    std::string value(raw_value);
    std::free(raw_value);
    return value;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
#endif
}

nlohmann::json makeTool(const std::string& name) {
    // 工具对象同时包含必需 name/inputSchema、展示字段和一个未知扩展。
    // inputSchema 接受任意对象属性，使集成测试聚焦传输而非 Schema 校验。
    // readOnlyHint 只是非可信元数据，Client 不应据此执行或降低风险。
    return {
        {"name",        name                                                },
        {"title",       "stdio 集成工具"                                },
        {"description", "回显参数、环境和工作目录"              },
        {"inputSchema", {{"type", "object"}, {"additionalProperties", true}}},
        {"annotations", {{"readOnlyHint", true}}                            },
        {"x-fixture",   "保留扩展"                                      }
    };
}

// runChildSleeper 创建一个永不自行退出的后代，专门验证 Transport 的进程树回收。
// 子进程不参与协议；Windows 继承父进程 Job，POSIX 继承父进程组。
[[noreturn]] void runChildSleeper() {
    // 无限内核等待不消耗 CPU，也不引入依赖调度速度的短 sleep。
    // noreturn 让编译器和调用点明确该模式只能由外部进程控制结束。
#ifdef _WIN32
    while(true) {
        ::Sleep(INFINITE);
    }
#else
    while(true) {
        ::pause();
    }
#endif
}

// spawnChildSleeper 返回可通过测试控制结果观察的 PID，不经过 shell。
// Windows 明确指定当前可执行文件；POSIX fork 后不触碰复杂库状态并直接进入休眠。
std::uint64_t spawnChildSleeper() {
#ifdef _WIN32
    // 读取当前模块绝对路径，确保后代与正在测试的夹具二进制完全一致。
    std::vector<wchar_t> path_buffer(32768U, L'\0');
    const DWORD path_size = GetModuleFileNameW(nullptr, path_buffer.data(), static_cast<DWORD>(path_buffer.size()));
    if(path_size == 0U || path_size >= path_buffer.size()) {
        throw std::runtime_error("stdio 测试夹具无法取得自身可执行路径");
    }
    const std::wstring executable(path_buffer.data(), path_size);
    // 命令行只包含受控开关；路径用引号包围以覆盖中文和空格目录。
    std::wstring command_line = L"\"" + executable + L"\" --child-sleeper";
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    // CreateProcessW 要求可变命令行缓冲；executable 参数单独固定真实程序。
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &startup, &process)) {
        throw std::runtime_error("stdio 测试夹具无法创建后代进程");
    }
    const std::uint64_t pid = process.dwProcessId;
    // 测试仅需 PID 作为外部存活预言机，不持有后代线程或进程句柄。
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return pid;
#else
    // fork 继承父进程组；子分支不再分配复杂协议状态，直接进入休眠。
    const pid_t pid = ::fork();
    if(pid < 0) {
        throw std::runtime_error("stdio 测试夹具无法创建后代进程");
    }
    if(pid == 0) {
        // 子进程永不返回 main，避免意外继续读取与父进程共享的 stdin。
        runChildSleeper();
    }
    return static_cast<std::uint64_t>(pid);
#endif
}

}  // namespace

int main(int argc, char** argv) {
    // 选项先完整解析再采取动作，使参数顺序不改变各测试模式的语义。
    bool exit_immediately = false;
    bool hang_on_eof = false;
    bool spawn_child = false;
    bool child_sleeper = false;
    bool stderr_flood = false;
    bool close_stdin_after_initialize_response = false;
    InitialOutputMode initial_output_mode = InitialOutputMode::JsonLf;
    std::size_t output_mode_count = 0U;
    std::string label;
    for(int index = 1; index < argc; ++index) {
        // 每个已知布尔开关只翻转对应行为，不吞掉相邻 argv 项。
        const std::string argument = argv[index];
        if(argument == "--exit-immediately") {
            exit_immediately = true;
        } else if(argument == "--hang-on-eof") {
            hang_on_eof = true;
        } else if(argument == "--spawn-child") {
            spawn_child = true;
        } else if(argument == "--child-sleeper") {
            child_sleeper = true;
        } else if(argument == "--stderr-flood") {
            stderr_flood = true;
        } else if(argument == "--close-stdin-after-initialize-response") {
            close_stdin_after_initialize_response = true;
        } else if(argument == "--stdout-crlf") {
            initial_output_mode = InitialOutputMode::JsonCrlf;
            ++output_mode_count;
        } else if(argument == "--stdout-empty-line") {
            initial_output_mode = InitialOutputMode::EmptyLine;
            ++output_mode_count;
        } else if(argument == "--stdout-noise") {
            initial_output_mode = InitialOutputMode::Noise;
            ++output_mode_count;
        } else if(argument == "--stdout-malformed-json") {
            initial_output_mode = InitialOutputMode::MalformedJson;
            ++output_mode_count;
        } else if(argument == "--stdout-partial-eof") {
            initial_output_mode = InitialOutputMode::PartialEof;
            ++output_mode_count;
        } else if(argument == "--label" && index + 1 < argc) {
            // 标签值显式推进索引，后续参数仍按独立项继续解析。
            label = argv[++index];
        }
    }
    if(child_sleeper) {
        // 后代模式优先级最高，确保不会执行提前退出或协议 Server 分支。
        runChildSleeper();
    }
    if(output_mode_count > 1U) {
        // stdout 预言机必须唯一，否则测试无法判断哪个违规获得首因所有权。
        std::cerr << "stdio 测试夹具只能选择一种 stdout 模式\n";
        return 3;
    }
    if(exit_immediately) {
        // 在任何 stdout 协议字节前退出，Client 不可能误收一条残留成功响应。
        std::cerr << "夹具按测试要求提前退出，标签=" << label << '\n';
        return 7;
    }

    // stderr 输出量足以证明 Transport 持续消费独立管道，但不会进入公开结果。
    std::cerr << "stdio MCP 测试夹具已启动，标签=" << label << '\n' << std::flush;
    if(stderr_flood) {
        // 洪泛发生在协议读取前，若 stderr 没有并行排空，initialize 永远无法完成。
        writeStderrFlood();
    }
    const std::uint64_t child_pid = spawn_child ? spawnChildSleeper() : 0U;
    const std::uint64_t server_pid = currentFixtureProcessId();
    const std::string_view response_line_ending =
        initial_output_mode == InitialOutputMode::JsonCrlf ? std::string_view("\r\n") : std::string_view("\n");
    // child_pid 为零表示未创建后代；非零值只随工具结果回传给测试进程。
    std::string line;
    while(std::getline(std::cin, line)) {
        if(line.empty()) {
            // 空行不是合法 MCP 消息；夹具直接退出，让 Transport 把 EOF 归类为失败。
            return 8;
        }

        const nlohmann::json message = nlohmann::json::parse(line);
        // Client 发出的请求和通知都有 method；响应只有 id 与 result/error。
        if(!message.contains("method")) {
            // 这是 Client 对夹具 Server 请求的响应，测试只需证明它不会破坏后续关联。
            continue;
        }
        const std::string method = message.at("method").get<std::string>();
        if(method == "initialize") {
            // 故障模式在 Client 已成功写入请求后触发，排除纯启动失败的混淆。
            const int fault_exit_code = writeInitialFaultFrame(initial_output_mode);
            if(fault_exit_code != 0) {
                return fault_exit_code;
            }
            // 响应复用请求 id，并声明唯一受支持协议版本及 Tools 能力对象。
            writeMessage(
                {
                    {"jsonrpc", "2.0"                                                    },
                    {"id",      message.at("id")                                         },
                    {"result",
                     {{"protocolVersion", "2025-11-25"},
                      {"capabilities", {{"tools", {{"listChanged", false}}}}},
                      {"serverInfo", {{"name", "stdio 测试夹具"}, {"version", "1"}}}}}
            },
                response_line_ending);
            if(close_stdin_after_initialize_response) {
                // 合法响应已经刷新；关闭读端后保持 stdout 与进程存活，让失败只来自下一次 stdin 写。
                closeStandardInput();
                runChildSleeper();
            }
        } else if(method == "notifications/initialized") {
            // 初始化通知本身没有响应；主动 ping 用于覆盖后台 Server 请求响应路径。
            writeMessage(
                {
                    {"jsonrpc", "2.0"                   },
                    {"id",      "stdio-server-ping"     },
                    {"method",  "ping"                  },
                    {"params",  nlohmann::json::object()}
            },
                response_line_ending);
        } else if(method == "ping") {
            // ping 返回空对象，覆盖最小 JSON-RPC 成功响应而不引入业务字段。
            writeMessage(
                {
                    {"jsonrpc", "2.0"                   },
                    {"id",      message.at("id")        },
                    {"result",  nlohmann::json::object()}
            },
                response_line_ending);
        } else if(method == "tools/list") {
            const auto& params = message.at("params");
            if(params.contains("cursor")) {
                // 任意 cursor 都进入第二页；Client 单元测试负责更细的 cursor 类型校验。
                writeMessage(
                    {
                        {"jsonrpc", "2.0"                                                   },
                        {"id",      message.at("id")                                        },
                        {"result",  {{"tools", nlohmann::json::array({makeTool("second")})}}}
                },
                    response_line_ending);
            } else {
                // 首次请求返回固定 cursor，Unicode 值验证传输没有窄字符破坏。
                writeMessage(
                    {
                        {"jsonrpc", "2.0"                                                                      },
                        {"id",      message.at("id")                                                           },
                        {"result",
                         {{"tools", nlohmann::json::array({makeTool("inspect")})}, {"nextCursor", "下一页"}}}
                },
                    response_line_ending);
            }
        } else if(method == "tools/call") {
            // structuredContent 同时回传参数、Unicode argv、自定义环境和实际工作目录。
            // message.at 链式访问会在请求结构错误时让夹具退出，避免伪造成功结果。
            const nlohmann::json structured = {
                {"arguments",        message.at("params").at("arguments")      },
                {"label",            label                                     },
                {"environment",      environmentValue("MCP_FIXTURE_ENV")       },
                {"workingDirectory", std::filesystem::current_path().u8string()},
                {"childPid",         child_pid                                 },
                {"serverPid",        server_pid                                }
            };
            writeMessage(
                {
                    {"jsonrpc", "2.0"           },
 // content 提供 Adapter 可消费的文本块，structuredContent 保留审计字段。
                    {"id",      message.at("id")},
                    {"result",
                     {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "stdio 调用成功"}}})},
                      {"structuredContent", structured},
                      {"isError", false}}       }
            },
                response_line_ending);
        }
    }

    if(hang_on_eof) {
        // Transport 应在正常等待上限后终止进程树，不能永久等待不合作的 Server。
        // EOF 已证明 Client 关闭了 stdin；继续休眠专门验证 shutdown_timeout 分支。
        runChildSleeper();
    }
    // 合作模式在 stdin EOF 后正常退出，Transport 应能无强杀完成回收。
    return 0;
}
