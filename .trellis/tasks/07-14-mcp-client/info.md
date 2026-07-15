# MCP Client 实现设计

## 1. 实现依赖图

```text
ai_sdk_mcp
  ├─ PUBLIC: ai_sdk、nlohmann_json
  ├─ PRIVATE: cpr、Threads
  ├─ MCPClient
  │    ├─ MCPProtocol：JSON-RPC、生命周期、Tools
  │    └─ IMCPTransport
  │         ├─ StdioMCPTransport
  │         │    └─ detail::Process（Windows / POSIX 条件源）
  │         └─ StreamableHttpMCPTransport
  │              ├─ detail::MCPHttpBackend（cpr 封装）
  │              └─ detail::MCPSseDecoder
  └─ MCPToolAdapter
       └─ ai_sdk::ToolRegistry / ToolHandler

ai_sdk 不得包含 MCP 头文件，也不得链接 ai_sdk_mcp。
```

## 2. 文件职责

| 文件或目录 | 单一职责 |
|---|---|
| `include/mcp/MCPServerConfig.h` | 公开配置、限制和逐请求凭据接口 |
| `include/mcp/MCPTypes.h` | 状态、错误、目录、工具与调用结果值类型 |
| `include/mcp/IMCPTransport.h` | 两阶段准备/提交和传输事件窄接口 |
| `include/mcp/MCPClient.h` | 同步公开操作与只读状态入口 |
| `include/mcp/MCPToolAdapter.h` | 显式筛选、绑定、注册和注销入口 |
| `src/mcp/MCPProtocol.cpp` | JSON-RPC 构造、严格校验和 Tools 解析 |
| `src/mcp/MCPClient.cpp` | 生命周期、前台槽、请求关联、目录代次与错误完成 |
| `src/mcp/StdioMCPTransport.cpp` | 逐行收发、出站队列与进程事件转换 |
| `src/mcp/StreamableHttpMCPTransport.cpp` | POST/GET/DELETE、会话、Listener 和恢复编排 |
| `src/mcp/detail/` | 不导出的协议、SSE、HTTP Backend 与进程边界 |
| `tests/mcp/` | 协议单元、真实 stdio、真实 HTTP 三类顶层 CTest |

## 3. 必须保持的实现不变量

- 用户代码和凭据 Provider 只能在 Client / Transport 状态锁外调用。
- `prepareMessage()` 不执行 MCP I/O；`commitPrepared()` 只在状态复核后原子入队。
- 工具请求只有进入发送队列后才是 `Submitted`；此后没有终局 JSON-RPC 结果时统一返回 `OutcomeUnknown`。
- 单个 Client 只有一个公开操作槽；Listener、控制消息、状态读取和 `close()` 不占槽。
- `list_changed` 只阻断依赖目录的 `tools/list` / `tools/call`，不能阻断协议必需控制消息。
- 公开头文件不出现 cpr、libcurl、Win32 句柄、POSIX 文件描述符或线程实现类型。
- 所有消息、队列、SSE 事件、分页、错误文本、stderr 和重连均受配置上限约束。
- MCP 不新增隐式 Agent Loop，也不修改 `AIClient` 持有连接。

## 4. 小步实现顺序

1. 建立可选目标、公共类型、错误类型、协议辅助函数和脚本传输 Client 测试。
2. 增加 `ToolRegistry` 通用注销和 MCP Tool Adapter。
3. 实现 Windows / POSIX Process 与 stdio Transport，完成真实子进程测试。
4. 实现增量 SSE 与 Streamable HTTP，完成真实回环 HTTP 测试。
5. 增加示例、README、规格更新，并执行 Windows / Linux、ON / OFF 完整矩阵。

每一步先通过本层测试再继续；同类失败连续三次时暂停并重新评估。
