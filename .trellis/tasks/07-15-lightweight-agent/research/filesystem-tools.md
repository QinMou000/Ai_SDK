# 文件系统工具安全研究

## 研究目标

为首版 ReAct Agent 增加读取、新建、覆盖写入和定点修改文本文件的能力，同时避免模型生成的路径越过授权目录、读取敏感文件或破坏任意本机文件。

## 参考模式

### 路径穿越威胁

来源：[OWASP Path Traversal](https://owasp.org/www-community/attacks/Path_Traversal)、[MITRE CWE-22](https://cwe.mitre.org/data/definitions/22)

- `../`、绝对路径、盘符和不同目录分隔符都可能让不可信路径越过预期根目录。
- 只删除字面量 `../` 不可靠；路径必须先规范化，再验证最终位置仍属于授权根目录。
- 文件读取会带来机密性风险，文件创建和覆盖会带来完整性与可用性风险，因此不能默认访问整个操作系统文件系统。

### C++17 路径规范化

来源：[Microsoft C++ `<filesystem>` functions](https://learn.microsoft.com/en-us/cpp/standard-library/filesystem-functions?view=msvc-170)

- `std::filesystem::canonical` 会生成绝对路径、解析符号链接，并消除 `.` 与 `..` 路径分量。
- 已存在的读取和写入目标可使用规范化后的真实路径做根目录归属检查。
- 新建目标本身尚不存在，应先规范化其父目录，确认父目录在根目录内，再拼接并校验文件名。

### 当前仓库工具边界

来源：`include/tool/Tool.h`、`include/tool/ToolRegistry.h`、`src/tool/ToolRegistry.cpp`

- `ToolRiskLevel::Low` 表示只读或影响可忽略；`Medium` 表示可能修改局部状态；`High` 表示可能产生外部副作用。
- `ToolRegistry` 保存风险元数据但不会审批，Agent 必须在执行前自行实施策略。
- `ToolHandler` 已能把业务校验失败表示为 `ToolResult`，适合将越界、文件不存在、重复匹配等问题作为 Observation 回填模型。

## 建议的首版工具

| 工具 | 行为 | 风险等级 |
|---|---|---|
| `list_directory` | 列出授权根目录下单层目录项 | `Low` |
| `read_text_file` | 读取授权根目录下 UTF-8 文本文件 | `Low` |
| `create_text_file` | 仅在目标不存在时新建 UTF-8 文本文件 | `Low` |
| `write_text_file` | 仅覆盖已经存在的 UTF-8 文本文件 | `Low` |
| `replace_text_in_file` | 对已存在文本做精确、唯一匹配替换 | `Low` |

## 必要约束

- 调用方显式提供工作区根目录；工具不以进程当前目录或系统根目录作为隐式授权。
- 拒绝绝对路径、空路径、NUL 字符、越过根目录的 `..`、符号链接或目录联接逃逸。
- 拒绝访问 `.git/`、`.env` 和常见私钥文件；这只是最低保护，不能替代调用方选择最小工作区根目录。
- 首版仅处理 UTF-8 文本，拒绝目录冒充文件、二进制文件和超出大小限制的内容。
- `create_text_file` 不覆盖已有文件；`write_text_file` 不隐式创建文件；`replace_text_in_file` 在零匹配或多匹配时失败，避免改错位置。
- 首版不提供删除、移动、复制、权限修改、Shell 执行或工作区外访问。

## 当前项目授权方案

### 构造时显式提供工作区根目录

- 调用方提供规范化工作区根目录；根目录缺失时不注册任何文件工具。
- 五个工作区文件工具均标记为 `Low`，Agent 可在该根目录内自动执行；其他 `Medium`、`High` 工具仍拒绝。
- 优点：无需引入交互式审批系统，授权范围明确且可测试。
- 风险：一次授权覆盖本次 Agent 实例生命周期，调用方必须选择最小根目录。

## 结论

首版可以加入文件工具，但必须采用显式工作区根目录。当前项目将五个受限工作区文件工具统一标记为 `Low`；路径沙箱是主要安全边界。文件工具注册、路径策略和 Agent ReAct Loop 应保持分离，以便后续独立增加子目录白名单、审批回调、其他本地工具、MCP/Skill 适配器或更细权限策略。
