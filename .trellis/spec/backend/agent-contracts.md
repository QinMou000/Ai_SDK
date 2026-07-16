# Agent 与工作区文件工具契约

## 场景一：无状态 `SimpleAgent` 的同步与回调流式 ReAct

### 1. 作用范围 / 触发条件

- 触发：新增或修改 `include/agent/SimpleAgent.h`、`src/agent/SimpleAgent.cpp`，或调整 Agent 对 `AIClient`、工具和 Trace 的组合方式。
- 目标：在不修改 `AIClient` 的前提下提供同步与回调流式的最小 ReAct 循环，并保持 SDK 基础客户端不承担消息历史或继续决策。

### 2. 关键签名

- `SimpleAgent::SimpleAgent(AIClient& client, SimpleAgentOptions options = {})`
- `AgentResult SimpleAgent::run(const std::string& input)`
- `AgentResult SimpleAgent::run(const std::string& input, TraceSession& trace_session)`
- `AgentResult SimpleAgent::runStream(const std::string& input, AgentStreamCallback callback)`
- `AgentResult SimpleAgent::runStream(const std::string& input, AgentStreamCallback callback, TraceSession& trace_session)`
- `struct AgentResult { bool success; std::string final_answer; std::string error_message; }`
- `enum class AgentStreamEventType { TextDelta, ToolCallReady, ToolExecutionFinished }`
- `struct AgentStreamEvent { AgentStreamEventType type; std::string delta; std::string tool_call_id; std::string tool_name; bool success; }`
- `struct SimpleAgentOptions { std::string system_prompt; std::optional<WorkspaceFileToolOptions> workspace_file_tools; }`

### 3. 契约

- Agent 只能经 `AIClient::chat(...)`、`AIClient::streamChat(...)`、`AIClient::executeToolCalls(...)` 和 `AIClient::tools()` 组合既有能力；不得修改 `AIClient` 的声明、实现或 Provider/HTTP 协议。
- 每次 `run(...)` 或 `runStream(...)` 都新建局部消息数组：非空系统提示词（若有）加当前用户输入。返回后不得保留跨任务消息、会话或记忆。
- 模型响应有 `tool_calls` 时，必须先回填该 assistant 消息，再按原顺序回填每个 `ToolExecutionResult::toToolMessage()`；无 `tool_calls` 时返回 `success == true` 与 `response.content`。
- `runStream(...)` 的 callback 必填且仅在当前同步调用期间使用。文本分片必须立即以 `TextDelta` 交付；回调抛出的异常按 Agent 流式运行失败收敛。
- 流式 Tool Call 只能消费 `StreamEvent::tool_call_deltas` 中的结构化 `index`、可选 `id`/`name` 与 `arguments_delta`，不得在 Agent 中解析 Provider 原始 JSON。每轮流结束后按 `index` 聚合、校验 ID、名称和 JSON 对象参数，再执行工具。
- 流式工具参数或结果正文不得默认经 callback 泄露。参数校验通过后发出 `ToolCallReady`；每个串行执行结果回填 ToolMessage 后发出 `ToolExecutionFinished`，并以 `success` 表示结果状态。
- 每轮只把 `ToolRiskLevel::Low` 工具放入 `ChatRequest::tools`。模型直接请求已注册的 `Medium` 或 `High` 工具时，不得执行 handler，必须回填失败 Observation。
- 未知工具和工具 handler 失败继续委托既有 `ToolExecutor` 收敛为失败结果，Agent 不得因此提前终止循环。
- 公开 `run` 不接收循环轮次。内部最多请求模型 1024 次；第 1024 次仍返回 Tool Call 时，返回失败 `AgentResult`，不得发起第 1025 次请求。
- 显式 Trace 重载只复用调用方传入的 `TraceSession`，使模型请求和工具批次写入同一会话；Agent 不创建隐式 Trace 会话或自定义步骤类型。

### 4. 校验与错误矩阵

