#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aiSDK {
namespace detail {

// ProcessOptions 是 stdio MCP Server 的内部启动参数。
// executable 和 working_directory 使用路径类型，避免调用层提前丢失平台字符宽度。
// arguments 始终按独立 argv 项传入，任何平台都不得把它们交给 shell 二次解释。
// environment 的值可能包含秘密，平台错误不得复制这些内容。
struct ProcessOptions {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    std::unordered_map<std::string, std::string> environment;
    bool inherit_parent_environment = false;
};

// Process 封装一个直接启动的子进程以及分离的 stdin、stdout、stderr 管道。
// 原生句柄、文件描述符和进程树控制对象只存在于平台条件源的 Impl 中。
// 单个标准流在同一时刻只允许一个读者或写者；Transport 负责固定 Worker 的所有权。
// readStdout/readStderr 返回零表示 EOF 或 cancelIo 已请求停止，不返回残缺协议含义。
// writeStdin 要么写完全部字节，要么抛出带稳定中文上下文的运行期异常。
// waitForExit 返回 false 只表示指定时间内尚未退出，返回 true 时写入稳定退出码。
// close 与析构均为幂等 noexcept 清理边界，不允许遗留根进程或受控子孙进程。
class Process final {
   public:
    // start 在 Client 生成的初始化绝对截止时间内完成参数校验、进程启动和进程树约束。
    // 任一步骤失败或超时都会先回收已经创建的资源，再抛出异常。
    static Process start(ProcessOptions options, std::chrono::steady_clock::time_point absolute_deadline);

    ~Process() noexcept;

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;

    // 每次读取最多写入 capacity 个字节；buffer 为空或容量为零属于调用方错误。
    std::size_t readStdout(char* buffer, std::size_t capacity);
    std::size_t readStderr(char* buffer, std::size_t capacity);

    // writeStdin 保持输入字节原样，不追加换行，也不解释 JSON 或命令字符。
    void writeStdin(std::string_view data);
    // closeStdin 用于正常关闭握手；重复调用保持无操作。
    void closeStdin() noexcept;

    // waitForExit 使用单调超时语义；负超时按零处理。
    bool waitForExit(std::chrono::milliseconds timeout, int& exit_code);
    // terminateTree 强制终止当前 Process 管理的完整进程树。
    void terminateTree();
    // cancelIo 唤醒正在等待的读写 Worker，供 Transport 在关闭阶段先行收敛线程。
    void cancelIo() noexcept;
    // close 取消 I/O、关闭 stdin、终止仍存活的进程树并释放全部本地资源。
    void close() noexcept;

   private:
    struct Impl;

    explicit Process(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace detail
}  // namespace aiSDK
