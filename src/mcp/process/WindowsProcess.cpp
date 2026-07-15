#include "mcp/detail/Process.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace aiSDK {
namespace detail {
namespace {

// Windows 进程边界总则
// - 本文件只实现 stdio MCP 所需的直接子进程能力，不解释 JSON-RPC。
// - ProcessOptions 已由上层静态校验，本层仍执行防御性 OS 前置检查。
// - 所有平台对象都封闭在 Impl 中，公共 MCP 头文件不会出现 HANDLE。
// - 实现不调用 cmd.exe、PowerShell、ShellExecute 或其他命令解释器。
// - executable 始终通过 CreateProcessW 的 lpApplicationName 单独传入。
// - arguments 只形成可逆 argv，不参与路径选择、重定向或变量展开。
// - stdout 与 stderr 使用独立管道，协议字节和诊断字节不能混流。
// - 任一启动阶段失败都必须保留强异常安全，不发布半初始化 Process。
// - 析构和 close 是最后资源边界，不能把清理异常传播到宿主。
// 配置校验不变量
// - 可执行文件必须为存在的绝对普通文件，拒绝依赖当前目录搜索。
// - 工作目录为空表示调用方未指定，非空时必须为存在的绝对目录。
// - CMD 与 BAT 属于脚本入口，首版必须由调用方显式选择解释器。
// - 每个参数都保留独立边界，NUL、回车和换行在创建资源前拒绝。
// - 环境变量名称不能为空，不能包含等号或控制边界字符。
// - 环境变量值允许普通 UTF-8 内容，但拒绝 NUL、回车和换行。
// - 校验错误只描述字段类别，不回显完整可执行路径或秘密值。
// - 文件存在性使用 error_code 形式，避免文件系统异常携带敏感路径。
// - 防御性校验不能替代 MCPServerConfig 的无 I/O 静态校验。
// 字符编码不变量
// - filesystem::path 在 Windows 保持宽字符原生表示，不先转成窄字符串。
// - 外部参数和环境映射按项目约定解释为严格 UTF-8。
// - MultiByteToWideChar 使用 MB_ERR_INVALID_CHARS，禁止静默替换坏字节。
// - 空字符串是合法 argv 或环境值，转换函数必须保留其语义。
// - 转换失败只报告字段类别和数字系统错误，不复制原始字节。
// - CreateProcessW 固定使用 Unicode 环境块，避免系统代码页漂移。
// - 中文、空格和非 ASCII 参数都必须与调用方传入值一一对应。
// - 路径由 filesystem 负责宽字符生命周期，启动期间不得保存悬空指针。
// - 任何新增文本入口都必须先明确 UTF-8 或原生路径的所有权。
// Windows argv 编码不变量
// - CreateProcessW 接收命令行字符串，但子进程最终观察的是 argv 数组。
// - argv[0] 固定为真实 executable，不能由 arguments 覆盖。
// - 不含空白和引号的普通参数可以原样加入命令行。
// - 空参数必须显式加引号，否则会在子进程中消失。
// - 引号前的反斜杠按二倍加一规则转义。
// - 闭合引号前的尾部反斜杠必须加倍，避免吞掉闭合引号。
// - 命令元字符没有 shell 语义，只作为普通参数字符保留。
// - 命令行缓冲必须可写并以 NUL 结尾，满足 CreateProcessW 合同。
// - 参数编码测试应覆盖空值、空格、引号、尾部反斜杠和中文。
// 环境块构造不变量
// - inherit_parent_environment 为 false 时只传递显式环境映射。
// - 继承开启时先复制父环境，再用显式映射按名称覆盖。
// - Windows 环境名称按不区分大小写规则去重和排序。
// - 父环境中的 =C: 等驱动器工作目录项需要保留原始名称形状。
// - 环境块中的每一项使用 name=value 加单 NUL 终止。
// - 完整环境块使用额外 NUL 终止，空环境仍需要双 NUL。
// - GetEnvironmentStringsW 返回的内存必须在正常和异常路径释放。
// - 环境秘密只进入子进程环境块，不进入异常、Trace 或默认日志。
// - 未来增加环境过滤时必须保持显式值覆盖父环境的优先级。
// 管道方向不变量
// - stdin 管道由子进程持有读端，父进程 Transport 持有写端。
// - stdout 管道由子进程持有写端，父进程 Reader 持有读端。
// - stderr 与 stdout 使用相同方向但完全独立的句柄对。
// - 创建管道时临时允许继承，随后立即清除所有父端继承标志。
// - 子端仅在 CreateProcessW 调用窗口内由局部 RAII 对象持有。
// - 进程启动后父进程必须关闭三个子端，否则 EOF 永远不会到达。
// - 父端只由 Process::Impl 持有，移动 Process 不复制句柄。
// - 读取零字节或 broken pipe 都归一化为流结束。
// - 管道错误不能把 stderr 原文混入公开异常。
// 句柄继承白名单
// - bInheritHandles 必须为 TRUE 才能传入标准流子句柄。
// - TRUE 不能代表继承全部句柄，必须配合 PROC_THREAD_ATTRIBUTE_HANDLE_LIST。
// - 白名单中只出现 child stdin、child stdout 和 child stderr。
// - 父端句柄通过 SetHandleInformation 明确禁止继承。
// - 属性列表缓冲由 RAII 容器持有到 CreateProcessW 返回。
// - UpdateProcThreadAttribute 失败时必须先删除已初始化属性列表。
// - 宿主文件、网络、Token 和其他线程句柄不能进入子进程。
// - 未来新增控制管道时必须显式评审是否属于继承白名单。
// - 测试应验证子进程不会因为父端误继承导致关闭永久等待。
// 启动事务不变量
// - 所有确定性字符串与路径校验发生在 CreateProcessW 之前。
// - 三个管道和 Job Object 都由局部 RAII 持有，失败可逆。
// - 新进程使用 CREATE_SUSPENDED，避免加入 Job Object 前逃逸创建子孙。
// - AssignProcessToJobObject 成功后才允许恢复主线程。
// - 加入 Job 失败时直接终止尚未恢复的根进程并等待回收。
// - ResumeThread 失败时终止整个 Job，而不是只关闭本地句柄。
// - Impl 分配失败时 Job 局部析构仍通过 KILL_ON_JOB_CLOSE 回收进程树。
// - 只有全部启动步骤完成后才移动资源并返回 Process。
// - start 不等待 MCP initialize，协议启动超时属于上层 Client。
// Job Object 不变量
// - 每个 Process 创建独立 Job Object，不与其他 MCP Client 共享。
// - JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 是最终进程树回收保险。
// - 根进程在 suspended 状态加入 Job，关闭创建子孙的竞态窗口。
// - terminateTree 使用 TerminateJobObject 覆盖根进程和受控后代。
// - Job 句柄在 Process 生命周期内保持有效，不能提前释放。
// - 根进程正常退出后 Job 仍负责清理遗留后代。
// - 关闭 Job 前先尽力等待根进程，便于获得稳定退出码。
// - 数字系统错误可以用于诊断，但不能回显完整命令。
// - 若未来支持嵌套 Job，必须单独验证宿主 Job 的兼容策略。
// I/O Worker 所有权
// - 同一 stdin 在任意时刻只允许一个 Writer 调用 writeStdin。
// - 同一 stdout 在任意时刻只允许一个 Reader 调用 readStdout。
// - 同一 stderr 在任意时刻只允许一个 Reader 调用 readStderr。
// - 并发重复访问同一流属于 Transport 实现错误，使用 logic_error 暴露。
// - 每次同步 I/O 注册当前线程的可取消内核句柄。
// - Worker 注册和活动标志由 io_mutex 统一保护。
// - 系统调用执行期间不持有 io_mutex，close 可以发出取消。
// - 完成路径先撤销活动标志，再通知 close 的条件变量。
// - Transport 必须在线程 join 后再销毁 Process 所有权。
// 同步 I/O 取消不变量
// - Windows 匿名管道不提供直接可等待的数据事件，本实现使用专用 Worker。
// - cancelIo 先设置不可逆取消状态，后续 beginIo 立即拒绝。
// - 已注册 Worker 通过 CancelSynchronousIo 解除 ReadFile 或 WriteFile。
// - 取消后关闭三个父端句柄，覆盖取消早于系统调用建立的窄竞态。
// - ERROR_OPERATION_ABORTED 在关闭路径中归一化为流结束或写入取消。
// - ERROR_INVALID_HANDLE 同样可能来自并发关闭，不应泄漏系统消息。
// - 取消不调用 Client 回调，也不持有 Client 状态锁。
// - 重复 cancelIo 必须是无操作，不能重复关闭已转移句柄。
// - close 等待全部活动 I/O 标志归零后才销毁 Impl。
// stdout 读取不变量
// - readStdout 只负责原始字节，不识别换行、UTF-8 或 JSON。
// - 调用方提供固定缓冲区，Process 不进行协议级无界累积。
// - capacity 为零或 buffer 为空属于明确调用方错误。
// - 单次 ReadFile 的长度限制在 DWORD 可表达范围内。
// - 正常读取得到的字节数原样返回，不添加字符串终止符。
// - ERROR_BROKEN_PIPE 和 ERROR_HANDLE_EOF 都表示 stdout 完成。
// - 关闭取消同样返回零，由 Transport 状态决定是否上报故障。
// - 其他读取错误只提供中文动作和数字错误码。
// - 逐行 framing 与残缺尾部判定留在 StdioMCPTransport。
// stderr 读取不变量
// - readStderr 与 stdout 使用相同字节合同但不同物理管道。
// - stderr 内容不参与 MCP 协议，也不能改变 JSON-RPC 关联。
// - Transport 必须持续排空 stderr，防止 Server 因管道写满死锁。
// - Process 不缓存 stderr，内存上限由 Transport 的尾部环形缓冲承担。
// - 调用方回调和脱敏策略不在本平台层执行。
// - EOF 与取消返回零，便于固定 Worker 退出。
// - 读取错误不附加 stderr 正文，避免秘密进入公开边界。
// - stdout 与 stderr Worker 可以并行，但各自只允许单读者。
// - 资源测试应使用超过管道容量的 stderr 噪声验证持续排空。
// stdin 写入不变量
// - writeStdin 不追加 LF，完整 JSON 行由准备消息阶段构造。
// - 空数据写入直接成功，不创建无意义系统调用。
// - 每次调用循环处理部分写入，直到所有字节提交或发生错误。
// - 单次 WriteFile 长度限制在 DWORD 范围内。
// - 零进度写入视为故障，防止 Worker 永久空转。
// - broken pipe 表示 Server 已关闭 stdin，使用稳定中文分类。
// - 取消写入与远端关闭分开分类，便于 Client 选择根因。
// - 写入期间不持有出站队列锁，其他控制消息可继续准备入队。
// - 消息原子性由唯一 Writer 队列保证，而不是依赖管道单写大小。
// stdin 正常关闭不变量
// - closeStdin 用于通知 Server 不再接收客户端字节。
// - 重复 closeStdin 保持无操作，不能关闭其他标准流。
// - 若 Writer 正在阻塞，先请求取消该 Worker。
// - 父 stdin 句柄关闭后子进程最终观察到 EOF。
// - 关闭过程中等待 stdin_active 归零，避免销毁 Worker 使用的状态。
// - 正常关闭不主动关闭 stdout 或 stderr，Server 仍可完成尾部输出。
// - Transport 应先停止接受新出站消息，再调用 closeStdin。
// - close 会在全局 cancelIo 后再次走幂等 stdin 清理。
// - 该方法 noexcept，系统清理失败由最终 Job 兜底。
// 退出等待不变量
// - waitForExit 使用内核进程对象等待，不依赖墙钟或任意 sleep。
// - 负超时按零处理，提供非阻塞状态探测。
// - 超时过大时裁剪到 WaitForSingleObject 的有限 DWORD 范围。
// - 等待使用复制进程句柄，避免与 close 争用同一所有权句柄。
// - WAIT_TIMEOUT 只返回 false，不修改调用方退出码。
// - WAIT_OBJECT_0 后读取 GetExitCodeProcess 并缓存。
// - 缓存退出码允许后续调用在进程句柄关闭前稳定重复读取。
// - WAIT_FAILED 等异常转换为中文动作和数字错误码。
// - 单调绝对截止和 shutdown_timeout 的编排仍属于 Transport。
// 终止语义不变量
// - terminateTree 是强制终止边界，不承担优雅 MCP 关闭。
// - 优雅流程应先关闭 stdin 并在上层等待 shutdown_timeout。
// - 超时后 TerminateJobObject 同时终止根进程和受控子孙。
// - 终止操作使用复制 Job 句柄，允许与状态读取并发。
// - 已关闭或缺少 Job 的 Process 直接返回。
// - 强制终止错误可以抛出，由显式调用方分类。
// - close 捕获终止错误并继续释放 Job，触发 KILL_ON_JOB_CLOSE。
// - 终止不会把命令、参数、环境或 stderr 放入异常。
// - 未来若增加软终止阶段，应新增明确 API 而非改变本方法语义。
// close 顺序不变量
// - close 首先取消全部 I/O，阻止 Worker 再进入平台调用。
// - 随后等待三个活动标志归零，确保 Impl 不被并发访问。
// - 若根进程仍存活，则调用 terminateTree 强制回收整个 Job。
// - 强制终止后尽力等待五秒获取根进程终态。
// - 等待或终止异常不能穿透 noexcept 关闭边界。
// - 最终先关闭进程句柄，再关闭 Job 触发子孙兜底回收。
// - impl_ reset 使重复 close 和移动后析构自然成为无操作。
// - close 不 detach 线程；线程 join 责任由持有 Worker 的 Transport 完成。
// - 上层 close_timeout 必须包围 Worker 收敛和本进程清理。
// 移动语义不变量
// - Process 禁止复制，避免两个对象同时关闭同一进程树。
// - 移动构造只转移唯一 Impl 所有权，不触发系统调用。
// - 移动赋值先关闭目标原有进程，再接管来源 Impl。
// - 移出对象的 impl_ 为空，析构不会重复清理。
// - Impl 内含 mutex 和 condition_variable，因此只移动 unique_ptr。
// - 平台句柄只在 Impl 地址稳定期间被 Worker 引用。
// - Transport 启动 Worker 后不应再次移动底层 Process。
// - 返回值优化和移动构造允许 start 安全按值返回。
// - 任何未来共享句柄需求都应通过显式观察副本而非复制 Process。
// 锁与竞态不变量
// - state_mutex 只保护进程句柄、关闭标志和退出码缓存。
// - io_mutex 只保护管道所有权、Worker 注册和取消状态。
// - 系统阻塞调用期间不持有 state_mutex 或 io_mutex。
// - 条件变量只用于等待活动 I/O 收敛，不用于消息队列。
// - 本层永远不获取 MCPClient 状态锁。
// - 本层永远不调用用户回调或凭据 Provider。
// - 句柄观察通过 DuplicateHandle 建立独立生命周期。
// - 取消状态一旦设置不再恢复，Process 实例不能重新打开。
// - 锁顺序不跨越 Client 和 Transport，避免形成反向依赖。
// 错误与隐私不变量
// - 所有自行生成的可读错误使用简体中文。
// - 平台诊断只附加 GetLastError 数值，不使用可能含路径的系统格式化文本。
// - 异常不包含完整 executable、working_directory 或 argv。
// - 异常不包含环境名称对应值，更不能包含凭据。
// - stderr 原文永远不参与 Process 异常构造。
// - 参数校验使用 invalid_argument，重复流访问使用 logic_error。
// - 外部运行期失败使用 runtime_error，符合现有 SDK 错误层次。
// - close 和析构吞掉清理异常，但显式 terminateTree 保留失败感知。
// - 上层 MCPException 负责把平台失败映射为闭合公开错误码。
// 性能与资源上限
// - 每个 Process 固定持有三对标准流管道和一个 Job Object。
// - Process 本身不按消息创建线程，也不缓存协议正文。
// - 读写直接使用调用方缓冲或 string_view，避免额外大对象复制。
// - 环境和命令行只在 start 期间构造一次。
// - 等待依赖内核对象，不进行高频轮询。
// - 退出码只缓存一个整数，状态内存与消息数量无关。
// - 句柄数量与并发请求数无关，多个请求复用同一 stdio 通道。
// - 大 stderr 的有界缓存由 Transport 负责，本层只负责排空。
// - 资源验收应重复启动关闭并观察宿主句柄数不持续增长。
// 安全边界
// - Server 配置被视为应用受信输入，但不允许模型动态改写。
// - 没有任何代码路径通过 shell 解释命令字符。
// - 精确句柄白名单防止宿主敏感资源泄漏给 Server。
// - 默认不继承父环境，避免 API Key 等秘密隐式进入子进程。
// - 显式环境覆盖只影响当前子进程，不修改宿主进程环境。
// - Job Object 确保恶意或故障 Server 不能通过普通子孙逃逸清理。
// - CREATE_NO_WINDOW 避免库调用意外创建可见控制台窗口。
// - 错误消息不回显受信配置中的秘密部分。
// - 协议消息大小与队列上限由 Transport 在调用 writeStdin 前执行。
// 与 Transport 的集成合同
// - Transport 在 open 中创建 Process，构造函数不得提前启动。
// - Writer Worker 是 writeStdin 的唯一调用方。
// - stdout Reader 把原始分块交给逐行 framer。
// - stderr Reader 只更新有界诊断尾部或显式回调。
// - Reaper 使用 waitForExit 观察根进程终态。
// - commitPrepared 只入队，不能在 Client 状态锁内调用 writeStdin。
// - close 先停止队列和回调，再取消 Process I/O。
// - 所有 Worker join 后，Transport 才调用 Process::close 或析构。
// - Process 返回的 EOF 必须结合 Closing 与根进程状态分类。
// 预期测试合同
// - 测试 Server 路径必须来自 CMake TARGET_FILE，不能拼接 exe 后缀。
// - 中文和空格路径验证宽字符 application name。
// - 参数测试覆盖 shell 元字符只作为普通 argv 到达。
// - 引号和反斜杠测试验证 Windows 可逆 quoting。
// - 显式环境和禁继承测试验证秘密边界。
// - stderr 洪泛测试验证独立 Reader 不阻塞协议。
// - hang 模式验证 cancelIo 能解除读写等待。
// - spawn-child 模式验证 Job 关闭后根进程和子孙均终止。
// - 重复 close 和移动对象测试验证资源所有权唯一。
// 维护约束
// - 新增 Win32 API 前必须确认其句柄继承和取消语义。
// - 不得把 Windows 类型移动到 Process.h 或任何公开 MCP 头。
// - 不得为了方便改用 system、popen 或 shell 启动。
// - 不得把 stderr 默认打印到 stdout、日志或 Trace。
// - 不得在此层增加 JSON 解析、请求 ID 或 Tools 语义。
// - 不得让 close 依赖调用方手工释放任何原生句柄。
// - 新增状态必须明确由 state_mutex 还是 io_mutex 保护。
// - 新增阻塞点必须能够被 cancelIo 或进程终止解除。
// - 任何性能优化都不能削弱句柄白名单和 Job 回收合同。
// 首版平台范围
// - 本条件源只在 WIN32 目标中参与 ai_sdk_mcp 编译。
// - Linux 使用独立 PosixProcess.cpp，不通过宏混合两套原生实现。
// - macOS 不在首版进程范围内，CMake 应在启用 MCP 时明确拒绝。
// - 公共 Process API 在两个条件源中保持完全相同的符号集合。
// - 两个平台都必须满足无 shell、分离 stderr 和进程树回收。
// - 平台退出码允许原生差异，上层只把它作为诊断事实。
// - 平台错误码不直接成为 MCP JSON-RPC 错误。
// - Windows 验证必须使用 MSVC /W4 与 UTF-8 编译选项。
// - 条件源选择属于构建图合同，不能同时编译两个实现。
// 完成线性化约束
// - Process 不决定工具请求是否 Submitted。
// - Submitted 线性化点发生在 Transport 出站队列成功入队。
// - writeStdin 失败时请求可能已经 Submitted，上层必须保守分类。
// - 部分写入后断线不能在本层自动重放剩余或完整消息。
// - Process 不缓存上一次消息，因此不存在隐式重试入口。
// - stdout EOF 不推断具体请求结果，只报告字节流终止。
// - 根进程退出和管道 EOF 可能乱序，Transport 负责闭合一次终命事件。
// - cancelIo 只终止本地等待，不声明远端工具未执行。
// - 进程资源完成和协议结果完成属于两个独立状态域。

// Windows 错误只公开稳定动作和数字错误码，不拼接命令行、环境或敏感路径。
std::runtime_error windowsError(const std::string& action, DWORD error = GetLastError()) {
    return std::runtime_error(action + "，Windows 错误码: " + std::to_string(error));
}

// UniqueHandle 集中管理 Win32 内核句柄，所有提前返回都能自动回收局部资源。
class UniqueHandle final {
   public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() noexcept {
        reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if(this != &other) {
            reset(other.release());
        }
        return *this;
    }

