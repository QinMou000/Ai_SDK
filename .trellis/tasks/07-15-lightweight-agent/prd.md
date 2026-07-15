# 轻量 ReAct Agent 与工作区文件工具

## 目标

在 `master` 分支现有 DeepSeek 优先的 C++ AI SDK 基础上，设计并实现一个范围可控、可本地验证的轻量 Agent MVP，使其能够围绕用户指定的核心场景完成一次完整的“输入—决策—执行—结果”闭环。

## 已知信息

- 用户明确希望在当前 `master` 分支开发一个“小的 Agent”。
- 当前分支为 `master`，开始脑暴时工作树无已显示的未提交改动。
- 项目路线已经把 `SimpleAgent` 放在 MCP 与 Skill 之前，最小闭环定义为 `LLM → Tool → LLM`，直到模型不再返回 Tool Call。
- `include/agent/SimpleAgent.h` 已预留 `SimpleAgent::run(...)`、`AgentResult` 和消息历史，但尚无实现文件；`examples/06_simple_agent` 也只有占位入口。
- `examples/05_tool_call/main.cpp` 已显式实现一轮 `chat → executeToolCalls → 回填 assistant/tool 消息 → chat`，可直接作为 Agent Loop 的协议基线。
- `AIClient` 已统一持有 Provider 与 `ToolRegistry`，并公开普通/Trace 版本的 `chat(...)` 与 `executeToolCalls(...)`；Agent 不需要接触 Provider JSON 或 HTTP 层。
- `IModelProvider` 支持实例注入，现有测试可通过脚本化或伪 Provider 离线验证多轮请求，无需真实 API。
- Trace 采用调用方显式持有的 `TraceSession`；同一会话可以覆盖多次模型请求和工具批次，但各公开操作保持独立根步骤。
- `ToolRiskLevel` 已提供 `Low`、`Medium`、`High` 元数据，但 `ToolRegistry` 当前不会按风险等级审批或拦截；自动循环的 Agent 必须明确自己的执行策略。

## 临时假设

- “小”表示优先完成单一核心场景，不在首版建设通用 Agent 框架。
- 首版应复用 `AIClient`、`ChatRequest`、`Message`、`ToolRegistry`、`ToolExecutionResult` 与 `TraceSession`，避免引入新的重量级依赖。
- 所有功能与测试均在本地完成，不依赖 CI 或人工验证。
- 首版优先使用同步非流式循环；流式 Agent 会涉及 Tool Call 增量拼装与中断语义，暂不默认纳入。

## 待确认问题

- 无。

## 需求（持续演进）

- 基于仓库现有能力实现，不脱离 DeepSeek 优先的产品方向。
- 保持实现简洁，并为后续扩展保留清晰边界。
- 选择可复用的本地工具 Agent：实现同步多轮 Tool Call、模型自主结束、Trace 贯通、在线示例与离线测试。
- Agent 必须是 `AIClient` 之上的独立层，只调用其现有公开接口；本任务不得修改 `include/AIClient.h` 与 `src/AIClient.cpp`。
- 首版只编排已经注册到 `ToolRegistry` 的本地工具，不接入 MCP 或 Skill。
- 单个 Agent 实例首版只提供最简单的 ReAct 风格任务执行：模型决策、Tool Call 动作、工具结果观察、继续决策；调用方不指定循环轮次。
- 正常终止完全由模型响应决定：响应包含 Tool Call 时继续执行与观察，响应不再包含 Tool Call 时把文本作为最终答案并结束。
- Agent 内部固定最多发起 16 次模型请求。该上限不出现在 `run(...)` 参数中，只在模型持续请求工具的异常情况下熔断并返回失败。
- 单个工具执行失败不立即终止任务；失败结果作为 Observation 回填，允许模型改用其他工具或给出可解释的最终答案。
- Agent 只向模型展示并自动执行 `ToolRiskLevel::Low` 工具；本次五个工作区文件工具均标记为 `Low`。其他 `Medium` 与所有 `High` 工具继续拒绝。
- `SimpleAgent` 除调用方显式启用的工作区文件工具外，不主动注册其他业务工具；时间、加法等演示工具仍由示例通过 `AIClient::tools().registerTool(...)` 提供。
- 首版增加工作区文件工具：列目录、读取文本、新建文本、覆盖写入文本、精确替换文本内容。
- 所有文件工具必须绑定调用方显式提供的工作区根目录；不得访问工作区外路径。
- 采用构造时工作区授权：调用方提供有效工作区根目录后，Agent 才注册并自动执行该根目录内的五个文件工具。
- 工作区根目录是文件能力的唯一显式授权；未提供时不注册任何文件工具。其他 `Medium` 与所有 `High` 工具继续拒绝。
- 每次 `run(...)` 只维护本次任务所需的临时消息轨迹；调用结束后不提供跨任务会话状态，后续再独立建设会话管理与记忆系统。
- 不新增 Thought 文本解析器或公开推理内容字段；模型推理保持在现有 Provider 内，Agent 只编排结构化 Tool Call。

