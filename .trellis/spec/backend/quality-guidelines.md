# 质量规范

## 适用范围

本文件定义当前 SDK 代码、示例和规格更新后的最低本地验证要求。

## 构建与测试事实

- 构建系统：CMake 3.23+，入口见 `CMakeLists.txt`。
- 语言标准：C++17，`CMAKE_CXX_EXTENSIONS OFF`。
- 预设：`CMakePresets.json` 提供 `windows-debug`、`windows-release`、`linux-debug`、`linux-release`。
- 测试框架：GTest，入口在 `tests/CMakeLists.txt`。
- 示例构建：由 `AISDK_BUILD_EXAMPLES` 控制。
- 测试构建：由 `AISDK_BUILD_TESTS` 控制，默认开启。
- MCP 构建：由 `AISDK_BUILD_MCP` 控制，默认开启并生成独立 `ai_sdk_mcp`。

## 本地验证要求

### 代码改动

改动 `include/`、`src/`、`tests/` 或 `examples/` 时，至少执行：

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug --output-on-failure
```

如果在 Linux 环境工作，使用 `linux-debug` 对应预设。

### 规格或文档改动

只改 `.trellis/spec/`、任务 PRD 或其他文档时，至少执行：

- 模板占位符扫描
- Markdown 相对链接检查
- `python ./.trellis/scripts/get_context.py --mode packages`，确认规格层能被 Trellis 正确识别

## 测试分层约定

- `tests/core/`：值对象、配置与 JSON 序列化行为。
- `tests/http/`：协议解析与 HTTP 边界行为。
- `tests/provider/`：供应商请求和真实联机测试。
- `tests/smoke/`：从 SDK 门面观察的最小通路。
- `tests/tool/`：工具注册、批量执行、异常收敛和 Tool 结果消息转换；目标名为 `ai_sdk_tool_test`。
- `tests/trace/`：Trace 会话、并发、脱敏、JSON 与跨层确定性验证；目标名为 `ai_sdk_trace_test`。
- `tests/mcp/`：MCP 单元、真实 stdio 与真实 HTTP/TLS；顶层 CTest 固定为 `ai_sdk_mcp_test`、`ai_sdk_mcp_stdio_test`、`ai_sdk_mcp_http_test`。

## 新增或修改代码时必须覆盖的验证点

- 修改配置装载：补 `tests/core/config_test.cpp` 风格的单元测试。
- 修改消息或响应模型：补 `tests/core/message_test.cpp`、`chat_request_test.cpp`、`chat_response_test.cpp` 同层测试。
- 修改 SSE 或流式协议：补 `tests/http/sse_parser_test.cpp` 风格的正常、边界和错误案例。
- 修改 Provider 行为：补 `tests/provider/deepseek_provider_test.cpp` 同层断言；涉及外网时保留可跳过机制。
- 修改 SDK 总入口或 Provider 选择：补 `tests/smoke/ai_sdk_smoke_test.cpp` 类似的最小集成断言。
- 修改工具定义、注册表或执行器：补 `tests/tool/` 的正常、边界和错误恢复用例，并验证 `AIClient` 门面不会隐式发起网络请求。
- 修改 Trace 或跨层埋点：补 `tests/trace/` 的关闭、成功、失败、并发、脱敏和敏感哨兵断言。
- 修改 MCP：正常、边界、错误恢复必须同时覆盖；stdio 使用真实无 Shell 子进程，HTTP 使用真实回环 Socket，TLS 在 Windows/Linux 都执行自签名拒绝、测试 CA 正确主机成功、同 CA 错误主机拒绝三项对照。

## MCP 强制本地矩阵

- Windows MSVC：Debug ON 全量测试、Release ON 三项 MCP 测试、Debug OFF 核心全量测试。
- Linux GCC：Debug ON 全量测试、Release ON 三项 MCP 测试、Debug OFF 核心全量测试。
- 验收只能使用版本控制内 `windows-debug`、`windows-release`、`linux-debug`、`linux-release` 预设；`local-*` 只能用于开发反馈。
- ON 时 `ctest -N -L mcp` 必须精确列出三项；OFF 时必须为零，并检查 `compile_commands.json` 与 `build.ninja` 不含 MCP 源或目标。
- TLS、进程树回收和代理陷阱不得因平台能力不足而跳过；本机缺少 Linux/Windows 验证环境时任务保持未完成并记录阻塞，不能用远程 CI 或人工结果代替。

## 注释与编码验证

- 新增 C++ 代码必须使用详细简体中文注释说明职责、输入输出、边界和设计原因，不能只复述代码字面逻辑。
- 新增头文件、实现、示例和测试文件的注释覆盖率必须大于 30%；可按“以 `//` 开头的注释行 / 非空行”进行本地检查。
- 所有新增或修改的代码文件必须保持 UTF-8 无 BOM，并通过仓库 `.clang-format` 格式化检查。
- `.clang-format` 本身属于项目级格式契约；功能任务不得为了让当前改动通过而调整规则。确需变更时必须独立说明影响范围与理由，并重新验证所有受影响文件。

## 代码评审视角

- 公开头文件是否泄露三方实现细节。
- 例外路径是否给出明确异常类型和消息。
- 是否沿用已有测试分层，而不是把所有验证塞进一个冒烟用例。
- 示例代码是否仍然代表真实推荐用法。
- 规格是否需要随实现一起更新。

## 常见风险

- 只改示例不改测试，导致对外用法与真实实现漂移。
- 流式改动只验证成功路径，漏掉 `null` 内容、错误事件和回调异常。
- 依赖真实 API 的测试没有可跳过条件，导致本地环境无法重复验证。
