#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "mcp/detail/MCPDeadline.h"
#include "mcp/detail/Process.h"

extern char** environ;

namespace aiSDK {
namespace detail {
namespace {

// POSIX 进程边界总则
// - 本文件只承担 Linux stdio 子进程与原始字节通道，不解释 MCP 方法。
// - 平台类型、PID 和文件描述符全部封闭在 Process::Impl。
// - ProcessOptions 保持与 Windows 条件源完全相同的内部接口。
// - 实现直接调用 fork 和 execve，不经过 sh、bash、popen 或 system。
// - 根进程使用独立进程组，关闭必须覆盖受控子孙。
// - stdout 与 stderr 物理分离，协议和诊断不能混用。
// - 所有阻塞 I/O 都通过 poll 同时观察取消 FD。
// - 启动失败保持事务性，不能发布半初始化 Process。
// - close 与析构不得传播清理异常。
// 防御性配置校验
// - executable 必须是存在且有执行权限的绝对普通文件。
// - 绝对路径直接传给 execve，不使用 PATH 搜索。
// - working_directory 非空时必须是存在的绝对目录。
// - 每个参数保持独立 argv 边界，不进行空白拆分。
// - 参数中的 NUL、回车和换行在创建管道前拒绝。
// - 环境名称不能为空、不能含等号或控制边界字符。
// - 环境值拒绝 NUL、回车和换行，避免静默截断。
// - 错误消息只描述字段类型，不复制路径、参数或秘密环境值。
// - 本层校验是 OS 防线，不能替代公开配置的静态校验。
// argv 构造合同
// - argv[0] 固定为 executable 的原生字节路径。
// - ProcessOptions.arguments 从 argv[1] 开始逐项追加。
// - 空字符串参数必须保留为空 argv 项。
// - 空格、引号、反斜杠和 shell 元字符都保持普通字节。
// - argv 指针数组在 fork 前完成分配并以 nullptr 终止。
// - fork 后子分支不再修改 vector 或 string。
// - execve 成功前所有 argv 存储都由父栈的写时复制内存提供。
// - Process 不替调用方推断 locale 或转码文件系统路径。
// - 真实测试应回显 argv 验证边界没有合并或丢失。
// 环境构造合同
// - inherit_parent_environment 为 false 时 envp 只含显式映射。
// - 继承开启时先复制 environ，再用显式值覆盖同名项。
// - 环境项按名称排序，生成结果稳定且便于审计。
// - 每个 envp 项使用单个 name=value 字符串。
// - envp 指针数组在 fork 前建立并以 nullptr 终止。
// - 空环境通过只含 nullptr 的 envp 明确传递。
// - 显式环境只进入子进程，不调用 setenv 修改宿主。
// - 环境值可能含秘密，任何错误和默认诊断都不能回显。
// - 父进程并发修改全局环境不属于 Process 支持的同步范围。
// fork 后安全边界
// - fork 之前完成所有 C++ 分配、路径检查和字符串构造。
// - 子分支不调用 iostream、filesystem、malloc 或异常机制。
// - 子分支只使用 setpgid、dup3、dup2、close、chdir、execve、write 和 _exit。
// - 失败通过固定尺寸 ChildFailure 写入错误管道。
// - 错误阶段使用整数枚举，子分支不构造中文字符串。
// - errno 在失败系统调用后立即复制，避免后续调用覆盖。
// - 错误写入处理 EINTR，不依赖锁或条件变量。
// - 子分支最终只能 execve 成功或 _exit(127)。
// - 新增 fork 后逻辑必须先确认 async-signal-safe 性。
// CLOEXEC 管道合同
// - 所有管道使用 pipe2 原子设置 O_CLOEXEC。
// - 原子设置避免多线程宿主在 pipe 与 fcntl 之间 fork 泄漏。
// - stdin、stdout、stderr 和启动错误分别使用独立管道。
// - 父端和子端所有权在 fork 后立即按方向收敛。
// - exec 成功会自动关闭错误管道写端，父端据此识别成功。
// - 非标准流父端始终保持 CLOEXEC，不能传给 Server。
// - 取消管道只在父进程创建，不进入子进程地址空间。
// - UniqueFd 负责所有提前异常路径的描述符关闭。
// - 任何新增控制通道都必须默认 CLOEXEC。
// 高位描述符合同
// - 新建管道端点复制到 10 以上，避免宿主关闭标准流时发生别名。
// - F_DUPFD_CLOEXEC 同时保证最小编号和继承安全。
// - 原始 pipe2 端点在复制完成后由 RAII 自动关闭。
// - 子分支可以安全把高位端点 dup2 到 0、1、2。
// - 启动错误写端固定复制到 FD 3 并保持 CLOEXEC。
// - 高位布局使 close_range 可以从 4 开始一次关闭其余描述符。
// - 端点编号不作为公开状态或错误信息。
// - 描述符数量固定，与 MCP 请求数量无关。
// - 测试应覆盖宿主标准流被重定向的运行环境。
// 受控 FD 继承
// - exec 前只允许 0、1、2 和临时错误 FD 3 存活。
// - 新内核优先使用 close_range 清理未知宿主描述符。
// - close_range 从 4 开始，不影响标准流与启动错误通道。
// - 旧内核退化为 fork 前取得上限的 close 循环。
// - 循环只调用 close，不进行目录扫描或内存分配。
// - 宿主网络、文件、Token 和其他子进程句柄不能泄漏给 Server。
// - FD 清理发生在 chdir 和 execve 之前。
// - 清理失败不需要逐项报告，exec 边界以关闭最大集合为目标。
// - 未来改变最低内核要求时仍须保留无泄漏验收。
// 启动错误管道
// - 错误管道用于区分 exec 成功与 fork 后准备失败。
// - 写端设置 CLOEXEC，exec 成功后父端读取 EOF。
// - ChildFailure 大小远小于 PIPE_BUF，单次记录不会与其他写者交错。
// - 父端仍循环读取，防御 EINTR 和部分读取。
// - 零字节表示 exec 已经关闭写端，不代表进程长期存活。
// - 非零但不足完整结构视为启动状态损坏。
// - stage 映射为稳定中文动作，error 保留数字 errno。
// - 错误内容不包含路径、argv、环境或 stderr。
// - 父端发现任何启动错误都由 SpawnGuard 强制回收子进程。
// 独立进程组合同
// - 子分支在 execve 前调用 setpgid(0,0) 建立独立组。
// - 父分支再调用 setpgid(pid,pid) 缩小快速 exec 的竞态窗口。
// - EACCES 表示子进程已越过可由父端设置的阶段，可以继续检查错误管道。
// - ESRCH 可能表示根进程快速退出，后续 wait 路径负责分类。
// - process_group 固定为根 PID，不使用宿主当前进程组。
// - 强制关闭向负 PGID 发送信号，覆盖受控子孙。
// - 根 PID 仍单独发送信号，覆盖组创建失败的窄窗口。
// - Server 不能通过普通 fork 留下脱离清理的同组后代。
// - 主动 setsid 或重新分组属于不受支持的恶意逃逸边界。
// 启动事务回滚
// - fork 成功后立即建立 SpawnGuard，直到 Impl 完整发布。
// - 父进程关闭所有 child 端，确保 EOF 语义正确。
// - setpgid、错误管道读取、非阻塞配置和 Impl 分配任一失败都会回滚。
// - 回滚先向进程组和根进程发送 SIGKILL。
// - 回滚随后调用 waitpid，避免留下僵尸根进程。
// - 所有父端描述符继续由局部 UniqueFd 自动释放。
// - 取消管道创建失败同样不能遗留已 exec 的 Server。
// - 只有 Process 接管 PID 和所有父端后才释放 SpawnGuard。
// - start 返回表示 OS 通道可用，不表示 MCP initialize 已完成。
// 非阻塞父端合同
// - 父 stdin 写端设置 O_NONBLOCK，避免 Server 不读时永久阻塞。
// - 父 stdout 与 stderr 读端设置 O_NONBLOCK，配合 poll 驱动。
// - cancel 写端设置 O_NONBLOCK，重复取消不会因管道已满阻塞。
// - 非阻塞不等于忙轮询，Worker 始终先等待 poll。
// - EAGAIN 和 EWOULDBLOCK 表示重新等待就绪，不是终命错误。
// - 部分读写按实际字节推进，不假设 PIPE_BUF 覆盖完整消息。
// - 文件状态标志在 fork 成功后只修改父端。
// - 子进程标准流保持普通阻塞语义，兼容常见 MCP Server。
// - 任何新父端阻塞操作都必须纳入取消机制。
// 取消管道合同
// - Process 为三个标准流 Worker 共享一条父进程内部取消管道。
// - cancelIo 写入单字节后不读取，使 read 端持续保持可读。
// - 当前 poll 和取消后的新 poll 都会立即观察停止信号。
// - 取消状态受 io_mutex 保护并且不可逆。
// - 重复 cancelIo 不重复写入或改变资源所有权。
// - 取消只唤醒本地 Worker，不发送 POSIX 信号给 Server。
// - 取消不会解释工具是否执行，也不触发请求重放。
// - close 在活动 Worker 归零前保持取消 FD 有效。
// - 取消管道字节不进入 stdout、stderr 或公开错误。
// I/O Worker 唯一性
// - stdin 同一时刻只允许一个 Writer。
// - stdout 同一时刻只允许一个 Reader。
// - stderr 同一时刻只允许一个 Reader。
// - 重复并发访问同一流使用 logic_error 暴露 Transport 缺陷。
// - beginIo 在 io_mutex 内登记活动标志和 FD 快照。
// - poll、read 和 write 执行期间不持有 io_mutex。
// - finishIo 清除活动标志并通知 close 条件变量。
// - cancelIo 可以与三个系统调用并发且不会获取 Client 锁。
// - Transport 在线程 join 前必须保持 Process::Impl 存活。
// stdout 原始读取合同
// - readStdout 只返回原始字节，不执行 UTF-8、换行或 JSON 校验。
// - 调用方缓冲区为空或容量为零属于 invalid_argument。
// - poll 同时等待 stdout 数据、HUP、错误和取消。
// - 读取正数字节时原样返回，不追加 NUL。
// - read 返回零表示真实 EOF。
// - 取消管道就绪同样返回零，由 Transport 状态区分关闭。
// - EINTR、EAGAIN 和 EWOULDBLOCK 重新进入等待。
// - 其他 errno 转换为稳定中文动作与数字码。
// - 逐行 framing 和残缺尾部检查留给 StdioMCPTransport。
// stderr 原始读取合同
// - readStderr 与 stdout 共享等待算法但使用独立 FD。
// - stderr 永远不进入 MCP JSON-RPC 解析。
// - 专用 Reader 必须持续排空，避免子进程因诊断洪泛阻塞。
// - Process 不缓存诊断内容，Transport 负责有界尾部策略。
// - stderr EOF 不应单独判定协议故障。
// - 取消时返回零，Worker 可以确定退出。
// - 读取异常不得附加已读 stderr 原文。
// - stdout 和 stderr 可并行读取，互不持有对方状态。
// - 集成测试应写入超过典型管道容量的 stderr。
// stdin 写入合同
// - writeStdin 不追加 LF，调用方传入的完整字节序列即唯一负载。
// - 空 string_view 不创建 poll 或 write 调用。
// - Writer 先等待 POLLOUT，同时观察取消 FD。
// - 每次 write 处理可能的部分写入并推进 offset。
// - 零进度视为运行期故障，防止无限循环。
// - POLLHUP、POLLERR 和 EPIPE 表示 Server 已关闭 stdin。
// - 取消和远端关闭使用不同中文错误分类。
// - 写入期间不持有 Transport 队列锁或 Client 状态锁。
// - 一行原子性依赖唯一 Writer，而不是单次 write 的长度。
// SIGPIPE 隔离合同
// - 向关闭管道 write 可能先产生 SIGPIPE 再返回 EPIPE。
// - SDK 不能把 SIGPIPE 全局设为忽略，否则会改变宿主应用语义。
// - 每次 writeStdin 只在线程局部临时屏蔽 SIGPIPE。
// - pthread_sigmask 返回的错误码直接作为数字诊断。
// - 若线程原先已屏蔽 SIGPIPE，退出时保留原状态。
// - 若线程原先未屏蔽，恢复前消费本次产生的待处理信号。
// - sigtimedwait 使用零超时，只处理已经挂起的 SIGPIPE。
// - RAII 析构恢复原信号掩码且不抛异常。
// - Linux close-stdin 测试必须证明宿主进程仍然存活。
// stdin 正常关闭合同
// - closeStdin 关闭父写端，让 Server 观察标准输入 EOF。
// - 重复调用保持无操作。
// - 无活动 Writer 时不取消 stdout 或 stderr Reader。
// - 活动 Writer 存在时通过全局取消管道解除其 poll 或 write。
// - 取消后等待 stdin_active 归零再关闭 FD。
// - 正常协议收尾应由 Transport 先停止出站队列。
// - Server 仍可通过 stdout 或 stderr 输出最后数据。
// - close 会先执行全局 cancelIo，再走统一 FD 释放。
// - 本方法 noexcept，失败不能穿透析构路径。
// pidfd 优先等待
// - 支持 SYS_pidfd_open 的内核为根进程创建可轮询 pidfd。
// - pidfd 只用于退出就绪通知，最终回收仍由 waitpid 完成。
// - pidfd 生命周期与 Process 一致并默认 CLOEXEC。
// - pidfd 创建失败不阻止启动，保留旧内核回退路径。
// - poll pidfd 使用调用方剩余单调超时。
// - EINTR 只重新计算剩余时间，不重置整体截止。
// - pidfd 不暴露到公共 API 或 Trace。
// - 关闭前保留 pidfd，便于 terminate 后及时观察退出。
// - 构建不新增 liburing 或其他等待依赖。
// 旧内核等待回退
// - 没有 pidfd 时使用 waitid/waitpid 的短间隔有界探测。
// - 回退间隔最多十毫秒，不改变公开超时上限。
// - poll 空集合承担内核等待，避免主动 std::this_thread::sleep_for。
// - 每轮都根据 steady_clock 截止重新计算剩余时间。
// - 负超时按零处理，立即执行一次状态探测。
// - 超时返回 false 且不修改调用方退出码。
// - 回退只影响等待延迟，不改变进程组回收语义。
// - 真实超时测试使用秒级截止，避免调度抖动假阴性。
// - 未来提高最低 Linux 内核版本后可移除回退但需更新规格。
// WNOWAIT 观察合同
// - waitid 使用 WNOWAIT 在回收根进程前先观察终态。
// - 根进程保持僵尸 PID，可暂时阻止 PID 和 PGID 被系统复用。
// - 观察到终态后先向原进程组发送 SIGKILL 清理残留后代。
// - 只有进程组处理完成后才调用 waitpid 真正回收根进程。
// - 该顺序避免根 PID 被复用后误伤无关新进程组。
// - waitid 的空 siginfo 表示尚未退出。
// - EINTR 重新等待，其他错误按运行期失败报告。
// - 终态观察和回收由 wait_mutex 限制为单个调用者。
// - 退出码缓存在线性化完成后对后续等待稳定可见。
// waitpid 回收合同
// - waitpid 是根进程僵尸回收的唯一最终入口。
// - 正常退出映射为 WEXITSTATUS。
// - 信号退出映射为 128 加信号值，保持常见诊断形状。
// - waitpid 的 EINTR 必须重试。
// - 无法回收已观察到的根进程属于明确传输故障。
// - 成功回收后 exit_code_known 在 state_mutex 内发布。
// - 后续 waitForExit 直接返回缓存，不再次调用 waitpid。
// - 关闭路径必须在 SIGKILL 后尽力进入 waitpid。
// - 退出码只作为本地诊断，不直接成为 JSON-RPC 错误。
// PGID 复用安全
// - 根进程退出后立即回收可能让同一数字 PID 被内核复用。
// - 进程组信号若晚于复用可能影响无关进程，必须避免。
// - WNOWAIT 窗口保留根僵尸身份直到子孙清理完成。
// - exit_code_known 后 terminateTree 不再向旧 PGID 发送信号。
// - close 在未知退出码时先终止组再等待根进程。
// - 启动回滚阶段根进程尚未回收，kill(-pid) 仍指向受控组。
// - process_group 永远来源于已创建根 PID，不接受外部输入。
// - 任何新增异步 Reaper 都必须维持同样的先组后根顺序。
// - 资源测试应包含根进程退出但孙进程继续运行的模式。
// 强制终止合同
// - terminateTree 使用 SIGKILL，明确表示最终强制阶段。
// - 优雅阶段由 Transport 关闭 stdin 并等待 shutdown_timeout。
// - 负 process_group 不会被调用，避免向宿主进程组发信号。
// - kill(-pgid) 终止同组根进程和受控后代。
// - 随后 kill(pid) 覆盖 setpgid 尚未建立的竞态。
// - ESRCH 表示目标已不存在，可按成功清理处理。
// - 其他 errno 以中文动作和数字码抛出。
// - terminateTree 不调用 waitpid，允许 Reaper 统一完成回收。
// - close 捕获终止异常但继续执行最终资源释放。
// close 顺序合同
// - close 首先调用 cancelIo 唤醒全部标准流 Worker。
// - 随后等待三个活动标志归零，避免关闭被使用的 FD。
// - 数据和取消 FD 只在 Worker 收敛后释放。
// - 最终关闭总是尝试 terminateTree，覆盖残留进程组。
// - SIGKILL 后等待根进程最多五秒。
// - 正常情况下 SIGKILL 不可被忽略，等待应快速完成。
// - 任何清理异常都不能穿透 noexcept 边界。
// - pidfd 在进程终止流程完成后最后释放。
// - impl_ reset 让重复 close 和移出对象析构成为无操作。
// 移动与所有权合同
// - Process 禁止复制，保证 PID 和 FD 只有一个关闭责任者。
// - 移动构造只转移 unique_ptr，不移动 mutex 或 condition_variable。
// - 移动赋值先关闭目标原有进程，再接管来源 Impl。
// - 移出对象为空，析构不会重复 kill 或 waitpid。
// - Worker 启动后 Transport 不应再次移动 Process。
// - 父端 FD 只存在于稳定 Impl 地址中。
// - start 可以依赖返回值优化或移动按值返回。
// - 观察状态不通过复制 Process 实现。
// - 未来共享 Reaper 状态也必须保持单一 OS 资源所有者。
// 锁分工合同
// - state_mutex 保护 PID 生命周期、closed 和退出码缓存。
// - wait_mutex 序列化 waitid 与 waitpid，避免双重回收。
// - io_mutex 保护 FD 所有权、活动标志和取消状态。
// - 系统 poll、read、write、wait 和 kill 不在持有 io_mutex 时执行。
// - 本层不获取 MCPClient 状态锁。
// - 本层不调用传输回调、用户函数或凭据 Provider。
// - 条件变量只等待 Worker 收敛，不参与协议消息关联。
// - 取消状态不可恢复，Process 不支持重新 start。
// - 新增锁必须给出与现有三把锁的固定顺序。
// 错误与隐私合同
// - 所有自行生成的可读错误使用简体中文。
// - POSIX 诊断只附加 errno 数值，不调用可能回显路径的 strerror。
// - 异常不包含 executable、working_directory 或完整 argv。
// - 异常不包含环境值、Token 或 API Key。
// - stderr 原文绝不拼接进 Process 错误。
// - 参数错误使用 invalid_argument，生命周期误用使用 logic_error。
// - OS 运行期失败使用 runtime_error。
// - close 和析构吞掉清理失败，显式方法保留错误感知。
// - 上层负责映射 MCPErrorCode 和 OutcomeUnknown。
// 性能与资源合同
// - 每个 Process 固定使用四条子进程相关管道和一条取消管道。
// - 根进程可选持有一个 pidfd。
// - Process 不按消息创建线程或分配协议缓存。
// - read 直接写入调用方固定缓冲区。
// - write 直接消费 string_view，不复制完整消息。
// - argv 和环境只在 start 阶段构造一次。
// - poll 阻塞等待事件，不进行高频 CPU 自旋。
// - 资源数量与 tools/list 页数和工具调用数无关。
// - 重复启动关闭测试应观察 FD 数不会持续增长。
// 安全边界合同
// - 配置来自受信应用，但模型输出不能直接构造 ProcessOptions。
// - execve 使用绝对路径，禁止 PATH 劫持。
// - 无 shell 路径意味着重定向、管道和命令替换字符没有特殊语义。
// - 默认空环境避免父进程秘密隐式泄漏。
// - close_range 防止其他宿主 FD 被 Server 继承。
// - 独立进程组提供受控子孙回收能力。
// - 错误文本不回显秘密配置。
// - Process 不访问 Server 返回的 URI 或富媒体内容。
// - 消息大小和队列限制由写入前的 Transport 执行。
// 与 stdio Transport 的合同
// - Transport 在显式 open 中调用 Process::start。
// - Writer Worker 是 writeStdin 的唯一使用者。
// - stdout Reader 把分块交给增量行解码器。
// - stderr Reader 维护有界尾部，不能停止排空。
// - Reaper 调用 waitForExit 并归一化 ServerExited 事件。
// - commitPrepared 只入队，不在 Client 状态锁内执行 write。
// - close 先停止出站准入，再关闭 stdin 或取消全部 I/O。
// - Worker join 后才能销毁 Process。
// - EOF、退出和取消可能乱序，Transport 必须只完成一次终命事件。
// 预期 Linux 集成测试
// - 测试 Server 必须是 CMake 编译的本地 C++ 可执行文件。
// - TARGET_FILE 提供绝对路径，不依赖 PATH 或当前目录。
// - 参数测试覆盖空格、中文和 shell 元字符。
// - 显式环境与禁继承分别验证子进程可见集合。
// - stderr 洪泛验证独立排空不会阻塞 stdout。
// - close-stdin 模式验证 EPIPE 不会触发宿主 SIGPIPE 终止。
// - hang 模式验证 cancelIo 和 SIGKILL 有界收敛。
// - spawn-child 模式验证根进程、进程组和孙进程全部消失。
// - 重复周期验证 /proc/self/fd 与僵尸进程无持续增长。
// 维护与平台范围
// - 本条件源只在 Linux 目标中参与编译。
// - Windows 使用独立 WindowsProcess.cpp，不在本文件混入平台宏。
// - macOS 不属于首版支持范围。
// - 不得为了简化改用 std::system、popen 或 shell。
// - 不得把 pid_t 或 FD 移到 Process.h 或公开 MCP 头。
// - 新增 fork 子分支调用必须通过 async-signal-safe 审核。
// - 新增 FD 必须明确 CLOEXEC、父子方向和取消方式。
// - 新增等待点必须受单调截止或 cancelIo 控制。
// - Linux 验证必须使用 GCC 的 Wall、Wextra 和 Wpedantic。
// 请求确定性边界
// - Process 不决定请求何时进入 Submitted。
// - Transport 队列成功入队才是 Submitted 线性化点。
// - write 部分成功后失败不能自动重放整条 JSON 行。
// - Process 不保存最后一条消息，也没有重试方法。
// - EPIPE 只能说明通道关闭，不能证明远端工具未执行。
// - stdout EOF 只能说明字节流终止，不能推断 JSON-RPC 结果。
// - cancelIo 解除本地等待，不撤销可能发生的远端副作用。
// - 根进程退出与匹配响应到达由 Client 状态机共同裁决。
// - OutcomeUnknown 的提升只由上层在 Submitted 请求上执行。

// POSIX 错误只暴露稳定动作和 errno，避免把命令、环境或本机敏感路径带到上层。
std::runtime_error posixError(const std::string& action, int error = errno) {
    return std::runtime_error(action + "，POSIX 错误码: " + std::to_string(error));
}

// UniqueFd 统一管理管道、pidfd 与取消 FD，异常路径不会遗留描述符。
class UniqueFd final {
   public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() noexcept {
        reset();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if(this != &other) {
            reset(other.release());
        }
        return *this;
    }