## 验收标准（持续演进）

- [x] 提供一个可重复运行的本地 Agent 示例。
- [x] 自动化测试覆盖正常流程、边界条件与错误恢复。
- [x] 使用项目既有构建与测试流程完成本地验证。
- [x] `git diff -- include/AIClient.h src/AIClient.cpp` 为空，证明 Agent 没有侵入核心客户端。
- [x] Agent 仅通过 `chat(...)`、`executeToolCalls(...)`、`tools()` 和 `startTrace(...)` 等现有公开入口完成闭环。
- [x] 离线测试证明单次 `run(...)` 可以完成至少两轮连续 Tool Call 后返回最终答案。
- [x] 连续两次调用 `run(...)` 时，第二次模型请求不会携带第一次任务的消息轨迹。
- [x] `run(...)` 的公开接口不要求调用方传入循环轮次；脚本化 Provider 可以在任意合理轮次返回最终文本并让 Agent 正常结束。
- [x] 脚本化 Provider 连续 16 次返回 Tool Call 时，Agent 在第 16 次模型请求后确定性失败，不会发起第 17 次请求。
- [x] 未知工具或工具处理函数失败时，失败结果会回填给模型，后续模型请求仍可返回最终答案。
- [x] 请求中的工具列表只包含已注册的 `Low` 工具；其他 `Medium` 与所有 `High` 工具不会暴露给模型。
- [x] 防御性测试证明模型臆造 `Medium` 或 `High` 工具调用时，对应处理函数不会执行，拒绝结果会作为 Observation 回填。
- [x] 文件工具覆盖正常读写、目标不存在、重复创建、替换零匹配/多匹配、绝对路径、`..` 越界和符号链接逃逸。
- [x] 文件写入测试只操作测试临时目录，不修改仓库真实源码或用户文件。
- [x] 未提供工作区根目录时，Agent 不向模型展示任何文件工具；提供后五个文件工具只允许访问授权根目录。

## 完成定义

- 新增或更新适当层级的单元测试、集成测试或冒烟测试。
- 格式化、编译、静态检查和测试全部通过。
- 行为变化同步到文档或示例说明。
- 明确风险、回滚方式与后续扩展点。

## 明确不在范围内

- 不建设通用多 Agent 编排、长期记忆或生产级操作系统沙箱。
- 不在脑暴阶段修改实现代码。
- 不修改 `AIClient` 的声明、实现与既有行为。
- 不在首版接入 MCP、Skill、Planner、并行工具调用或流式 Agent Loop。
- 不实现会话管理、跨任务上下文、短期/长期记忆、历史持久化与恢复。
- 不公开或保存模型私有思维链，不引入自定义 Thought/Action 文本协议。
- 不实现文件删除、移动、复制、权限修改、Shell 执行、二进制文件编辑或工作区外访问。

