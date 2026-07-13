# 实现 DeepSeek Tool Call 闭环

## 目标

把当前已能发送工具定义并解析 DeepSeek Tool Call 响应的 SDK，补齐本地工具注册、串行执行、工具结果消息回填和可运行示例。SDK 只提供单轮模型调用和显式工具执行能力，是否把结果回传给模型并继续下一轮由上层应用或后续 Agent 决定。

## 已知事实

- `DeepSeekProvider` 已将 `ChatRequest.tools` 转为 OpenAI-compatible `tools` 请求字段，并会把普通响应中的 `message.tool_calls` 解析到 `ChatResponse` 和 `Message`。
- `ToolRegistry` 已能在内存中注册、查询、列举和执行本地 C++ 工具；`ToolExecutor` 已按输入顺序串行执行多个 `ToolCall`，并能生成绑定 `tool_call_id` 的 `Tool` 角色消息。
- `AIClient` 持有一个 `ToolRegistry`，但目前没有将已注册工具注入请求，也没有编排工具调用后的二次模型请求。
- `examples/04_register_tool`、`examples/05_tool_call`、`examples/06_simple_agent` 及 `tests/tool` 目前只有 CMake 占位入口，没有实现或测试。
- 项目根目录的 PRD 明确要求 SDK 层负责模型调用、Tool Call 解析和工具执行；是否继续循环属于 Agent 层决策，SDK 不应代替调用方编排多轮模型请求。

## 技术方案

- 本任务只面向 DeepSeek 的非流式 `chat` 接口；流式工具调用和 MiniMax Provider 留待后续任务。
- 第一版本地工具保持同步、串行执行；不引入线程、超时调度、MCP、Skill、Trace 持久化或网络工具。
- 参数 Schema 的完整 JSON Schema 校验不在本任务中实现，工具处理函数自行校验业务参数并返回结构化失败结果。
- `AIClient::executeToolCalls(...)` 接收模型返回的 `std::vector<ToolCall>`，复用客户端持有的 `ToolRegistry` 串行执行，并返回 `std::vector<ToolExecutionResult>`。
- `ToolExecutionResult::toToolMessage()` 继续作为结果到模型消息的显式转换边界；SDK 不额外提供自动追加历史或自动二次请求的方法。

## 需求

- 所有新增和调整的 C++ 接口、非显然实现、测试场景及示例步骤必须补充详细简体中文注释；注释说明职责、输入输出、边界与设计原因，不复述代码字面逻辑。
- 在 `AIClient` 提供显式的工具能力入口：调用方可从其注册表取得工具定义，并执行某次 `ChatResponse` 中的 Tool Call；普通 `AIClient::chat` 的现有语义不改变。
- 调用方能够将已注册工具作为 `ChatRequest.tools` 注入一次模型请求；SDK 不隐式修改会话历史或再次调用模型。
- 收到 Tool Call 后，`AIClient` 按模型返回顺序执行工具，并返回带对应 `tool_call_id` 的 `Tool` 角色消息，供调用方自行保存、展示或用于下一次 `chat` 请求。
- 工具不存在、工具处理函数抛异常、无效参数等失败必须转为结构化工具结果和 tool 消息；单个工具失败不能阻止同一批其他工具执行。
- 提供离线单元测试覆盖工具注册、串行顺序、工具失败转换和消息回填；提供一个使用 `get_current_time` 的真实 DeepSeek 示例。

## 验收标准

- [x] 新增代码的中文注释覆盖率大于 30%，公开 API 注释明确说明 SDK 与 Agent 的职责边界。
- [x] 纯文本请求保持现有 `AIClient::chat` 行为不变。
- [x] 已注册工具会以正确的 OpenAI-compatible Schema 发送给 DeepSeek。
- [x] 模型返回一个或多个 Tool Call 时，工具按响应顺序执行，结果消息保留对应的 `tool_call_id`。
- [x] 未知工具和工具异常会成为可序列化的失败结果，而非中断同一批 Tool Call 执行。
- [x] SDK 不会自动把工具结果追加到请求历史，也不会自动发起第二次模型调用。
- [x] `examples/05_tool_call` 可在配置 `DEEPSEEK_API_KEY` 后真实运行。
- [x] 工具相关测试可离线运行，且全量本地构建和测试通过。

## 决策（ADR-lite）

**背景**：项目已有 `AIClient`、Provider、ToolRegistry 和 ToolExecutor，但缺少从统一 SDK 入口执行一次模型响应中的 Tool Call 的公开接口。

**决定**：Tool Call API 放在 SDK 的 `AIClient` 中，但只负责单批 Tool Call 的显式执行和结果消息构造；`executeToolCalls(...)` 返回 `std::vector<ToolExecutionResult>`，不把多轮 LLM 调用循环放入 SDK。

**影响**：调用方能以单一 SDK 入口完成模型协议适配和本地工具执行，现有普通聊天 API 保持兼容；上层应用或后续 `SimpleAgent` 可以决定是否以结果消息发起下一轮调用。

## 技术备注

- 已检查：`src/provider/DeepSeekProvider.cpp`、`include/tool/ToolRegistry.h`、`include/tool/ToolExecutor.h`、`include/core/Message.h`、`src/core/Message.cpp`、`include/AIClient.h`、`src/AIClient.cpp`、`examples/01_chat_deepseek/main.cpp`、`tests/provider/deepseek_provider_test.cpp`、`docs/PRD.md`。
- 现有 `DeepSeekProvider` 已具备工具请求和响应协议适配；本任务应复用该边界，避免在 Tool 层复制 Provider JSON 逻辑。
- `ToolExecutor` 依赖 `ToolRegistry` 的引用，可由 `AIClient` 在单批工具执行入口内部复用。

## 定义完成

- 补齐相应单元测试、真实示例和 README 使用说明。
- 使用仓库现有 CMake 预设执行本地构建与测试；在线示例仅在本机存在密钥时执行。
- 根据实现中沉淀的稳定约定更新 Trellis 规格。

## 实施计划

1. 将工具注册、查询和执行行为移入 `.cpp`，补齐输入校验与异常收敛。
2. 在 `AIClient` 增加 `executeToolCalls(...)`，保持 `chat(...)` 行为不变。
3. 增加离线工具单元测试，覆盖正常、批量、未知工具与处理函数异常。
4. 实现 `04_register_tool` 和 `05_tool_call` 示例；Tool Call 示例只演示 SDK 能力，由示例代码显式决定是否发起第二轮请求。
5. 更新 README，执行本地构建、全量测试和可用时的真实 DeepSeek 示例验证。

## 范围外

- MiniMax Provider、流式 Tool Call、并行工具执行。
- MCP、Skill Loader、长期 Trace 持久化。
- 自动或有限轮次 Agent 循环、工具权限审批和完整 JSON Schema 引擎。

## 验证记录

- 2026-07-13：`cmake --preset windows-debug` 与 `cmake --build --preset windows-debug` 通过。
- 2026-07-13：`ctest --preset windows-debug --output-on-failure` 共 5 组测试全部通过。
- 2026-07-13：`example_register_tool.exe` 离线执行得到 `{"sum":42.0}`。
- 2026-07-13：`example_tool_call.exe` 完成真实 DeepSeek Tool Call，并输出工具返回的本地时间。
- 2026-07-13：新增 C++ 文件注释率最低为 30.2%，且 UTF-8 无 BOM、clang-format、Markdown 相对链接和模板占位符检查均通过。
