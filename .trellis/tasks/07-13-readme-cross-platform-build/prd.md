# 完善 README 跨平台编译说明

## 目标

完善 `README.md` 中的本地构建章节，使 Windows 和 Linux 用户能够理解每条命令的目的，并能够按仓库现有 CMake Presets 完成 Debug、Release 与测试流程。

## 已确认事实

- Windows 本机预设由 `CMakeUserPresets.json` 提供，包括 `local-windows-debug`、`local-windows-release` 和 `local-windows-bootstrap`。
- Linux 共享预设由 `CMakePresets.json` 提供，包括 `linux-debug`、`linux-release`，其中只有 `linux-debug` 提供测试预设。
- Windows 使用 MSVC 时需要先加载 `vcvars64.bat`；当前 README 记录的是本机 Visual Studio 安装路径。
- Windows 与 Linux 预设都依赖 `VCPKG_ROOT` 指向有效的 vcpkg 安装目录。
- `local-windows-bootstrap` 会关闭示例和测试，仅验证核心库与依赖配置；当前没有对应的 Linux bootstrap 预设。

## 需求

- 保留 Windows 的分步执行与一次性执行两种方式。
- 在所有 Windows 命令前使用 `REM` 注释，说明加载工具链、配置、编译和测试各步骤的用途。
- 补充 Windows Release 与快速验证命令的逐条注释。
- 新增 Linux 编译说明，解释 `VCPKG_ROOT` 环境变量的用途。
- 提供 Linux Debug 配置、编译、测试以及 Release 配置、编译命令。
- 在所有 Linux 命令前使用 `#` 注释说明用途。
- 明确 Linux 使用共享预设，而不是 Windows 专用的本地预设。
- 所有新增说明和注释使用简体中文。

## 验收标准

- [x] Windows 构建章节中的每条可执行命令都有紧邻的中文注释。
- [x] Linux 构建章节覆盖 vcpkg 环境变量、Debug、测试和 Release 流程。
- [x] README 中的预设名称与 `CMakePresets.json`、`CMakeUserPresets.json` 完全一致。
- [x] Markdown 代码块语言与对应终端一致，且代码块闭合正确。
- [x] 文档保持 UTF-8 无 BOM，并通过 `git diff --check`。

## 完成定义

- README 修改完成且没有引入新的构建脚本或预设。
- 对照现有 CMake 配置完成命令名称与参数检查。
- 执行文档级本地验证，确认格式、编码与引用一致性。

## 技术方案

直接修改 `README.md` 的“构建方式”章节：Windows `cmd` 代码块使用 `REM` 行注释；Linux `bash` 代码块使用 `#` 行注释。Linux 命令复用仓库共享的 `linux-debug`、`linux-release` 预设，不复制或新增本地 Linux 预设。

## 技术选型理由

- **为什么使用现有预设**：预设是仓库构建命令的唯一事实来源，可避免 README 与实际构建配置漂移。
- **优势**：改动范围小、命令可复制、Windows 与 Linux 说明保持一致。
- **劣势和风险**：当前环境是 Windows，无法在本机真实执行 Linux 编译；通过解析 CMake 预设、核对条件与命令名称进行补偿验证。

## 关键风险点

- **边界条件**：Linux 用户必须把 `VCPKG_ROOT` 设置为自己的实际安装目录。
- **兼容性**：不把 `local-windows-*` 预设误用于 Linux，也不虚构不存在的 Linux bootstrap 预设。
- **性能与安全**：本次仅修改文档，不改变构建性能、运行行为或密钥处理方式。

## 决策记录

- **背景**：用户需要逐条命令说明并补充 Linux 编译方式。
- **决策**：使用代码块内的原生注释语法解释命令，并严格复用现有预设。
- **影响**：README 更适合直接复制执行；Linux 部分受当前 Windows 环境限制，只能做配置级静态验证。
- **确认状态**：用户已于 2026-07-13 明确确认按本方案实施。

## 不在范围内

- 不新增或修改 CMake 预设。
- 不新增 Linux bootstrap 预设。
- 不修改 `docs/跨平台构建方案.md`。
- 不执行 CI 或远程 Linux 验证。

## 技术备注

- 已检查 `README.md`、`CMakePresets.json`、`CMakeUserPresets.json`、`docs/跨平台构建方案.md` 和根目录 `CMakeLists.txt`。
- 本地 Windows 验证命令延续仓库已验证的 `vcvars64.bat` 与 `local-windows-debug` 流程。

## 本地验证记录

- 通过 JSON 解析确认 README 引用的 9 个配置、编译和测试预设均真实存在。
- 通过命令扫描确认目标章节中的 19 条 Windows/Linux 命令均有紧邻的中文注释。
- `cmake --list-presets=all` 成功解析当前预设文件。
- README 的 32 行 Markdown 代码围栏成对闭合，2 个本地文档链接均有效。
- README 与 PRD 均为有效 UTF-8 且无 BOM，未发现模板占位符。
- `git diff --check` 通过；Git 仅提示现有换行策略可能在后续操作时将 LF 转为 CRLF。
- 当前主机是 Windows，且 `wsl.exe --status` 返回退出码 `50` 并提示先安装 WSL，本机没有可用 Linux 环境，无法真实运行 Linux 构建。
- 未使用 CI、远程流水线或人工验证；Linux 命令已通过预设 JSON 解析、平台条件、命令名称与既有跨平台方案核对完成补偿验证。后续首次配置本地 Linux 环境时，应完整执行 README 中的 Debug、测试和 Release 命令。

## 规格更新判断

- 本次只补充用户侧构建说明，没有新增或变更 CMake 命令、环境变量契约、公开 API 或跨层行为。
- `.trellis/spec/backend/quality-guidelines.md` 已记录 Windows/Linux 共享预设与文档检查要求，现有规格足以约束后续工作。
- 因此不修改 `.trellis/spec/`，避免把一次性文案细节重复沉淀为代码规格。