| 条件 | 行为 |
| --- | --- |
| `input` 为空 | 直接返回 `success == false`，`error_message == "Agent 输入不能为空"`，不调用 Provider |
| Provider 或未收敛的运行期代码抛 `std::exception` | 返回 `success == false`，错误以 `Agent 执行失败: ` 为上下文 |
| `runStream(...)` 的 callback 为空 | 直接返回 `success == false`，`error_message == "Agent 流式回调不能为空"`，不调用 Provider |
| 流中出现 `Error`、Tool Call 缺 ID/名称或参数不是 JSON 对象 | 返回 `success == false`，错误以 `Agent 流式执行失败: ` 为上下文；不得执行该轮工具 |
| 响应不含 Tool Call | 返回 `success == true`，最终答案为响应文本 |
| Tool handler 失败或工具未知 | 回填失败 ToolMessage，允许下一轮模型生成最终文本 |
| 模型调用已注册 `Medium`/`High` 工具 | 不执行 handler，回填“拒绝执行非低风险工具”失败结果 |
| 连续 1024 次响应均含 Tool Call | 返回 `success == false` 与内部安全熔断错误 |

### 5. Good / Base / Bad

- Good：上层创建 `SimpleAgent`，按需调用 `run(input, trace)` 或 `runStream(input, callback, trace)`；Agent 复用 `AIClient` 的消息、工具结果和 Trace 公开协议。
- Base：无文件根目录、无 Low 工具时模型仍可直接返回文本；Agent 不强制 Tool Call。
- Bad：在 `AIClient::chat()`、`streamChat()` 或 `executeToolCalls()` 内自动追加消息并无限循环；在 Agent 中解析 Provider JSON；或让 Agent 自动执行 Medium/High 工具。

### 6. 必要测试

- `tests/agent/simple_agent_test.cpp` 必须覆盖：两轮以上 Tool Call、两次 `run` 历史隔离、未知工具和 handler 失败恢复、风险过滤和直接臆造拦截、1024 次熔断、空输入、显式 Trace。
- 同一文件必须使用脚本化流式 Provider 覆盖：无 Tool Call 文本分片顺序与完整答案、交错工具分片按 index 聚合、参数 JSON 失败和流错误均无工具副作用、`ToolCallReady`/`ToolExecutionFinished` 顺序、流式风险拦截、1024 次流式熔断、空 callback 与流式 Trace。
- 通过可注入 `IModelProvider` 脚本化响应，不依赖 API Key、网络或真实 Provider。
- 修改工具策略时，同时断言 `ChatRequest::tools` 的可见集合与 handler 实际执行次数。

### 7. 错误写法 vs 正确写法

#### 错误写法

```cpp
// 错误：把 Agent 循环塞进基础 SDK 门面，并把所有已注册工具交给模型。
request.tools = client.tools().listTools();
while(true) {
    const ChatResponse response = client.chat(request);
    client.executeToolCalls(response.tool_calls);
}
```

#### 正确写法

```cpp
SimpleAgent agent(client, options);
TraceSession trace = client.startTrace();
const AgentResult result = agent.runStream(
    "整理工作区说明",
    [](const AgentStreamEvent& event) {
        if(event.type == AgentStreamEventType::TextDelta) {
            render(event.delta);
        }
    },
    trace);
```

## 场景二：显式授权的工作区文本工具

### 1. 作用范围 / 触发条件

- 触发：修改 `WorkspaceFileToolOptions`、`registerWorkspaceFileTools(...)`、工作区文件工具 Schema 或路径/文本安全策略。
- 目标：仅在调用方显式授权的根目录内，为 Agent 提供小范围 UTF-8 文本操作；文件系统细节不得扩散到 ReAct Loop。

### 2. 关键签名

- `struct WorkspaceFileToolOptions { std::filesystem::path root; std::size_t max_file_bytes; std::size_t max_directory_entries; std::size_t max_search_results; }`
- `void registerWorkspaceFileTools(ToolRegistry& registry, const WorkspaceFileToolOptions& options)`
- 工具名：`list_directory`、`read_text_file`、`create_text_file`、`write_text_file`、`replace_text_in_file`、`find_files`、`search_text`

### 3. 契约