    int get() const noexcept {
        return fd_;
    }
    explicit operator bool() const noexcept {
        return fd_ >= 0;
    }
    int release() noexcept {
        const int result = fd_;
        fd_ = -1;
        return result;
    }
    void reset(int fd = -1) noexcept {
        if(fd_ >= 0) {
            // Linux 在 close 返回 EINTR 时也已经释放描述符；重试可能误关其他线程复用的新 FD。
            ::close(fd_);
        }
        fd_ = fd;
    }

   private:
    int fd_ = -1;
};

// ManagedPipe 把两个端点都提升到标准流范围之外，简化 fork 后的安全重定向。
struct ManagedPipe {
    UniqueFd read_end;
    UniqueFd write_end;
};

// duplicateAtLeast 使用 CLOEXEC 复制描述符，保证 exec 不会泄漏父进程端点。
UniqueFd duplicateAtLeast(int source, int minimum, const std::string& action) {
    const int duplicate = ::fcntl(source, F_DUPFD_CLOEXEC, minimum);
    if(duplicate < 0) {
        throw posixError(action);
    }
    return UniqueFd(duplicate);
}

// createManagedPipe 使用 Linux pipe2 原子设置 CLOEXEC，避免多线程启动时的继承窗口。
ManagedPipe createManagedPipe(const std::string& action) {
    int raw_pipe[2] = {-1, -1};
    if(::pipe2(raw_pipe, O_CLOEXEC) != 0) {
        throw posixError(action);
    }
    UniqueFd raw_read(raw_pipe[0]);
    UniqueFd raw_write(raw_pipe[1]);
    ManagedPipe result;
    result.read_end = duplicateAtLeast(raw_read.get(), 10, action);
    result.write_end = duplicateAtLeast(raw_write.get(), 10, action);
    return result;
}

// setNonBlocking 让读写 Worker 可以通过 poll 同时观察取消管道，关闭不会永久卡住。
void setNonBlocking(int fd, const std::string& action) {
    const int current = ::fcntl(fd, F_GETFL, 0);
    if(current < 0 || ::fcntl(fd, F_SETFL, current | O_NONBLOCK) != 0) {
        throw posixError(action);
    }
}

// validateOptions 在创建任何 OS 资源前拒绝不完整、不可执行或含 NUL 的配置。
void validateOptions(const ProcessOptions& options) {
    const auto contains_forbidden_control = [](const std::string& value) {
        return value.find('\0') != std::string::npos || value.find('\r') != std::string::npos ||
               value.find('\n') != std::string::npos;
    };
    std::error_code error;
    if(options.executable.empty() || !options.executable.is_absolute()) {
        throw std::invalid_argument("子进程可执行文件必须是绝对路径");
    }
    const std::string executable_text = options.executable.native();
    if(contains_forbidden_control(executable_text)) {
        throw std::invalid_argument("子进程可执行文件路径包含非法控制字符");
    }
    if(!std::filesystem::is_regular_file(options.executable, error) || error ||
       ::access(options.executable.c_str(), X_OK) != 0) {
        throw std::invalid_argument("子进程可执行文件不存在、不是普通文件或没有执行权限");
    }
    if(!options.working_directory.empty()) {
        if(contains_forbidden_control(options.working_directory.native())) {
            throw std::invalid_argument("子进程工作目录包含非法控制字符");
        }
        error.clear();
        if(!options.working_directory.is_absolute() ||
           !std::filesystem::is_directory(options.working_directory, error) || error) {
            throw std::invalid_argument("子进程工作目录必须是存在的绝对目录");
        }
    }
    for(const std::string& argument : options.arguments) {
        if(contains_forbidden_control(argument)) {
            throw std::invalid_argument("子进程参数不能包含 NUL、回车或换行");
        }
    }
    for(const auto& [name, value] : options.environment) {
        if(name.empty() || name.find('=') != std::string::npos || contains_forbidden_control(name)) {
            throw std::invalid_argument("子进程环境变量名称非法");
        }
        if(contains_forbidden_control(value)) {
            throw std::invalid_argument("子进程环境变量值不能包含 NUL、回车或换行");
        }
    }
}

// buildEnvironment 在 fork 前完成所有分配，子进程分支只使用已经稳定的 char 指针。
std::vector<std::string> buildEnvironment(const ProcessOptions& options) {
    std::map<std::string, std::string> values;
    if(options.inherit_parent_environment && environ != nullptr) {
        for(char** entry = environ; *entry != nullptr; ++entry) {
            const std::string item(*entry);
            const std::size_t separator = item.find('=');
            if(separator != std::string::npos && separator != 0U) {
                values[item.substr(0, separator)] = item.substr(separator + 1U);
            }
        }
    }
    for(const auto& [name, value] : options.environment) {
        values[name] = value;
    }

    std::vector<std::string> environment;
    environment.reserve(values.size());
    for(const auto& [name, value] : values) {
        environment.push_back(name + "=" + value);
    }
    return environment;
}

// ChildFailure 是 fork 后唯一允许写入错误管道的固定尺寸记录。
// stage 只标识失败动作，父进程负责生成简体中文错误信息。
struct ChildFailure {
    int stage = 0;
    int error = 0;
};

enum class ChildStage : int {
    ProcessGroup = 1,
    StandardStreams = 2,
    WorkingDirectory = 3,
    Execute = 4,
};

// reportChildFailure 只调用 async-signal-safe 的 write 和 _exit。
[[noreturn]] void reportChildFailure(int error_fd, ChildStage stage, int error) noexcept {
    const ChildFailure failure{static_cast<int>(stage), error};
    const char* bytes = reinterpret_cast<const char*>(&failure);
    std::size_t offset = 0;
    while(offset < sizeof(failure)) {
        const ssize_t written = ::write(error_fd, bytes + offset, sizeof(failure) - offset);
        if(written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if(written < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    _exit(127);
}

// closeInheritedDescriptors 只保留 0、1、2 和 CLOEXEC 的错误 FD 3。
// 新内核使用 close_range；旧内核退化为 fork 前确定上限的 close 循环。
void closeInheritedDescriptors(long maximum_fd) noexcept {
#ifdef SYS_close_range
    if(::syscall(SYS_close_range, 4U, std::numeric_limits<unsigned int>::max(), 0U) == 0) {
        return;
    }
#endif
    const long safe_maximum = maximum_fd > 4 ? maximum_fd : 1024;
    for(long fd = 4; fd < safe_maximum; ++fd) {
        ::close(static_cast<int>(fd));
    }
}

// childStageText 将固定阶段编号映射为稳定中文动作，不接触原始配置。
std::string childStageText(int stage) {
    switch(static_cast<ChildStage>(stage)) {
        case ChildStage::ProcessGroup:
            return "创建子进程进程组失败";
        case ChildStage::StandardStreams:
            return "重定向子进程标准流失败";
        case ChildStage::WorkingDirectory:
            return "切换子进程工作目录失败";
        case ChildStage::Execute:
            return "执行 stdio MCP 子进程失败";
    }
    return "启动 stdio MCP 子进程失败";
}

// terminateAndReap 用于 start 的事务回滚；进程组未建立时仍会单独终止根进程。
void terminateAndReap(pid_t pid) noexcept {
    if(pid <= 0) {
        return;
    }
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
    int status = 0;
    while(::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

// SpawnGuard 保证 fork 成功后的任何父进程异常都会回收尚未发布的子进程。
class SpawnGuard final {
   public:
    explicit SpawnGuard(pid_t pid) noexcept : pid_(pid) {}
    ~SpawnGuard() noexcept {
        if(active_) {
            terminateAndReap(pid_);
        }
    }
    SpawnGuard(const SpawnGuard&) = delete;
    SpawnGuard& operator=(const SpawnGuard&) = delete;
    void release() noexcept {
        active_ = false;
    }

   private:
    pid_t pid_ = -1;
    bool active_ = true;
};

// ScopedSigpipeBlock 把 SIGPIPE 影响限制在当前写调用，不改变宿主进程的全局信号配置。
class ScopedSigpipeBlock final {
   public:
    ScopedSigpipeBlock() {
        sigemptyset(&blocked_set_);
        sigaddset(&blocked_set_, SIGPIPE);
        const int error = pthread_sigmask(SIG_BLOCK, &blocked_set_, &previous_set_);
        if(error != 0) {
            throw posixError("屏蔽子进程管道 SIGPIPE 失败", error);
        }
        active_ = true;
        previously_blocked_ = sigismember(&previous_set_, SIGPIPE) == 1;
    }

    ~ScopedSigpipeBlock() noexcept {
        if(!active_) {
            return;
        }
        // 若本线程原先未屏蔽 SIGPIPE，恢复前先消费本次 EPIPE 产生的待处理信号。
        if(!previously_blocked_) {
            timespec no_wait{};
            while(::sigtimedwait(&blocked_set_, nullptr, &no_wait) >= 0) {
            }
        }
        pthread_sigmask(SIG_SETMASK, &previous_set_, nullptr);
    }

    ScopedSigpipeBlock(const ScopedSigpipeBlock&) = delete;
    ScopedSigpipeBlock& operator=(const ScopedSigpipeBlock&) = delete;

   private:
    sigset_t blocked_set_{};
    sigset_t previous_set_{};
    bool active_ = false;
    bool previously_blocked_ = false;
};

enum class IoChannel {
    Stdin,
    Stdout,
    Stderr,
};

// pollTimeout 把剩余单调时长安全压缩到 poll 的 int 毫秒范围。
int pollTimeout(std::chrono::steady_clock::time_point deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if(now >= deadline) {
        return 0;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return static_cast<int>(
        std::min<long long>(std::max<long long>(1, remaining.count()), std::numeric_limits<int>::max()));
}

}  // namespace

// Impl 保存根 PID、独立进程组以及可被同一个取消管道唤醒的三个标准流端点。
struct Process::Impl {
    pid_t pid = -1;
    pid_t process_group = -1;
    UniqueFd pidfd;
    UniqueFd stdin_write;
    UniqueFd stdout_read;
    UniqueFd stderr_read;
    UniqueFd cancel_read;
    UniqueFd cancel_write;

    std::mutex state_mutex;
    std::mutex wait_mutex;
    bool closed = false;
    bool exit_code_known = false;
    int exit_code = 0;

    std::mutex io_mutex;
    std::condition_variable io_finished;
    bool io_cancelled = false;
    bool stdin_closed = false;
    bool stdin_active = false;
    bool stdout_active = false;
    bool stderr_active = false;

    // beginIo 绑定单个标准流 Worker，并返回在 close 前保持有效的描述符快照。
    bool beginIo(IoChannel channel, int& fd, int& cancel_fd) {
        std::lock_guard<std::mutex> lock(io_mutex);
        UniqueFd* selected = nullptr;
        bool* active = nullptr;
        switch(channel) {
            case IoChannel::Stdin:
                selected = &stdin_write;
                active = &stdin_active;
                break;
            case IoChannel::Stdout:
                selected = &stdout_read;
                active = &stdout_active;
                break;
            case IoChannel::Stderr:
                selected = &stderr_read;
                active = &stderr_active;
                break;
        }
        if(io_cancelled || (channel == IoChannel::Stdin && stdin_closed) || !*selected || !cancel_read) {
            return false;
        }
        if(*active) {
            throw std::logic_error("同一子进程标准流不能并发读写");
        }
        *active = true;
        fd = selected->get();
        cancel_fd = cancel_read.get();
        return true;
    }

    // finishIo 与 close 使用同一条件变量，确保描述符只在 Worker 退出后释放。
    void finishIo(IoChannel channel) noexcept {
        std::lock_guard<std::mutex> lock(io_mutex);
        switch(channel) {
            case IoChannel::Stdin:
                stdin_active = false;
                break;
            case IoChannel::Stdout:
                stdout_active = false;
                break;
            case IoChannel::Stderr:
                stderr_active = false;
                break;
        }
        io_finished.notify_all();
    }

    // readChannel 用 poll 同时等待数据、EOF 与全局取消，不使用任意休眠轮询。
    std::size_t readChannel(IoChannel channel, char* buffer, std::size_t capacity) {
        int fd = -1;
        int cancel_fd = -1;
        if(!beginIo(channel, fd, cancel_fd)) {
            return 0U;
        }

        int failure = 0;
        std::size_t received = 0U;
        bool finished = false;
        while(!finished) {
            std::array<pollfd, 2> descriptors{
                {{fd, static_cast<short>(POLLIN | POLLHUP), 0}, {cancel_fd, POLLIN, 0}}
            };
            const int poll_result = ::poll(descriptors.data(), descriptors.size(), -1);
            if(poll_result < 0) {
                if(errno == EINTR) {
                    continue;
                }
                failure = errno;
                break;
            }
            if((descriptors[1].revents & POLLIN) != 0) {
                finished = true;
                break;
            }
            if((descriptors[0].revents & POLLNVAL) != 0) {
                failure = EBADF;
                break;
            }
            if((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                const ssize_t result = ::read(fd, buffer, capacity);
                if(result > 0) {
                    received = static_cast<std::size_t>(result);
                    finished = true;
                    break;
                }
                if(result == 0) {
                    finished = true;
                    break;
                }
                if(errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                failure = errno;
                break;
            }
        }
        finishIo(channel);
        if(failure != 0) {
            throw posixError(channel == IoChannel::Stdout ? "读取子进程 stdout 失败" : "读取子进程 stderr 失败",
                             failure);
        }
        return received;
    }

    // signalCancellation 让取消管道保持可读，所有当前和后续 poll 都会立即收敛。
    void signalCancellation() noexcept {
        if(!cancel_write) {
            return;
        }
        const char marker = 1;
        const ssize_t ignored = ::write(cancel_write.get(), &marker, sizeof(marker));
        (void)ignored;
    }
};

Process::Process(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Process::~Process() noexcept {
    close();
}

Process::Process(Process&& other) noexcept : impl_(std::move(other.impl_)) {}

Process& Process::operator=(Process&& other) noexcept {
    if(this != &other) {
        close();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

// start 在 fork 前准备 argv、环境和全部管道；子分支只执行 async-signal-safe 系统调用。
Process Process::start(ProcessOptions options, std::chrono::steady_clock::time_point absolute_deadline) {
    // 截止时间由 MCPClient 在公开 connect 起点生成；平台层不能重新启动一段 startup_timeout。
    if(std::chrono::steady_clock::now() >= absolute_deadline) {
        throw std::runtime_error("启动 stdio MCP 子进程超时");
    }
    validateOptions(options);
    if(std::chrono::steady_clock::now() >= absolute_deadline) {
        throw std::runtime_error("启动 stdio MCP 子进程超时");
    }

    std::vector<std::string> arguments;
    arguments.reserve(options.arguments.size() + 1U);
    arguments.push_back(options.executable.string());
    arguments.insert(arguments.end(), options.arguments.begin(), options.arguments.end());
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1U);
    for(std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> environment = buildEnvironment(options);
    std::vector<char*> envp;
    envp.reserve(environment.size() + 1U);
    for(std::string& entry : environment) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    ManagedPipe stdin_pipe = createManagedPipe("创建子进程 stdin 管道失败");
    ManagedPipe stdout_pipe = createManagedPipe("创建子进程 stdout 管道失败");
    ManagedPipe stderr_pipe = createManagedPipe("创建子进程 stderr 管道失败");
    ManagedPipe error_pipe = createManagedPipe("创建子进程启动错误管道失败");
    const long maximum_fd = ::sysconf(_SC_OPEN_MAX);

    // fork 前固化子分支所需的全部指针与描述符，避免在复制后的多线程地址空间调用 C++ 对象方法。
    const char* executable_path = options.executable.c_str();
    const char* working_directory = options.working_directory.empty() ? nullptr : options.working_directory.c_str();
    char* const* argv_data = argv.data();
    char* const* envp_data = envp.data();
    const int child_stdin = stdin_pipe.read_end.get();
    const int child_stdout = stdout_pipe.write_end.get();
    const int child_stderr = stderr_pipe.write_end.get();
    const int child_error = error_pipe.write_end.get();

    const pid_t pid = ::fork();
    if(pid < 0) {
        throw posixError("创建 stdio MCP 子进程失败");
    }
    if(pid == 0) {
        // 独立进程组使终止逻辑可以覆盖 Server 创建的受控子孙进程。
        if(::setpgid(0, 0) != 0) {
            reportChildFailure(child_error, ChildStage::ProcessGroup, errno);
        }
        if(::dup3(child_error, 3, O_CLOEXEC) < 0 || ::dup2(child_stdin, STDIN_FILENO) < 0 ||
           ::dup2(child_stdout, STDOUT_FILENO) < 0 || ::dup2(child_stderr, STDERR_FILENO) < 0) {
            reportChildFailure(child_error, ChildStage::StandardStreams, errno);
        }
        closeInheritedDescriptors(maximum_fd);
        if(working_directory != nullptr && ::chdir(working_directory) != 0) {
            reportChildFailure(3, ChildStage::WorkingDirectory, errno);
        }
        ::execve(executable_path, argv_data, envp_data);
        reportChildFailure(3, ChildStage::Execute, errno);
    }

    SpawnGuard spawn_guard(pid);
    stdin_pipe.read_end.reset();
    stdout_pipe.write_end.reset();
    stderr_pipe.write_end.reset();
    error_pipe.write_end.reset();

    // 父进程补做 setpgid 以关闭 fork 后根进程快速 exec 的竞态窗口。
    if(::setpgid(pid, pid) != 0 && errno != EACCES && errno != ESRCH) {
        throw posixError("确认 stdio MCP 子进程进程组失败");
    }

    ChildFailure child_failure{};
    std::size_t failure_bytes = 0U;
    while(failure_bytes < sizeof(child_failure)) {
        // 启动错误管道在 exec 成功时因 CLOEXEC 关闭；poll 同时等待错误结构或确定 EOF。
        // 每次等待都复用调用方绝对截止，避免子进程卡在 exec 前使 connect 永久阻塞。
        pollfd descriptor{error_pipe.read_end.get(), static_cast<short>(POLLIN | POLLHUP), 0};
        const int poll_result = ::poll(&descriptor, 1, pollTimeout(absolute_deadline));
        if(poll_result == 0 || std::chrono::steady_clock::now() >= absolute_deadline) {
            throw std::runtime_error("启动 stdio MCP 子进程超时");
        }
        if(poll_result < 0) {
            if(errno == EINTR) {
                continue;
            }
            throw posixError("等待 stdio MCP 子进程启动状态失败");
        }
        if((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            throw std::runtime_error("等待 stdio MCP 子进程启动状态失败");
        }
        if((descriptor.revents & (POLLIN | POLLHUP)) == 0) {
            continue;
        }

        const ssize_t received =
            ::read(error_pipe.read_end.get(), reinterpret_cast<char*>(&child_failure) + failure_bytes,
                   sizeof(child_failure) - failure_bytes);
        if(received > 0) {
            failure_bytes += static_cast<std::size_t>(received);
            continue;
        }
        if(received == 0) {
            break;
        }
        if(errno == EINTR) {
            continue;
        }
        throw posixError("读取 stdio MCP 子进程启动状态失败");
    }
    if(std::chrono::steady_clock::now() >= absolute_deadline) {
        throw std::runtime_error("启动 stdio MCP 子进程超时");
    }
    if(failure_bytes != 0U) {
        if(failure_bytes != sizeof(child_failure)) {
            throw std::runtime_error("stdio MCP 子进程启动状态不完整");
        }
        throw posixError(childStageText(child_failure.stage), child_failure.error);
    }

    ManagedPipe cancel_pipe = createManagedPipe("创建子进程 I/O 取消管道失败");
    setNonBlocking(stdin_pipe.write_end.get(), "设置子进程 stdin 非阻塞模式失败");
    setNonBlocking(stdout_pipe.read_end.get(), "设置子进程 stdout 非阻塞模式失败");
    setNonBlocking(stderr_pipe.read_end.get(), "设置子进程 stderr 非阻塞模式失败");
    setNonBlocking(cancel_pipe.write_end.get(), "设置子进程 I/O 取消管道非阻塞模式失败");

    auto impl = std::make_unique<Impl>();
    impl->pid = pid;
    impl->process_group = pid;
    impl->stdin_write = std::move(stdin_pipe.write_end);
    impl->stdout_read = std::move(stdout_pipe.read_end);
    impl->stderr_read = std::move(stderr_pipe.read_end);
    impl->cancel_read = std::move(cancel_pipe.read_end);
    impl->cancel_write = std::move(cancel_pipe.write_end);
#ifdef SYS_pidfd_open
    const int pidfd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0U));
    if(pidfd >= 0) {
        impl->pidfd.reset(pidfd);
    }
#endif
    // 只有完整平台对象仍位于同一启动预算内时才释放 SpawnGuard 的进程树所有权。
    if(std::chrono::steady_clock::now() >= absolute_deadline) {
        throw std::runtime_error("启动 stdio MCP 子进程超时");
    }
    spawn_guard.release();
    return Process(std::move(impl));
}

std::size_t Process::readStdout(char* buffer, std::size_t capacity) {
    if(buffer == nullptr || capacity == 0U) {
        throw std::invalid_argument("子进程读取缓冲区不能为空");
    }
    if(!impl_) {
        return 0U;
    }
    return impl_->readChannel(IoChannel::Stdout, buffer, capacity);
}

std::size_t Process::readStderr(char* buffer, std::size_t capacity) {
    if(buffer == nullptr || capacity == 0U) {
        throw std::invalid_argument("子进程读取缓冲区不能为空");
    }
    if(!impl_) {
        return 0U;
    }
    return impl_->readChannel(IoChannel::Stderr, buffer, capacity);
}

void Process::writeStdin(std::string_view data) {
    if(data.empty()) {
        return;
    }
    if(!impl_) {
        throw std::logic_error("子进程已经关闭，不能写入 stdin");
    }

    int fd = -1;
    int cancel_fd = -1;
    if(!impl_->beginIo(IoChannel::Stdin, fd, cancel_fd)) {
        throw std::runtime_error("子进程 stdin 已关闭或写入已取消");
    }

    int failure = 0;
    bool cancelled = false;
    bool made_no_progress = false;
    std::size_t offset = 0U;
    try {
        ScopedSigpipeBlock sigpipe_block;
        while(offset < data.size()) {
            std::array<pollfd, 2> descriptors{
                {{fd, POLLOUT, 0}, {cancel_fd, POLLIN, 0}}
            };
            const int poll_result = ::poll(descriptors.data(), descriptors.size(), -1);
            if(poll_result < 0) {
                if(errno == EINTR) {
                    continue;
                }
                failure = errno;
                break;
            }
            if((descriptors[1].revents & POLLIN) != 0) {
                cancelled = true;
                break;
            }
            if((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                failure = EPIPE;
                break;
            }
            if((descriptors[0].revents & POLLOUT) == 0) {
                continue;
            }
            const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
            if(written > 0) {
                offset += static_cast<std::size_t>(written);
                continue;
            }
            if(written == 0) {
                made_no_progress = true;
                break;
            }
            if(errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            failure = errno;
            break;
        }
    } catch(...) {
        impl_->finishIo(IoChannel::Stdin);
        throw;
    }
    impl_->finishIo(IoChannel::Stdin);
    if(cancelled) {
        throw std::runtime_error("子进程 stdin 写入已取消");
    }
    if(made_no_progress) {
        throw std::runtime_error("写入子进程 stdin 时未产生进度");
    }
    if(failure == EPIPE) {
        throw std::runtime_error("子进程已关闭 stdin，无法继续写入");
    }
    if(failure != 0) {
        throw posixError("写入子进程 stdin 失败", failure);
    }
}

void Process::closeStdin() noexcept {
    if(!impl_) {
        return;
    }
    std::unique_lock<std::mutex> lock(impl_->io_mutex);
    if(impl_->stdin_closed) {
        return;
    }
    impl_->stdin_closed = true;
    if(impl_->stdin_active) {
        impl_->io_cancelled = true;
        impl_->signalCancellation();
    }
    impl_->io_finished.wait(lock, [this] { return !impl_->stdin_active; });
    impl_->stdin_write.reset();
}

// decodeExitStatus 统一正常退出和信号终止，信号采用 shell 兼容的 128 + signal 形式。
static int decodeExitStatus(int status) noexcept {
    if(WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if(WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return status;
}

bool Process::waitForExit(std::chrono::milliseconds timeout, int& exit_code) {
    if(!impl_) {
        exit_code = 0;
        return true;
    }
    std::lock_guard<std::mutex> wait_lock(impl_->wait_mutex);
    {
        std::lock_guard<std::mutex> state_lock(impl_->state_mutex);
        if(impl_->exit_code_known) {
            exit_code = impl_->exit_code;
            return true;
        }
    }

    const auto non_negative_timeout = std::max(timeout, std::chrono::milliseconds::zero());
    const auto deadline = saturatingSteadyDeadlineAfter(non_negative_timeout);
    int status = 0;
    while(true) {
        // 先用 WNOWAIT 观察根进程终态，在保留僵尸 PID 的窗口内清理同组子孙，避免 PGID 复用误伤无关进程。
        siginfo_t child_info{};
        const int inspect_result =
            ::waitid(P_PID, static_cast<id_t>(impl_->pid), &child_info, WEXITED | WNOHANG | WNOWAIT);
        if(inspect_result == 0 && child_info.si_pid == impl_->pid) {
            int process_group_error = 0;
            if(impl_->process_group > 0 && ::kill(-impl_->process_group, SIGKILL) != 0 && errno != ESRCH) {
                process_group_error = errno;
            }
            pid_t reap_result = -1;
            do {
                reap_result = ::waitpid(impl_->pid, &status, 0);
            } while(reap_result < 0 && errno == EINTR);
            if(reap_result != impl_->pid) {
                throw posixError("回收 stdio MCP 根进程失败");
            }
            const int decoded = decodeExitStatus(status);
            std::lock_guard<std::mutex> state_lock(impl_->state_mutex);
            impl_->exit_code = decoded;
            impl_->exit_code_known = true;
            exit_code = decoded;
            if(process_group_error != 0) {
                throw posixError("回收 stdio MCP 子进程组失败", process_group_error);
            }
            return true;
        }
        if(inspect_result < 0 && errno != EINTR) {
            throw posixError("等待 stdio MCP 子进程退出失败");
        }
        if(std::chrono::steady_clock::now() >= deadline) {
            return false;
        }

        const int remaining_ms = pollTimeout(deadline);
        if(impl_->pidfd) {
            pollfd descriptor{impl_->pidfd.get(), POLLIN, 0};
            const int poll_result = ::poll(&descriptor, 1, remaining_ms);
            if(poll_result < 0 && errno != EINTR) {
                throw posixError("等待 stdio MCP 子进程 pidfd 失败");
            }
        } else {
            // 旧内核没有 pidfd 时使用短、有界的 waitpid 探测；总时长仍由单调截止控制。
            const int fallback_wait = std::min(remaining_ms, 10);
            const int poll_result = ::poll(nullptr, 0, fallback_wait);
            if(poll_result < 0 && errno != EINTR) {
                throw posixError("等待 stdio MCP 子进程退出失败");
            }
        }
    }
}

void Process::terminateTree() {
    if(!impl_) {
        return;
    }

    pid_t pid = -1;
    pid_t process_group = -1;
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if(impl_->closed || impl_->exit_code_known) {
            return;
        }
        pid = impl_->pid;
        process_group = impl_->process_group;
    }

    if(process_group > 0 && ::kill(-process_group, SIGKILL) != 0 && errno != ESRCH) {
        throw posixError("终止 stdio MCP 子进程组失败");
    }
    // 若根进程在 setpgid 前退出或组已消失，单独信号仍可关闭竞态窗口。
    if(pid > 0 && ::kill(pid, SIGKILL) != 0 && errno != ESRCH) {
        throw posixError("终止 stdio MCP 根进程失败");
    }
}

void Process::cancelIo() noexcept {
    if(!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->io_mutex);
    if(impl_->io_cancelled) {
        return;
    }
    impl_->io_cancelled = true;
    impl_->stdin_closed = true;
    impl_->signalCancellation();
}

void Process::close() noexcept {
    if(!impl_) {
        return;
    }

    cancelIo();
    {
        std::unique_lock<std::mutex> lock(impl_->io_mutex);
        impl_->io_finished.wait(
            lock, [this] { return !impl_->stdin_active && !impl_->stdout_active && !impl_->stderr_active; });
        impl_->stdin_write.reset();
        impl_->stdout_read.reset();
        impl_->stderr_read.reset();
        impl_->cancel_write.reset();
        impl_->cancel_read.reset();
    }

    bool termination_requested = false;
    bool reaped = false;
    try {
        // Process 最终关闭时始终终止残留进程组；正常握手应由 Transport 在此前完成等待。
        terminateTree();
        termination_requested = true;
        int ignored_exit_code = 0;
        reaped = waitForExit(std::chrono::seconds(5), ignored_exit_code);
    } catch(...) {
        // close 不传播清理异常；根进程的 waitpid 已在可行路径尽力执行。
    }
    if(termination_requested && !reaped) {
        // SIGKILL 已经成功发送；直接 waitpid 不再发信号，因此即使其他路径已回收 PID 也不会误伤无关进程。
        int ignored_status = 0;
        while(::waitpid(impl_->pid, &ignored_status, 0) < 0 && errno == EINTR) {
        }
    }

    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        impl_->closed = true;
        impl_->pidfd.reset();
    }
    impl_.reset();
}

}  // namespace detail
}  // namespace aiSDK