## 技术备注

- 任务目录：`.trellis/tasks/07-15-lightweight-agent/`。
- 已检查模式一：`examples/05_tool_call/main.cpp` 的显式两次模型调用，确认 assistant Tool Call 消息必须先回填，再按原 `tool_call_id` 追加 Tool 消息。
- 已检查模式二：`AIClient` 与 `ToolExecutor` 的职责边界，确认工具批次串行执行、单项失败不中断批次、Agent 才负责是否继续循环。
- 已检查模式三：`IModelProvider` 注入与现有 GTest 结构，确认 Agent 测试可离线脚本化多轮响应，并覆盖最终文本、连续 Tool Call 与步数耗尽。
- 已检查模式四：Trace 集成测试，确认 Agent 可复用单一 `TraceSession` 串起完整时间线，但不应改变既有父子层级语义。
- 已检查安全边界：`include/tool/Tool.h` 明确高风险工具应由上层应用决定是否执行，`ToolRegistry::execute(...)` 则会直接调用任意已注册工具，因此 Agent 不能假设注册表已经完成审批。
- 输入协议：`run(std::string)` 追加用户消息且不接收循环轮次；模型输入为 `ChatRequest{messages, tools}`；工具输出通过 `ToolExecutionResult::toToolMessage()` 回填。
- 输出协议：`AgentResult` 返回 `success`、`final_answer` 与中文 `error_message`；内部请求次数不作为调用参数，Trace 通过显式 `TraceSession` 读取。
- 配置与环境：核心单元测试可通过注入 Provider 完全离线；在线示例沿用最近 `.env` 的 `DEEPSEEK_API_KEY`、`DEEPSEEK_BASE_URL`、`DEEPSEEK_MODEL`。
- 构建与测试：C++17、CMake、vcpkg、GTest；MSVC 使用 `/W4 /utf-8`，其他编译器使用 `-Wall -Wextra -Wpedantic`。
- 当前仓库没有任何 `Medium` 或 `High` 工具注册；现有示例与测试中的 `get_current_time`、加法、Trace 工具和 echo 工具都标记为 `Low`。
- 文件工具输入来自模型，属于不可信路径；必须先规范化并验证最终路径仍在授权根目录内，不能只过滤字面量 `../`。

## 工具注册边界

- `SimpleAgent` 默认不注册业务工具；构造选项含有效工作区根目录时，由独立文件工具组件向 `AIClient::tools()` 注册五个工作区工具。
- `06_simple_agent` 注册 `get_current_time`：只读取本机时间，不修改状态，风险等级为 `Low`。
- `06_simple_agent` 同时注册 `add_numbers`：只做内存中的数值加法，不访问文件、网络或外部系统，风险等级为 `Low`。
- 示例不注册 Shell、HTTP、MCP、Skill、删除文件或工作区外工具。

### 首版文件工具

- `list_directory`：列出授权目录下单层条目，`Low`。
- `read_text_file`：读取授权目录下 UTF-8 文本文件，`Low`。
- `create_text_file`：目标不存在时新建 UTF-8 文本文件，`Low`。
- `write_text_file`：覆盖已经存在的 UTF-8 文本文件，`Low`。
- `replace_text_in_file`：对现有文本执行精确且唯一的内容替换，`Low`。
- 所有工具拒绝绝对路径、空路径、NUL、越界路径、符号链接逃逸、`.git/`、`.env` 与常见私钥文件。
- 单个文件的读取、创建、覆盖与替换结果限制为 64 KiB；目录列表最多返回 256 个单层条目，超限时返回中文失败结果。

## 依赖与集成点

