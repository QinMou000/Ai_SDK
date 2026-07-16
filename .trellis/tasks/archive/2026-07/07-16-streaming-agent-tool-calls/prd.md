# 实现流式 Agent 工具调用

## 目标

让 `SimpleAgent` 在保持同步调用模型的前提下，通过 `streamChat()` 实时交付文本增量、聚合流式 Tool Call、执行既有低风险工具并继续下一轮 ReAct 决策；示例 Agent 使用该能力展示真实的流式过程。

## 已确认的需求

- 新增独立的 `SimpleAgent::runStream(...)` 入口，不改变 `run(...)` 的同步语义。
- 流事件中的工具调用增量必须从原始 JSON 字符串演进为结构化的索引、调用 ID、函数名和参数片段，不能让 Agent 依赖 DeepSeek 私有 JSON 结构。
- Agent 按工具索引聚合参数片段；本轮流结束后才生成完整 `ToolCall`，复用既有低风险筛选、串行执行、ToolMessage 回填与 16 轮安全熔断。
- 文本增量需实时通知调用方；工具调用开始/结束和工具执行结果需以结构化 Agent 事件通知调用方。
- 流式 Tool Call 缺少 ID/名称、参数 JSON 非法或收到流式 Error 时不得执行工具，并产生明确失败结果。
- 显式 Trace 重载必须让流式模型请求和工具批次继续写入同一会话。
- `example_simple_agent` 改为使用流式入口，实时打印文本与工具状态。

## 验收标准

- [x] 文本流在没有 Tool Call 时按顺序回调，并返回完整最终答案。
- [x] 多片段、多个交错 Tool Call 可按索引聚合为完整参数并按模型顺序执行。
- [x] 工具执行后会回填 assistant 和 ToolMessage，并继续下一轮流式模型调用。
- [x] 非 Low 工具、未知工具和 handler 失败仍沿用现有恢复策略。
- [x] 不完整 Tool Call、非法参数 JSON 与 SSE Error 不产生工具副作用。
- [x] 16 轮熔断、空输入、显式 Trace 与现有 `run(...)` 行为均有离线测试。
- [x] Windows Debug 格式、构建和全部本地测试通过。

## 技术方案

在 HTTP/SSE 协议层定义供应商无关的 `ToolCallDelta` 并由 `SSEParser` 从 OpenAI-compatible 流事件解析。`SimpleAgent` 增加私有聚合器与 `runStreamInternal(...)`，只消费公开 `StreamEvent`，不直接解析 Provider JSON；每轮结束时构造 `Message` 和 `ToolCall` 后复用现有工具执行函数。新公开 Agent 流事件只交付文本增量和安全的工具生命周期信息，不额外暴露工具参数或执行结果正文。

## 决策记录

**背景**：当前 Provider 已发出 `ToolCallDelta`，但载荷是原始 JSON，`SimpleAgent` 只使用非流式 `chat()`，因此无法执行流式工具调用。

**决策**：将工具增量结构化到 SSE 协议边界，在 Agent 层聚合并执行；不在 `AIClient` 或 `ToolExecutor` 中引入 ReAct 循环。

**影响**：`StreamEvent` 增加结构化 Tool Call 字段，后续 Provider 可以映射到同一协议；首版仍为同步回调，不引入异步、取消或并行工具执行。

## 范围外

- 异步 API、协程、后台线程、取消令牌和背压控制。
- 并行 Tool Call 执行、跨请求会话记忆或自动重试。
- 非 OpenAI-compatible Provider 的具体流式协议适配。

## 涉及位置

- `include/http/SSEParser.h`
- `src/http/SSEParser.cpp`
- `include/agent/SimpleAgent.h`
- `src/agent/SimpleAgent.cpp`
- `tests/http/sse_parser_test.cpp`
- `tests/agent/simple_agent_test.cpp`
- `examples/06_simple_agent/main.cpp`
- `README.md`
- `.trellis/spec/backend/{sdk-contracts,agent-contracts}.md`