- 未提供 `workspace_file_tools` 时，`SimpleAgent` 不注册任何文件工具；提供有效根目录时，七项工具都以 `Low` 注册。
- 目标路径必须是工作区内相对路径。已有目标以 `canonical` 验证，新建目标以 canonical 父目录加文件名验证，最终路径必须属于授权根目录。
- 拒绝绝对路径、NUL、根目录逃逸、符号链接/目录联接点逃逸、`.git`、`.env`、`.env.*` 以及常见私钥文件名或扩展名。
- 只处理无 NUL 的 UTF-8 文本；默认单文件读取、创建、覆盖和替换内容上限 64 KiB，目录列举上限 256 个单层条目，单次搜索上限 256 个命中。
- `list_directory` 除跳过标准符号链接外，还必须对每个条目 `canonical` 最终目标；目录联接点等未被 `is_symlink()` 标识的重解析点若越过根或落在敏感位置，必须隐藏而不是作为可访问目录返回。
- `create_text_file` 只允许不存在的目标；`write_text_file` 只允许已存在普通文件；`replace_text_in_file` 要求非空查找文本恰好匹配一处。
- `find_files` 接受可选相对目录 `path`、文件名 UTF-8 字面量 `query` 和不超过配置的 `max_results`；只递归返回安全普通文件的相对路径，并以 `truncated` 标记超出上限的结果。
- `search_text` 接受可选相对目录 `path`、必填非空 UTF-8 字面量 `query` 和不超过配置的 `max_results`；每行最多返回最早命中，结果项含 `path`、从 1 开始的 UTF-8 码点 `line`/`column` 及受限 `text` 预览。
- 两项搜索不得跟随符号链接，敏感条目和根外重解析点必须静默隐藏；`search_text` 遇到超限、非 UTF-8、锁定或并发变化文件时跳过该文件继续，并通过 `skipped_files` 说明此类跳过数量。
- 文件系统并发删除、截断、权限变化或写入失败必须返回失败 ToolResult；首版不承诺跨进程事务、原子替换或备份。

### 4. 校验与错误矩阵

| 条件 | 行为 |
| --- | --- |
| 根目录为空、不存在、非目录或敏感目录 | 注册阶段抛 `std::invalid_argument` |
| 文件工具名称已被注册表占用 | 注册阶段抛 `std::invalid_argument`，不注册半套工具 |
| 文件路径为绝对路径、含 NUL、越过根或落在敏感位置 | handler 失败，经 `ToolRegistry` 返回失败 `ToolResult` |
| 读取/覆盖目标不存在或不是普通文件 | 返回失败 `ToolResult` |
| 创建目标已存在或父目录不存在 | 返回失败 `ToolResult` |
| 文本非 UTF-8、含 NUL 或超出容量 | 返回失败 `ToolResult` |
| 替换零处、多处或空查找串 | 返回失败 `ToolResult`，不修改文件 |
| 目录条目超出限制 | 返回失败 `ToolResult`，不静默截断 |
| 搜索上限为零、非整数或超过注册配额 | 返回失败 `ToolResult` |
| `search_text` 查询为空、包含 NUL 或不是 UTF-8 | 返回失败 `ToolResult` |
| 递归搜索命中超过本次上限 | 返回成功 `ToolResult`，保留前序命中并设 `truncated == true` |
| 搜索中单个文件超限、非 UTF-8、无法读取或并发变化 | 跳过该文件；其余文件继续，`search_text` 增加 `skipped_files` |

### 5. Good / Base / Bad

- Good：调用方传入专用目录；模型先用 `find_files` 或 `search_text` 缩小范围，再用 `read_text_file` 或唯一替换工具完成受限任务。
- Base：只注册业务 Low 工具，不传工作区选项；模型不会看到任何文件工具。
- Bad：把仓库根目录、用户主目录或 `.env` 目录当作默认工作区，或把递归遍历结果未经 `canonical` 和敏感路径校验直接回填。

### 6. 必要测试

- `tests/agent/workspace_file_tools_test.cpp` 必须覆盖写入工具的正常闭环、重复创建、缺失目标、替换零/多匹配、容量、NUL、无效 UTF-8、绝对路径、`..`、敏感路径与目录容量。
- 必须覆盖 `find_files` 的递归普通文件发现、稳定排序、结果截断及敏感路径隐藏；覆盖 `search_text` 的行列号、UTF-8 文本预览、超限/非 UTF-8 跳过、空查询与结果截断。
- 必须创建真实工作区外目录，并通过目录符号链接或 Windows 目录联接点验证读取和两项搜索均拒绝逃逸，且断言 `list_directory` 不暴露该链接条目。
- 所有文件测试只使用系统临时目录；测试结束后清理根目录与外部链接目标。

### 7. 错误写法 vs 正确写法

#### 错误写法

```cpp
// 错误：递归返回原始条目，链接和敏感目录仍可能进入模型上下文。
for(const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    files.push_back(entry.path().string());
}
```

#### 正确写法

```cpp
SimpleAgentOptions options;
options.workspace_file_tools = WorkspaceFileToolOptions{"D:/agent-workspace"};
SimpleAgent agent(client, std::move(options));
// handler 内部先 canonical 条目、验证根目录与敏感路径，再返回相对路径和受限结果。
```
