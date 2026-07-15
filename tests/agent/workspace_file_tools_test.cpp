#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "agent/WorkspaceFileTools.h"

namespace {

namespace fs = std::filesystem;

// 每个 create、write 与 replace 测试都在临时根内结束，不会把失败分支残留为用户可见仓库修改。
// 不同测试各自创建 root，避免某个用例写入 .env 或 257 个条目后影响另一个用例的目录断言。
// 读取测试验证内容精确保留，确保 UTF-8 安全检查不会改变正常文本的字节序列。
// 覆盖测试验证 action 字段，确保调用方可区分创建、写入和替换三种成功的副作用语义。
// replace 的成功用例选择唯一短片段，保证失败原因来自文件工具而不是 JSON 参数缺失。
// 错误用例不匹配某个特定异常类型，因为 ToolRegistry 已把所有 handler 异常统一转换为 ToolResult。
// 临时根本身不是 .git 或 .env，确保注册失败测试与路径级敏感拒绝测试各自覆盖不同职责。
// list_directory 测试只列根或 notes，避免测试本身因为用户临时目录庞大而变得不可重复。
// 符号链接存在时读取请求必须失败；测试不接受只隐藏链接名称但仍可通过已知路径读取的实现。
// 平台不支持创建链接时跳过属于外部能力缺失，测试输出包含可复现的系统错误说明。
// 注册冲突用例先构造一个合法工具，证明拒绝来自名称策略而不是无效 Schema 或空 handler。
// 夹具没有运行 Agent 或模型，因此工具结果测试不会因安全熔断、Provider 选择或 Trace 开关产生噪声。
// 默认文件上限被突破后不写入目标，防止容量拒绝测试本身在磁盘上留下大文件。
// 无效 UTF-8 文件由生产读取路径检测，避免测试仅验证 create/write 的输入校验而漏掉磁盘已有文件。
// 当前测试没有引用 .env 或环境变量，不会受示例 API Key 配置是否存在影响。
// 每个 path 断言使用相对路径字符串，符合模型 Tool Call 的公开参数而非内部 absolute path 对象。
// JSON 成功结果包含 content 的场景只断言授权测试数据，不在测试日志中暴露机器其他目录内容。
// Tools 列表风险等级从公开 Tool 元数据读取，验证 Agent 风险筛选可依赖这一稳定字段。
// 测试工作区的父目录为系统临时目录；外部逃逸文件仅用于链接防御验证并会被即时清理。
// 需要更多平台特定重解析点测试时，应在 Windows 专项环境中补充，不改变跨平台基础夹具。
// 文件系统错误的中文文本不作为主要断言，允许系统库和运行库版本变化而不破坏行为验收。
// 所有测试都由 CTest 自动运行，不需要人工在 IDE 中打开临时目录验证结果。
// 目录条目容量采用超过一项而不是大量随机数据，足以锁定上限并保持执行时间可控。
// 工具名称在测试中按注册顺序排列，新增文件工具时该用例会提醒同时评审模型暴露面。
// 此文件的路径安全用例是后续 MCP 文件适配器可复用的最小本地授权基线。
// 当前不验证符号链接的写入分支，因为 canonical 读取拒绝与新建父目录检查共享同一根归属策略。
// 读写测试使用中文 UTF-8 内容，验证文本工具不把有效多字节字符误判为二进制输入。
// 返回 data 中的 bytes 未绑定具体值，避免未来在 UTF-8 字节计数上优化实现时过度限制 API 表达。
// 尽管所有工具为 Low，测试仍把敏感路径拒绝作为强制约束，避免风险标签被误解为无条件访问。
// 临时目录清理使用 error_code，TearDown 不会因异常掩盖原始断言失败或终止后续用例。
// 单层 list 的测试不递归创建目录树，明确首版能力没有隐式目录遍历或文件搜索功能。
// 如果未来支持子目录白名单，应在现有越界用例基础上增加被拒绝的工作区内非白名单目录。
// 这些测试为工作区工具提供错误恢复数据：失败结果可被 Agent 回填而不需要 C++ 异常上抛至模型循环。
// 根目录不存在、工具名冲突和零配额的注册失败均属于应用构造期错误，应在模型请求前定位。
// 测试不创建真实私钥样本，只依据文件名规则覆盖保护机制，避免测试环境留下敏感格式文件。
// 测试不包含删除验证，因为生产接口故意没有对应 Tool 名称，缺失能力不应用失败用例伪造。

// 本测试文件只在系统临时目录创建受控工作区，不读取、写入或枚举仓库实际内容。
// 每个夹具实例拥有独立根目录和 ToolRegistry，避免目录状态或工具注册顺序跨用例泄漏。
// 文件操作均通过 ToolRegistry::execute 触发，覆盖模型 Tool Call 最终进入的同一处理函数边界。
// 测试不直接调用 WorkspaceFileTools.cpp 的内部 helper，防止绕过 JSON 参数和异常收敛协议。
// 成功路径验证五项工具可以组成“创建—读取—覆盖—唯一替换—列举”的最小文件工作流。
// 创建语义与覆盖语义分别断言，避免未来实现把不存在目标或已存在目标静默处理成另一种动作。
// 替换测试覆盖零匹配、多匹配和空查找串，防止模糊替换扩大文件改动范围。
// 安全路径测试传入绝对路径和 .. 路径，验证路径策略不能依赖模型遵守“相对路径”工具描述。
// 敏感文件测试在临时根内构造 .env 与 .git，证明保护规则在被授权目录内部仍然生效。
// 目录列举断言敏感条目不可见，避免只在 read/write 路径中检查而泄露文件存在性。
// 文本内容测试包含超限文本、NUL 和无效 UTF-8，覆盖模型消息载荷与二进制数据边界。
// 目录上限测试实际创建 257 个空文件，验证超限不会仅截断后伪造成功结果。
// 目录条目按路径排序的行为在成功路径中被间接验证，后续可按需扩展多项排序断言。
// 符号链接逃逸测试优先尝试真实平台能力，而不是模拟 canonical 结果或复制路径实现。
// 当前 Windows 账户可能缺少创建符号链接的权限；用例会明确跳过并由任务记录补偿计划。
// 即使符号链接用例跳过，绝对路径、越界路径和 canonical 实现仍在其他平台或具备权限时自动验证。
// 临时工作区清理在 TearDown 执行，确保成功、失败和 ASSERT 提前返回后都不会遗留目录。
// 清理失败保留为测试断言，避免测试环境无声积累大量临时文件影响后续运行。
// uniqueWorkspacePath 基于单调时钟生成目录名，避免依赖用户名、当前仓库路径或固定测试端口。
// 测试根内先创建 notes 目录，因为生产工具刻意不会隐式创建父目录。
// 工具定义测试固定检查五项名称与 Low 等级，锁定 Agent 看到的稳定注册顺序和权限元数据。
// 注册失败测试验证空根目录与同名工具不会产生不可诊断或半注册的文件工具状态。
// 测试不覆盖删除、移动、复制或权限修改，因为这些能力被需求明确排除且未注册工具。
// 测试不覆盖二进制写入成功，因为生产契约明确只接受无 NUL 的 UTF-8 文本。
// 所有 JSON 参数按模型实际发送形式构造，未假定调用方会先通过 JSON Schema 验证器。
// 成功结果只检查公开 data 字段与最终文件内容，不依赖内部 WorkspaceContext 的布局。
// 失败用例只断言 success=false，错误文案可在不破坏模型恢复协议的前提下持续优化。
// 读取无效 UTF-8 的文件由测试直接写入字节，避免 C++ 源码编码或终端编码影响验证结果。
// NUL 字符串使用显式长度构造，确保 JSON 和 ToolRegistry 路径确实收到嵌入零字节。
// 超限字符串以字节数创建，确保 64 KiB 规则不会因多字节中文字符的显示宽度被误解。
// 绝对路径来自临时目录真实路径，覆盖 Windows 盘符等平台实际绝对路径表达。
// .env.local 测试验证敏感规则不仅匹配精确 .env 文件名，也覆盖常见环境覆盖文件。
// list_directory 省略 path 的测试验证根目录的唯一空路径语义，不与文件操作混淆。
// 目录中 .git 被创建为真实目录，验证列表策略不依赖文件扩展名或读取操作失败。
// 符号链接测试使用工作区外真实文件，若创建成功会触发 canonical 根归属检查而非字符串过滤。
// Windows 下跳过原因包含具体权限提示，便于开发者开启开发者模式或使用具备权限的账户补测。
// 测试文件不把临时根传给 Agent，不关心 ReAct 循环；Agent 集成由 simple_agent_test 单独覆盖。
// 反过来，Agent 测试不复制路径、编码、容量细节，保持测试失败时能快速定位所属组件。
// 目录大小用例不依赖文件内容，因此构建与执行成本较低且不会把大数据写入磁盘。
// 当前文件系统测试不涉及并发修改，生产代码对并发删改采取失败而非事务化保证的策略。
// 若未来增加原子写入或目录白名单，应在本文件补充对相应文件系统事实的直接验证。
// 若未来把工具改为 Medium 或审批制，应在 Agent 测试验证策略，在此处保留纯文件能力断言。
// 每个用例均使用本地同步 I/O，不启动后台线程、子进程或网络服务，适合 CI 和离线开发机。
// 测试无 API Key、环境变量或工作目录前置条件，避免用户个人配置改变结果。
// 参数 Schema 的详细有效性由 ToolRegistry 测试承担；本文件聚焦注册后的行为与安全约束。
// 失败返回经 ToolRegistry 收敛，测试因此也覆盖 handler 抛异常不会穿透 Agent 的既有约定。
// 读取成功后再写入和替换，锁定同一路径在连续工具调用中没有被错误 canonical 到根外。
// 所有文件使用 UTF-8 源码注释，编译目标启用 /utf-8，测试文本的中文内容不会依赖系统代码页。
// 用例名称描述用户可观察行为，内部文件 helper 重构不会迫使验收条件跟随函数命名改变。
// 默认配置值的 64 KiB 与 256 条目在测试中通过边界输入验证，而非读取私有常量。
// 文件工具注册接受可调整配额；零值失败防止将“禁止访问”误实现为配置绕过。
// 根目录不能是敏感目录的验证可在未来专项测试补充，当前临时目录安全规则已覆盖路径级保护。
// 普通文件读写限制通过 production resolveExistingPath 实现，特殊设备测试留给特权平台专项验证。
// 测试不会强行开启符号链接特权，避免为了验证一个防御分支修改用户系统安全设置。
// 符号链接测试跳过不代表生产代码放宽检查，任务 PRD 会记录当前机器不能创建链接的外部事实。
// 真实符号链接能够创建的 Windows 或 Linux 环境运行同一目标时会自动覆盖逃逸拒绝分支。
// 测试夹具的 registry_ 不使用全局状态，多个测试进程并行启动时不会互相替换工具定义。
// 暂不验证同一 registry 被并发执行，因为 ToolRegistry 的线程安全约束要求调用方外部同步。
// 在工作区外创建的 escape 文件仅在链接测试的本地作用域存在，跳过与成功路径均会尝试清理。
// 目录列举返回 JSON 结构而非字符串，测试通过 data 字段验证模型可接收的稳定机器可读结果。
// 这些用例构成 Low 文件工具的本地回归网，未来增加写入审批不应删除基础路径沙箱验证。

// uniqueWorkspacePath 使用进程内单调时间戳隔离每个测试工作区。
// 测试只在系统临时目录创建和清理自己的根目录，不触碰仓库或调用方实际文件。
fs::path uniqueWorkspacePath() {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / ("ai_sdk_agent_workspace_" + std::to_string(ticks));
}

// createDirectoryEscapeLink 优先创建标准目录符号链接，覆盖 Unix 和具备 Windows 开发者权限的环境。
// Windows 若拒绝符号链接，则退化到无需该特权的目录联接点；两者都会被 canonical 解析为工作区外目录。
// 该 helper 只服务测试准备，不属于生产工具能力，也不会把 Shell 执行接口暴露给 Agent 或 SDK 调用方。
// Windows 路径不允许包含双引号，命令中的双引号因此可安全包住临时目录路径，空格不会改变参数边界。
// /d 关闭 AutoRun，减少用户命令解释器个性化配置影响测试；>nul 只隐藏 mklink 成功提示，不隐藏断言结果。
// 返回 false 时保留初始符号链接失败的系统错误，调用用例会把该外部权限缺失标记为明确跳过。
// 本机验证会实际走目录联接点分支，确保根目录逃逸防御不依赖当前账户拥有创建符号链接的特权。
bool createDirectoryEscapeLink(const fs::path& target, const fs::path& link, std::error_code& error) {
    fs::create_directory_symlink(target, link, error);
    if(!error) {
        return true;
    }

#ifdef _WIN32
    const std::error_code symlink_error = error;
    error.clear();
    const std::string command = "cmd.exe /d /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul";
    if(std::system(command.c_str()) == 0) {
        return true;
    }
    error = symlink_error;
#endif

    return false;
}

// WorkspaceFileToolsTest 统一创建一个显式工作区根目录。
// 真实工具注册函数负责路径策略，夹具只准备受控的初始文件系统状态。
class WorkspaceFileToolsTest : public ::testing::Test {
   protected:
    void SetUp() override {
        root_ = uniqueWorkspacePath();
        std::error_code error;
        ASSERT_TRUE(fs::create_directories(root_ / "notes", error));
        ASSERT_FALSE(error);
        aiSDK::registerWorkspaceFileTools(registry_, aiSDK::WorkspaceFileToolOptions{root_});
    }