```text
用户输入
  → SimpleAgent（模型驱动循环、单任务临时消息轨迹、安全熔断）
    → AIClient::chat（统一模型入口，可选 TraceSession）
      → IModelProvider（可注入，便于离线测试）
    → AIClient::executeToolCalls（复用客户端 ToolRegistry）
      → ToolExecutor（串行执行并保留结果顺序）
    → ToolExecutionResult::toToolMessage（按 tool_call_id 回填）
  → AgentResult（最终答案或可诊断失败）
```

## 候选范围

### 方案 A：可复用的本地工具 Agent（推荐）

- 实现 `SimpleAgent` 的同步多轮 Tool Call 循环、模型自主结束与 Trace 贯通。
- 补齐 `06_simple_agent` 在线示例，并用注入 Provider 做离线自动化测试。
- 优点：正好填补仓库占位与路线图缺口，后续 MCP/Skill 只需把工具适配进现有注册表。
- 风险：必须防止模型持续返回 Tool Call 导致异常死循环、无限费用和无法收尾。

### 方案 B：只做示例级 Agent

- 仅在 `06_simple_agent` 中把 `05_tool_call` 改造成有限循环，不增加可复用运行时。
- 优点：改动最小、最快看到效果。
- 风险：循环、错误处理和 Trace 会滞留在示例层，后续仍要重新抽象。

### 方案 C：直接做 MCP Agent

- Agent 同时负责接入 MCP 工具并执行循环。
- 优点：演示能力更强。
- 风险：当前 `master` 尚无 MCP 模块，会把两个里程碑耦合成更大任务，不符合“小 Agent”目标。

## 决策（ADR-lite）

**背景**：仓库已经具备模型调用、Tool Call 解析、工具执行和 Trace，但这些公开操作仍由示例手工串联；用户希望新增一个小型 Agent，并明确不能修改 `AIClient`。

**决策**：采用方案 A，在 `agent/` 上层实现可复用 `SimpleAgent`。首版是无跨任务记忆的最小 ReAct 风格执行器，只依赖 `AIClient` 和既有工具执行接口，负责有限循环、消息回填与最终结果，不改变 SDK 核心客户端。

**影响**：Agent 与模型供应商、HTTP 和工具协议保持解耦，未来 MCP/Skill 可作为工具来源接入，会话管理与记忆也可在该执行器之外增加；正常结束由模型控制，内部 16 次模型请求上限只负责阻断异常循环。

### 风险等级决策

**背景**：风险等级已经存在于 `Tool` 元数据中，但底层注册表不拦截执行；自动循环比手工工具调用更容易产生未经确认的副作用。

**决策**：首版 Agent 只展示和执行 `Low` 工具，并对模型臆造的 `Medium`、`High` 调用做二次拦截。五个内置工作区文件工具均标记为 `Low`，但仅在构造时提供有效工作区根目录后注册。

**影响**：首版不需要逐次审批即可形成可写闭环；工作区根目录构成显式授权，未来仍可在 Agent 层增加逐次审批，而无需修改 `AIClient`。

### 工作区文件授权决策

**背景**：创建、覆盖和修改文件会改变本地状态，但用户要求将受限工作区内的三个写工具归类为 `Low`，并让首版 Agent 能完成真实文件任务。

**决策**：采用构造时显式工作区授权。调用方必须提供有效工作区根目录，五个内置文件工具才会注册；其中创建、覆盖和替换工具均为 `Low`。

**影响**：授权在单个 Agent 实例生命周期内生效，不要求每次写入暂停；调用方必须选择最小必要工作区，所有路径仍需经过规范化、敏感路径拒绝和根目录归属检查。未来可在不改动 ReAct Loop 或 `AIClient` 的前提下，扩展为子目录白名单、按工具审批或更细的权限策略。

## 技术方案