    HANDLE get() const noexcept {
        return handle_;
    }
    explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() noexcept {
        HANDLE result = handle_;
        handle_ = nullptr;
        return result;
    }
    void reset(HANDLE handle = nullptr) noexcept {
        if(*this) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

   private:
    HANDLE handle_ = nullptr;
};

// duplicateHandle 创建独立等待或取消句柄，避免 close 与观察线程共享同一所有权。
UniqueHandle duplicateHandle(HANDLE source, const std::string& action) {
    HANDLE duplicate = nullptr;
    if(!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate, 0, FALSE,
                        DUPLICATE_SAME_ACCESS)) {
        throw windowsError(action);
    }
    return UniqueHandle(duplicate);
}

// utf8ToWide 对 argv 和显式环境执行严格 UTF-8 校验，拒绝静默替换字符。
std::wstring utf8ToWide(const std::string& value, const std::string& field_name) {
    if(value.find('\0') != std::string::npos) {
        throw std::invalid_argument(field_name + "不能包含空字符");
    }
    if(value.empty()) {
        return L"";
    }
    if(value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument(field_name + "长度超过平台限制");
    }

    const int required =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if(required <= 0) {
        throw windowsError(field_name + "不是合法 UTF-8");
    }
    std::wstring converted(static_cast<std::size_t>(required), L'\0');
    if(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                           converted.data(), required) != required) {
        throw windowsError("转换" + field_name + "失败");
    }
    return converted;
}

// quoteWindowsArgument 实现 CommandLineToArgvW 兼容的反斜杠与引号规则。
// CreateProcessW 虽不经过 shell，仍要求调用方构造一条可逆的命令行字符串。
std::wstring quoteWindowsArgument(const std::wstring& argument) {
    const bool needs_quotes = argument.empty() || argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if(!needs_quotes) {
        return argument;
    }

    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t backslash_count = 0;
    for(const wchar_t character : argument) {
        if(character == L'\\') {
            ++backslash_count;
            continue;
        }
        if(character == L'"') {
            quoted.append(backslash_count * 2U + 1U, L'\\');
            quoted.push_back(L'"');
            backslash_count = 0;
            continue;
        }
        quoted.append(backslash_count, L'\\');
        backslash_count = 0;
        quoted.push_back(character);
    }
    quoted.append(backslash_count * 2U, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

// buildCommandLine 把真实可执行文件作为 argv[0]，其余参数逐项引用后连接。
std::vector<wchar_t> buildCommandLine(const ProcessOptions& options) {
    std::wstring command_line = quoteWindowsArgument(options.executable.native());
    for(const std::string& argument : options.arguments) {
        command_line.push_back(L' ');
        command_line.append(quoteWindowsArgument(utf8ToWide(argument, "子进程参数")));
    }
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    return mutable_command;
}

// Windows 环境变量名称按不区分大小写规则排序，避免 PATH 与 Path 形成重复项。
struct EnvironmentNameLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const noexcept {
        const int result = CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                                static_cast<int>(right.size()), TRUE);
        if(result == CSTR_LESS_THAN) {
            return true;
        }
        if(result == CSTR_GREATER_THAN || result == CSTR_EQUAL) {
            return false;
        }
        return left < right;
    }
};

// buildEnvironmentBlock 创建 CreateProcessW 所需的双空字符结尾 Unicode 环境块。
std::vector<wchar_t> buildEnvironmentBlock(const ProcessOptions& options) {
    std::map<std::wstring, std::wstring, EnvironmentNameLess> environment;
    if(options.inherit_parent_environment) {
        LPWCH parent_block = GetEnvironmentStringsW();
        if(parent_block == nullptr) {
            throw windowsError("读取父进程环境失败");
        }
        try {
            for(const wchar_t* entry = parent_block; *entry != L'\0'; entry += std::wcslen(entry) + 1U) {
                const std::wstring item(entry);
                const std::size_t separator = item.find(L'=', item.front() == L'=' ? 1U : 0U);
                if(separator != std::wstring::npos) {
                    environment[item.substr(0, separator)] = item.substr(separator + 1U);
                }
            }
        } catch(...) {
            FreeEnvironmentStringsW(parent_block);
            throw;
        }
        FreeEnvironmentStringsW(parent_block);
    }

    for(const auto& [name_utf8, value_utf8] : options.environment) {
        if(name_utf8.empty() || name_utf8.find('=') != std::string::npos || name_utf8.find('\0') != std::string::npos) {
            throw std::invalid_argument("子进程环境变量名称非法");
        }
        environment[utf8ToWide(name_utf8, "环境变量名称")] = utf8ToWide(value_utf8, "环境变量值");
    }

    std::vector<wchar_t> block;
    for(const auto& [name, value] : environment) {
        block.insert(block.end(), name.begin(), name.end());
        block.push_back(L'=');
        block.insert(block.end(), value.begin(), value.end());
        block.push_back(L'\0');
    }
    // 空环境同样必须提供两个连续终止字符。
    if(block.empty()) {
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

// validateOptions 在创建任何管道或进程前完成所有确定性配置校验。
void validateOptions(const ProcessOptions& options) {
    const auto contains_forbidden_control = [](const std::string& value) {
        return value.find('\0') != std::string::npos || value.find('\r') != std::string::npos ||
               value.find('\n') != std::string::npos;
    };
    std::error_code error;
    if(options.executable.empty() || !options.executable.is_absolute()) {
        throw std::invalid_argument("子进程可执行文件必须是绝对路径");
    }
    const std::wstring executable_text = options.executable.native();
    if(executable_text.find(L'\0') != std::wstring::npos || executable_text.find(L'\r') != std::wstring::npos ||
       executable_text.find(L'\n') != std::wstring::npos) {
        throw std::invalid_argument("子进程可执行文件路径包含非法控制字符");
    }
    if(!std::filesystem::is_regular_file(options.executable, error) || error) {
        throw std::invalid_argument("子进程可执行文件不存在或不是普通文件");
    }

    std::wstring extension = options.executable.extension().native();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    if(extension == L".cmd" || extension == L".bat") {
        throw std::invalid_argument("stdio MCP 不直接执行 .cmd 或 .bat 文件");
    }

    if(!options.working_directory.empty()) {
        const std::wstring working_directory_text = options.working_directory.native();
        if(working_directory_text.find(L'\0') != std::wstring::npos ||
           working_directory_text.find(L'\r') != std::wstring::npos ||
           working_directory_text.find(L'\n') != std::wstring::npos) {
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

// createAnonymousPipe 创建一对仅由调用方决定继承方向的匿名管道句柄。
std::pair<UniqueHandle, UniqueHandle> createAnonymousPipe(const std::string& action) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if(!CreatePipe(&read_handle, &write_handle, &attributes, 0)) {
        throw windowsError(action);
    }
    return {UniqueHandle(read_handle), UniqueHandle(write_handle)};
}

// disableInheritance 确保父进程持有的管道端不会被子进程或其后代误继承。
void disableInheritance(HANDLE handle, const std::string& action) {
    if(!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0)) {
        throw windowsError(action);
    }
}

// AttributeList 只允许三个标准流句柄进入新进程，避免继承宿主的其他敏感句柄。
class AttributeList final {
   public:
    explicit AttributeList(const std::array<HANDLE, 3>& handles) {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if(bytes == 0) {
            throw windowsError("计算子进程句柄白名单大小失败");
        }
        storage_.resize(bytes);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if(!InitializeProcThreadAttributeList(list_, 1, 0, &bytes)) {
            throw windowsError("初始化子进程句柄白名单失败");
        }
        if(!UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, const_cast<HANDLE*>(handles.data()),
                                      handles.size() * sizeof(HANDLE), nullptr, nullptr)) {
            const DWORD error = GetLastError();
            DeleteProcThreadAttributeList(list_);
            list_ = nullptr;
            throw windowsError("设置子进程句柄白名单失败", error);
        }
    }

    ~AttributeList() noexcept {
        if(list_ != nullptr) {
            DeleteProcThreadAttributeList(list_);
        }
    }

    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;

    LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept {
        return list_;
    }

   private:
    std::vector<std::byte> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

// IoChannel 让三个同步管道共享同一套互斥与取消规则。
enum class IoChannel {
    Stdin,
    Stdout,
    Stderr,
};

}  // namespace

// Impl 隐藏全部 Win32 类型，并记录每条同步 I/O 所在线程以支持 CancelSynchronousIo。
struct Process::Impl {
    UniqueHandle process;
    UniqueHandle job;
    UniqueHandle stdin_write;
    UniqueHandle stdout_read;
    UniqueHandle stderr_read;

    std::mutex state_mutex;
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
    UniqueHandle stdin_thread;
    UniqueHandle stdout_thread;
    UniqueHandle stderr_thread;

    // beginIo 同时注册唯一 Worker 和对应管道的稳定句柄快照。
    bool beginIo(IoChannel channel, HANDLE& pipe_handle) {
        std::lock_guard<std::mutex> lock(io_mutex);
        UniqueHandle* pipe = nullptr;
        UniqueHandle* worker = nullptr;
        bool* active = nullptr;
        switch(channel) {
            case IoChannel::Stdin:
                pipe = &stdin_write;
                worker = &stdin_thread;
                active = &stdin_active;
                break;
            case IoChannel::Stdout:
                pipe = &stdout_read;
                worker = &stdout_thread;
                active = &stdout_active;
                break;
            case IoChannel::Stderr:
                pipe = &stderr_read;
                worker = &stderr_thread;
                active = &stderr_active;
                break;
        }
        if(io_cancelled || (channel == IoChannel::Stdin && stdin_closed) || !*pipe) {
            return false;
        }
        if(*active) {
            throw std::logic_error("同一子进程标准流不能并发读写");
        }
        *worker = duplicateHandle(GetCurrentThread(), "注册子进程 I/O Worker 失败");
        *active = true;
        pipe_handle = pipe->get();
        return true;
    }

    // finishIo 在系统调用返回后撤销 Worker 注册，并唤醒 close 的资源收敛等待。
    void finishIo(IoChannel channel) noexcept {
        std::lock_guard<std::mutex> lock(io_mutex);
        switch(channel) {
            case IoChannel::Stdin:
                stdin_active = false;
                stdin_thread.reset();
                break;
            case IoChannel::Stdout:
                stdout_active = false;
                stdout_thread.reset();
                break;
            case IoChannel::Stderr:
                stderr_active = false;
                stderr_thread.reset();
                break;
        }
        io_finished.notify_all();
    }

    // readChannel 在一次同步调用内成对维护 Worker 注册，避免异常路径重复完成新操作。
    std::size_t readChannel(IoChannel channel, char* buffer, std::size_t capacity) {
        HANDLE pipe = nullptr;
        if(!beginIo(channel, pipe)) {
            return 0U;
        }
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(capacity, std::numeric_limits<DWORD>::max()));
        DWORD received = 0;
        const BOOL succeeded = ReadFile(pipe, buffer, requested, &received, nullptr);
        const DWORD error = succeeded ? ERROR_SUCCESS : GetLastError();
        finishIo(channel);
        if(succeeded) {
            return static_cast<std::size_t>(received);
        }
        if(error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF || error == ERROR_OPERATION_ABORTED ||
           error == ERROR_INVALID_HANDLE) {
            return 0U;
        }
        throw windowsError(channel == IoChannel::Stdout ? "读取子进程 stdout 失败" : "读取子进程 stderr 失败", error);
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

// start 使用 STARTUPINFOEXW 白名单继承标准流，并在恢复主线程前加入受控 Job Object。
Process Process::start(ProcessOptions options, std::chrono::steady_clock::time_point absolute_deadline) {
    // 截止时间由 MCPClient 在公开 connect 起点生成；平台层不能重新启动一段 startup_timeout。
    if(std::chrono::steady_clock::now() >= absolute_deadline) {
        throw std::runtime_error("启动 stdio MCP 子进程超时");
    }
    validateOptions(options);
    if(std::chrono::steady_clock::now() >= absolute_deadline) {
        throw std::runtime_error("启动 stdio MCP 子进程超时");
    }

    auto [child_stdin_read, parent_stdin_write] = createAnonymousPipe("创建子进程 stdin 管道失败");
    auto [parent_stdout_read, child_stdout_write] = createAnonymousPipe("创建子进程 stdout 管道失败");
    auto [parent_stderr_read, child_stderr_write] = createAnonymousPipe("创建子进程 stderr 管道失败");
    disableInheritance(parent_stdin_write.get(), "限制 stdin 父句柄继承失败");
    disableInheritance(parent_stdout_read.get(), "限制 stdout 父句柄继承失败");
    disableInheritance(parent_stderr_read.get(), "限制 stderr 父句柄继承失败");

    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if(!job) {
        throw windowsError("创建子进程 Job Object 失败");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
    job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &job_limits, sizeof(job_limits))) {
        throw windowsError("设置子进程树回收策略失败");
    }

    const std::array<HANDLE, 3> inherited_handles{child_stdin_read.get(), child_stdout_write.get(),
                                                  child_stderr_write.get()};
    AttributeList attribute_list(inherited_handles);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_stdin_read.get();
    startup.StartupInfo.hStdOutput = child_stdout_write.get();
    startup.StartupInfo.hStdError = child_stderr_write.get();
    startup.lpAttributeList = attribute_list.get();

    std::vector<wchar_t> command_line = buildCommandLine(options);
    std::vector<wchar_t> environment = buildEnvironmentBlock(options);
    const wchar_t* working_directory = options.working_directory.empty() ? nullptr : options.working_directory.c_str();
    PROCESS_INFORMATION process_information{};
    const DWORD creation_flags =
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | CREATE_NO_WINDOW;
    // CreateProcessW 不提供原生截止参数，因此在进入不可分割调用前再次裁决预算。
    if(std::chrono::steady_clock::now() >= absolute_deadline) {
        throw std::runtime_error("启动 stdio MCP 子进程超时");
    }
    if(!CreateProcessW(options.executable.c_str(), command_line.data(), nullptr, nullptr, TRUE, creation_flags,
                       environment.data(), working_directory, &startup.StartupInfo, &process_information)) {
        throw windowsError("启动 stdio MCP 子进程失败");
    }

    UniqueHandle process(process_information.hProcess);
    UniqueHandle primary_thread(process_information.hThread);
    // 子进程仍处于挂起状态，先加入 Job 后再处理 CreateProcessW 期间耗尽预算的情况。
    // 这样超时清理始终覆盖完整进程树，句柄 RAII 回收不会额外重启等待预算。
    const bool create_process_timed_out = std::chrono::steady_clock::now() >= absolute_deadline;
    if(!AssignProcessToJobObject(job.get(), process.get())) {
        const DWORD error = GetLastError();
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 5000);
        throw windowsError("把 stdio MCP 子进程加入 Job Object 失败", error);
    }
    if(create_process_timed_out || std::chrono::steady_clock::now() >= absolute_deadline) {
        TerminateJobObject(job.get(), 1);
        (void)WaitForSingleObject(process.get(), 0);
        throw std::runtime_error("启动 stdio MCP 子进程超时");
    }
    if(ResumeThread(primary_thread.get()) == static_cast<DWORD>(-1)) {
        const DWORD error = GetLastError();
        TerminateJobObject(job.get(), 1);
        WaitForSingleObject(process.get(), 5000);
        throw windowsError("恢复 stdio MCP 子进程主线程失败", error);
    }
    // 恢复调用自身也可能跨过截止点；此时立即终止 Job，绝不发布已超时的 Process。
    if(std::chrono::steady_clock::now() >= absolute_deadline) {
        TerminateJobObject(job.get(), 1);
        (void)WaitForSingleObject(process.get(), 0);
        throw std::runtime_error("启动 stdio MCP 子进程超时");
    }

    auto impl = std::make_unique<Impl>();
    impl->process = std::move(process);
    impl->job = std::move(job);
    impl->stdin_write = std::move(parent_stdin_write);
    impl->stdout_read = std::move(parent_stdout_read);
    impl->stderr_read = std::move(parent_stderr_read);
    // 返回前最后一次裁决可阻止已过期进程对象逃逸；Impl 析构会关闭 Job 并回收句柄。
    if(std::chrono::steady_clock::now() >= absolute_deadline) {
        TerminateJobObject(impl->job.get(), 1);
        (void)WaitForSingleObject(impl->process.get(), 0);
        throw std::runtime_error("启动 stdio MCP 子进程超时");
    }
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

    HANDLE pipe = nullptr;
    if(!impl_->beginIo(IoChannel::Stdin, pipe)) {
        throw std::runtime_error("子进程 stdin 已关闭或写入已取消");
    }
    std::size_t offset = 0;
    DWORD failure = ERROR_SUCCESS;
    bool made_no_progress = false;
    while(offset < data.size()) {
        const DWORD requested =
            static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if(!WriteFile(pipe, data.data() + offset, requested, &written, nullptr)) {
            failure = GetLastError();
            break;
        }
        if(written == 0) {
            made_no_progress = true;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    impl_->finishIo(IoChannel::Stdin);
    if(made_no_progress) {
        throw std::runtime_error("写入子进程 stdin 时未产生进度");
    }
    if(failure == ERROR_BROKEN_PIPE || failure == ERROR_NO_DATA) {
        throw std::runtime_error("子进程已关闭 stdin，无法继续写入");
    }
    if(failure == ERROR_OPERATION_ABORTED || failure == ERROR_INVALID_HANDLE) {
        throw std::runtime_error("子进程 stdin 写入已取消");
    }
    if(failure != ERROR_SUCCESS) {
        throw windowsError("写入子进程 stdin 失败", failure);
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
    if(impl_->stdin_thread) {
        CancelSynchronousIo(impl_->stdin_thread.get());
    }
    impl_->stdin_write.reset();
    impl_->io_finished.wait(lock, [this] { return !impl_->stdin_active; });
}

bool Process::waitForExit(std::chrono::milliseconds timeout, int& exit_code) {
    if(!impl_) {
        exit_code = 0;
        return true;
    }

    UniqueHandle process;
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if(impl_->exit_code_known) {
            exit_code = impl_->exit_code;
            return true;
        }
        process = duplicateHandle(impl_->process.get(), "复制子进程等待句柄失败");
    }

    const auto non_negative_timeout = std::max(timeout, std::chrono::milliseconds::zero());
    const auto maximum = std::chrono::milliseconds(std::numeric_limits<DWORD>::max() - 1ULL);
    const DWORD wait_ms = static_cast<DWORD>(std::min(non_negative_timeout, maximum).count());
    const DWORD wait_result = WaitForSingleObject(process.get(), wait_ms);
    if(wait_result == WAIT_TIMEOUT) {
        return false;
    }
    if(wait_result != WAIT_OBJECT_0) {
        throw windowsError("等待 stdio MCP 子进程退出失败");
    }

    DWORD native_exit_code = 0;
    if(!GetExitCodeProcess(process.get(), &native_exit_code)) {
        throw windowsError("读取 stdio MCP 子进程退出码失败");
    }
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        impl_->exit_code = static_cast<int>(native_exit_code);
        impl_->exit_code_known = true;
        exit_code = impl_->exit_code;
    }
    return true;
}

void Process::terminateTree() {
    if(!impl_) {
        return;
    }

    UniqueHandle job;
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if(impl_->closed || !impl_->job) {
            return;
        }
        job = duplicateHandle(impl_->job.get(), "复制子进程 Job Object 失败");
    }
    if(!TerminateJobObject(job.get(), 1)) {
        throw windowsError("终止 stdio MCP 子进程树失败");
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
    if(impl_->stdin_thread) {
        CancelSynchronousIo(impl_->stdin_thread.get());
    }
    if(impl_->stdout_thread) {
        CancelSynchronousIo(impl_->stdout_thread.get());
    }
    if(impl_->stderr_thread) {
        CancelSynchronousIo(impl_->stderr_thread.get());
    }
    // 先取消再关闭管道，覆盖“取消发生在同步调用建立前”的窄竞态窗口。
    impl_->stdin_write.reset();
    impl_->stdout_read.reset();
    impl_->stderr_read.reset();
    impl_->stdin_closed = true;
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
    }

    try {
        int ignored_exit_code = 0;
        if(!waitForExit(std::chrono::milliseconds::zero(), ignored_exit_code)) {
            terminateTree();
            waitForExit(std::chrono::seconds(5), ignored_exit_code);
        }
    } catch(...) {
        // close 是最终清理边界；Job Object 关闭仍会兜底终止受控进程树。
    }

    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        impl_->closed = true;
        impl_->process.reset();
        impl_->job.reset();
    }
    impl_.reset();
}

}  // namespace detail
}  // namespace aiSDK