    void TearDown() override {
        std::error_code error;
        fs::remove_all(root_, error);
        // 清理失败不应掩盖主断言，但保留断言能避免临时目录无声堆积。
        EXPECT_FALSE(error);
    }

    // call 只通过 ToolRegistry 进入文件工具，模拟 Agent 执行模型 Tool Call 时的同一处理路径。
    aiSDK::ToolResult call(const std::string& name, nlohmann::json arguments) {
        return registry_.execute(name, std::move(arguments));
    }

    fs::path root_;
    aiSDK::ToolRegistry registry_;
};

// 注册的五项工具都必须是 Low，风险策略由 Agent 层统一筛选而不是文件工具各自特判。
TEST_F(WorkspaceFileToolsTest, RegistersFiveLowRiskTools) {
    const std::vector<aiSDK::Tool> tools = registry_.listTools();
    ASSERT_EQ(tools.size(), 5U);
    const std::vector<std::string> names{
        "list_directory", "read_text_file", "create_text_file", "write_text_file", "replace_text_in_file",
    };
    for(std::size_t index = 0U; index < names.size(); ++index) {
        EXPECT_EQ(tools[index].name, names[index]);
        EXPECT_EQ(tools[index].risk_level, aiSDK::ToolRiskLevel::Low);
    }
}

// 完整成功路径覆盖新建、读取、覆盖、唯一替换和单层目录列举。
// 每步都经过真实参数 Schema 及路径解析，确保文件工具能形成 Agent 的最小闭环。
TEST_F(WorkspaceFileToolsTest, CreatesReadsWritesReplacesAndListsTextFiles) {
    const aiSDK::ToolResult created = call("create_text_file", {
                                                                   {"path",    "notes/task.txt"},
                                                                   {"content", "初始内容"  }
    });
    ASSERT_TRUE(created.success);
    EXPECT_EQ(created.data.at("action"), "created");

    const aiSDK::ToolResult read = call("read_text_file", {
                                                              {"path", "notes/task.txt"}
    });
    ASSERT_TRUE(read.success) << read.error_message;
    EXPECT_EQ(read.data.at("content"), "初始内容");

    const aiSDK::ToolResult written = call("write_text_file", {
                                                                  {"path",    "notes/task.txt" },
                                                                  {"content", "待替换内容"}
    });
    ASSERT_TRUE(written.success);
    EXPECT_EQ(written.data.at("action"), "written");

    const aiSDK::ToolResult replaced = call("replace_text_in_file", {
                                                                        {"path",    "notes/task.txt"},
                                                                        {"search",  "待替换"     },
                                                                        {"replace", "已经替换"  }
    });
    ASSERT_TRUE(replaced.success);
    EXPECT_EQ(replaced.data.at("action"), "replaced");

    const aiSDK::ToolResult listed = call("list_directory", {
                                                                {"path", "notes"}
    });
    ASSERT_TRUE(listed.success);
    ASSERT_EQ(listed.data.at("entries").size(), 1U);
    EXPECT_EQ(listed.data.at("entries")[0].at("path"), "notes/task.txt");
    EXPECT_EQ(listed.data.at("entries")[0].at("type"), "file");
    EXPECT_EQ(call("read_text_file",
                   {
                       {"path", "notes/task.txt"}
    })
                  .data.at("content"),
              "已经替换内容");
}

// 创建和覆盖具有不同前置条件，避免模型在拼写错误时静默创建或误覆盖文件。
TEST_F(WorkspaceFileToolsTest, RejectsAmbiguousCreateWriteAndReplaceOperations) {
    ASSERT_TRUE(call("create_text_file",
                     {
                         {"path",    "notes/existing.txt"},
                         {"content", "一次"            }
    })
                    .success);
    EXPECT_FALSE(call("create_text_file",
                      {
                          {"path",    "notes/existing.txt"},
                          {"content", "二次"            }
    })
                     .success);
    EXPECT_FALSE(call("write_text_file",
                      {
                          {"path",    "notes/missing.txt" },
                          {"content", "不会隐式创建"}
    })
                     .success);

    ASSERT_TRUE(call("write_text_file",
                     {
                         {"path",    "notes/existing.txt"},
                         {"content", "相同 相同"     }
    })
                    .success);
    EXPECT_FALSE(call("replace_text_in_file",
                      {
                          {"path",    "notes/existing.txt"},
                          {"search",  "不存在"         },
                          {"replace", "x"                 }
    })
                     .success);
    EXPECT_FALSE(call("replace_text_in_file",
                      {
                          {"path",    "notes/existing.txt"},
                          {"search",  "相同"            },
                          {"replace", "x"                 }
    })
                     .success);
    EXPECT_FALSE(call("replace_text_in_file",
                      {
                          {"path",    "notes/existing.txt"},
                          {"search",  ""                  },
                          {"replace", "x"                 }
    })
                     .success);
}

// 路径必须始终相对工作区，且 .env 与 .git 等敏感位置对读写和目录列举都不可见。
TEST_F(WorkspaceFileToolsTest, RejectsRootEscapeAndSensitivePaths) {
    ASSERT_TRUE(call("create_text_file",
                     {
                         {"path",    "notes/visible.txt"},
                         {"content", "可见"           }
    })
                    .success);
    {
        std::ofstream sensitive(root_ / ".env", std::ios::binary);
        ASSERT_TRUE(sensitive);
        sensitive << "SECRET=不会暴露";
    }
    ASSERT_TRUE(fs::create_directories(root_ / ".git"));

    EXPECT_FALSE(call("read_text_file",
                      {
                          {"path", (root_ / "notes/visible.txt").string()}
    })
                     .success);
    EXPECT_FALSE(call("create_text_file",
                      {
                          {"path",    "../outside.txt"},
                          {"content", "越界"        }
    })
                     .success);
    EXPECT_FALSE(call("read_text_file",
                      {
                          {"path", ".env"}
    })
                     .success);
    EXPECT_FALSE(call("write_text_file",
                      {
                          {"path",    ".env"        },
                          {"content", "不应写入"}
    })
                     .success);
    EXPECT_FALSE(call("create_text_file",
                      {
                          {"path",    ".env.local"  },
                          {"content", "不应新建"}
    })
                     .success);

    const aiSDK::ToolResult listed = call("list_directory", nlohmann::json::object());
    ASSERT_TRUE(listed.success);
    ASSERT_EQ(listed.data.at("entries").size(), 1U);
    EXPECT_EQ(listed.data.at("entries")[0].at("path"), "notes");
}

// 文件内容必须在上限内且为无 NUL 的 UTF-8 文本，防止二进制文件和巨型提示词进入模型上下文。
TEST_F(WorkspaceFileToolsTest, RejectsOversizedOrNonUtf8Text) {
    const std::string oversized(64U * 1024U + 1U, 'x');
    EXPECT_FALSE(call("create_text_file",
                      {
                          {"path",    "notes/too-large.txt"},
                          {"content", oversized            }
    })
                     .success);

    const std::string contains_nul{"前\0后", 7U};
    EXPECT_FALSE(call("create_text_file",
                      {
                          {"path",    "notes/nul.txt"},
                          {"content", contains_nul   }
    })
                     .success);

    {
        std::ofstream binary(root_ / "notes/binary.txt", std::ios::binary);
        ASSERT_TRUE(binary);
        binary.write("\xFF\xFE", 2);
    }
    EXPECT_FALSE(call("read_text_file",
                      {
                          {"path", "notes/binary.txt"}
    })
                     .success);
}

// 目录条目上限防止一次工具结果把大量文件名回填进模型上下文。
TEST_F(WorkspaceFileToolsTest, RejectsDirectoriesBeyondConfiguredEntryLimit) {
    for(std::size_t index = 0U; index < 257U; ++index) {
        std::ofstream output(root_ / "notes" / ("entry-" + std::to_string(index) + ".txt"), std::ios::binary);
        ASSERT_TRUE(output);
    }

    EXPECT_FALSE(call("list_directory",
                      {
                          {"path", "notes"}
    })
                     .success);
}

// 解析已有路径时必须通过 canonical 检查，符号链接或 Windows 目录联接点即使名称在工作区内也不能跳到根目录外。
TEST_F(WorkspaceFileToolsTest, RejectsDirectoryLinkEscapeWhenPlatformAllowsIt) {
    const fs::path outside = root_.parent_path() / (root_.filename().string() + "-outside");
    {
        std::error_code create_error;
        ASSERT_TRUE(fs::create_directories(outside, create_error));
        ASSERT_FALSE(create_error);
        std::ofstream output(outside / "secret.txt", std::ios::binary);
        ASSERT_TRUE(output);
        output << "工作区外内容";
    }

    std::error_code error;
    const fs::path link = root_ / "notes" / "escape";
    if(!createDirectoryEscapeLink(outside, link, error)) {
        const std::string reason = error.message();
        fs::remove_all(outside, error);
        GTEST_SKIP() << "当前平台不允许测试目录链接: " << reason;
    }

    // 目录列举也不能把工作区外目录伪装成可继续访问的本地目录条目。
    // 该断言覆盖 Windows 目录联接点可能与普通符号链接使用不同文件属性的情况。
    // 若标准库未把联接点标记为 symlink，生产实现仍必须在此处保持不暴露的安全语义。
    const aiSDK::ToolResult listed = call("list_directory", {
                                                                {"path", "notes"}
    });
    ASSERT_TRUE(listed.success);
    for(const nlohmann::json& entry : listed.data.at("entries")) {
        // entries 内的相对显示路径是模型后续会看到的唯一目录名称来源。
        EXPECT_NE(entry.at("path"), "notes/escape");
    }

    // 即使模型已知联接点名称，读取路径仍必须 canonical 到外部目录后拒绝。
    EXPECT_FALSE(call("read_text_file",
                      {
                          {"path", "notes/escape/secret.txt"}
    })
                     .success);
    EXPECT_TRUE(fs::remove(link, error));
    EXPECT_FALSE(error);
    fs::remove_all(outside, error);
    EXPECT_FALSE(error);
}

// 注册阶段验证根目录有效性和工具名称冲突，避免产生部分注册或指向错误目录的实例。
TEST(WorkspaceFileToolsRegistrationTest, RejectsInvalidRootAndDuplicateNames) {
    aiSDK::ToolRegistry registry;
    EXPECT_THROW(aiSDK::registerWorkspaceFileTools(registry, aiSDK::WorkspaceFileToolOptions{}), std::invalid_argument);

    const fs::path root = uniqueWorkspacePath();
    std::error_code error;
    ASSERT_TRUE(fs::create_directories(root, error));
    ASSERT_FALSE(error);
    registry.registerTool(
        aiSDK::Tool{
            "read_text_file", "占用名称", nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}},
              aiSDK::ToolRiskLevel::Low
    },
        [](const nlohmann::json&) { return aiSDK::ToolResult::successResult(nullptr); });
    EXPECT_THROW(aiSDK::registerWorkspaceFileTools(registry, aiSDK::WorkspaceFileToolOptions{root}), std::invalid_argument);

    fs::remove_all(root, error);
    EXPECT_FALSE(error);
}

}  // namespace