- `SimpleAgent` 只负责单任务 ReAct 循环、模型自主结束、16 次模型请求熔断、风险策略和消息回填。
- 普通 `run(input)` 与接收显式 `TraceSession&` 的重载保持现有 Trace 使用风格；Agent 不修改 Trace 层级协议。
- `AgentResult` 至少返回成功标记、最终答案与中文失败原因；不暴露私有思维链。
- 独立工作区文件工具组件负责注册工具、路径解析、UTF-8 文本 I/O、大小限制和错误转换，避免把文件细节塞进 Agent Loop；其注册入口与路径策略独立，后续可增加其他本地工具、MCP/Skill 适配器或审批策略。
- 构造选项承载系统提示词和可选工作区根目录；公开 API 不要求循环轮次。根目录缺失时不注册文件工具，提供后注册五个 `Low` 文件工具。
- `06_simple_agent` 显式注册 `get_current_time` 与 `add_numbers` 两个 `Low` 演示工具，并启用绑定示例工作区的五个文件工具。
- 单元测试通过可注入 Provider 脚本化模型响应；文件测试只使用系统临时目录；在线示例继续通过 `.env` 调用 DeepSeek。

## 实施计划

- 阶段一：补齐 Agent 公共结果、构造选项、ReAct 循环和离线循环测试。
- 阶段二：实现工作区文件工具、路径沙箱、风险授权及完整边界测试。
- 阶段三：补齐 `06_simple_agent`、CMake 集成、README 与 Trace 验证。

## 实施与验证记录

- 新增 `agent/SimpleAgent`：本地同步 ReAct 循环以模型是否返回 Tool Call 决定结束，不保存跨 `run(...)` 消息；内部 16 次请求熔断只处理异常循环。
- 新增独立 `agent/WorkspaceFileTools`：工作区根目录、路径规范化、敏感路径、UTF-8 与大小限制均封装在文件工具组件，避免文件系统策略耦合到 Agent 循环。
- 文件工具的五项能力均为 `Low`，但仅在调用方通过 `SimpleAgentOptions` 显式提供有效工作区根目录时注册；Agent 对 `Medium` 和 `High` 仍执行可见性筛选与执行前拦截。
- 新增离线 `tests/agent/ai_sdk_agent_test`：覆盖多轮循环、独立 run、工具失败与未知工具恢复、风险拦截、16 次熔断、显式 Trace、工作区工具注册、读写边界和目录链接逃逸。
- Windows 目录链接逃逸测试会先尝试标准目录符号链接，权限不足时自动创建无需该特权的目录联接点；本机已实际验证读取被拒绝且目录列举不暴露该条目。

### 本地验证

在已加载 `D:\software\VS\Community\VC\Auxiliary\Build\vcvars64.bat` 的本机环境执行：

```cmd
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug --output-on-failure
```

结果：配置、构建均成功；7 个 CTest 目标全部通过。`ai_sdk_agent_test` 包含 16 项离线用例，目录链接逃逸用例已单独实际通过。

### 风险、回滚与后续扩展

- 风险：首版文件写入不提供跨进程事务或版本备份；外部并发删改会转换为 ToolResult 失败，而不是保证原子更新。调用方应选择专用最小工作区并由版本控制承担回滚。
- 回滚：移除 `src/agent/`、`include/agent/`、`tests/agent/` 和 `examples/06_simple_agent/` 的集成条目即可恢复到原有显式 Tool Call 用法；`AIClient` 未发生改动。
- 后续：可在不改动 ReAct Loop 或 `AIClient` 的前提下，向工作区工具组件加入子目录白名单、原子写入、逐次审批，再把 MCP/Skill 适配成同一 `ToolRegistry` 来源；会话与记忆应作为独立上层状态组件引入。

## 研究引用

- [`research/react-mvp.md`](research/react-mvp.md) — ReAct、DeepSeek Tool Call 与仓库现有显式循环可以直接映射，不需要修改 `AIClient`。
- [`research/filesystem-tools.md`](research/filesystem-tools.md) — 文件工具必须绑定显式工作区根目录，并对路径规范化、越界与写入语义做防御。
