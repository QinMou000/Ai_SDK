# MCP Client 本地验证记录

## 当前状态

- 状态：实现与验证进行中，尚未达到任务完成条件。
- 验证原则：只接受本机自动执行结果，不使用远程 CI、人工外包或旧二进制结果。
- 失败处理：构建命令失败时不继续接受已有测试程序的成功结果；修复环境或实现后必须从构建重新执行。
- 本轮补充：stdio 集成测试显式依赖 `mcp_stdio_fixture`，单独构建测试目标时不会误用旧夹具；新增 CRLF、stderr 洪泛、stdout 非法帧与 stdin 提前关闭覆盖。
- 本轮补充：请求段从 `commitPrepared()` 原子提交点开始；HTTP 凭据准备只消耗公开操作绝对上限。POST SSE 完成合法响应头后切换为 SSE 空闲上限与公开绝对上限，普通 JSON 仍受请求段限制。

## 已完成的开发期验证

| 范围 | 命令摘要 | 当前结果 |
| --- | --- | --- |
| MCP 单元 | 加载 `vcvars64.bat`，构建 `ai_sdk_mcp_test`，再用 CTest 精确筛选 | Debug 75/75 通过；重复终局响应与两条目录失效 Binding 竞态用例各连续 50 次通过 |
| stdio 集成 | 构建真实 C++ Server 夹具与 `ai_sdk_mcp_stdio_test` | Debug 6/6 通过；覆盖 CRLF、stderr 洪泛、非法 stdout、stdin 关闭、后代进程回收和 25 轮资源用例 |
| HTTP 集成 | 构建 `ai_sdk_mcp_http_test`，覆盖 Listener 状态矩阵、405、POST JSON/SSE、Session 404、代理陷阱与 Event-ID GET 恢复 | Debug 普通 HTTP 9/9 通过；TLS 三项被下述平台合同阻塞 |
| HTTP 稳定性 | 同进程重复五项普通 HTTP 用例 | 20 轮、共 100/100 通过 |

## 源文件格式、编码与注释率

执行范围为 `include/mcp`、`src/mcp`、`tests/mcp` 和 `examples/08_mcp_tool_call` 中的 36 个 C++ 文件。使用当前工作区 120 列 `clang-format --dry-run --Werror`、UTF-8 无 BOM 检查，以及“以 `//`、`/*`、`*` 或 `*/` 开头的注释行 / 非空行”口径；根据用户于 2026-07-15 的确认，门槛为不低于 20%。全部通过，最低值为 20.4%。

| 文件 | 注释行 | 非空行 | 注释率 | 结果 |
| --- | ---: | ---: | ---: | --- |
| `examples/08_mcp_tool_call/main.cpp` | 39 | 127 | 30.7% | 通过 |
| `include/mcp/IMCPTransport.h` | 34 | 102 | 33.3% | 通过 |
| `include/mcp/MCPClient.h` | 18 | 59 | 30.5% | 通过 |
| `include/mcp/MCPServerConfig.h` | 35 | 116 | 30.2% | 通过 |
| `include/mcp/MCPToolAdapter.h` | 15 | 48 | 31.3% | 通过 |
| `include/mcp/MCPTypes.h` | 44 | 144 | 30.6% | 通过 |
| `src/mcp/detail/MCPDeadline.h` | 8 | 25 | 32.0% | 通过 |
| `src/mcp/detail/MCPHttpTestFactory.h` | 11 | 36 | 30.6% | 通过 |
| `src/mcp/detail/MCPProtocol.h` | 19 | 62 | 30.6% | 通过 |
| `src/mcp/detail/MCPSseDecoder.cpp` | 71 | 234 | 30.3% | 通过 |
| `src/mcp/detail/MCPSseDecoder.h` | 27 | 68 | 39.7% | 通过 |
| `src/mcp/detail/MCPText.h` | 17 | 56 | 30.4% | 通过 |
| `src/mcp/detail/MCPToolCatalogAccess.h` | 11 | 35 | 31.4% | 通过 |
| `src/mcp/detail/MCPTransportFactory.h` | 4 | 14 | 28.6% | 通过 |
| `src/mcp/detail/Process.h` | 20 | 61 | 32.8% | 通过 |
| `src/mcp/MCPClient.cpp` | 520 | 1778 | 29.2% | 通过 |
| `src/mcp/MCPProtocol.cpp` | 146 | 493 | 29.6% | 通过 |
| `src/mcp/MCPServerConfig.cpp` | 119 | 398 | 29.9% | 通过 |
| `src/mcp/MCPToolAdapter.cpp` | 76 | 254 | 29.9% | 通过 |
| `src/mcp/MCPTypes.cpp` | 39 | 132 | 29.5% | 通过 |
| `src/mcp/process/PosixProcess.cpp` | 391 | 1196 | 32.7% | 通过 |
| `src/mcp/process/WindowsProcess.cpp` | 310 | 961 | 32.3% | 通过 |
| `src/mcp/StdioMCPTransport.cpp` | 221 | 663 | 33.3% | 通过 |
| `src/mcp/StreamableHttpMCPTransport.cpp` | 689 | 2401 | 28.7% | 通过 |
| `tests/mcp/helpers/mcp_stdio_fixture.cpp` | 112 | 408 | 27.5% | 通过 |
| `tests/mcp/helpers/McpTlsTestCertificates.h` | 63 | 309 | 20.4% | 通过 |
| `tests/mcp/helpers/McpTlsTestServer.h` | 11 | 35 | 31.4% | 通过 |
| `tests/mcp/helpers/PosixMcpTlsTestServer.cpp` | 249 | 755 | 33.0% | 通过 |
| `tests/mcp/helpers/WindowsMcpTlsTestServer.cpp` | 292 | 954 | 30.6% | 通过 |
| `tests/mcp/mcp_adapter_test.cpp` | 145 | 472 | 30.7% | 通过 |
| `tests/mcp/mcp_client_test.cpp` | 455 | 1557 | 29.2% | 通过 |
| `tests/mcp/mcp_http_integration_test.cpp` | 363 | 1636 | 22.2% | 通过 |
| `tests/mcp/mcp_protocol_test.cpp` | 208 | 604 | 34.4% | 通过 |
| `tests/mcp/mcp_sse_decoder_test.cpp` | 82 | 275 | 29.8% | 通过 |
| `tests/mcp/mcp_stdio_integration_test.cpp` | 109 | 399 | 27.3% | 通过 |
| `tests/mcp/mcp_types_config_test.cpp` | 134 | 451 | 29.7% | 通过 |

## Windows 公共预设验收证据

| 矩阵项 | 本地结果 |
| --- | --- |
| MSVC Debug ON 配置与全量构建 | 使用版本控制内 `windows-debug` 预设，配置与 `/W4` 全量构建通过；MCP CTest 清单精确为 `ai_sdk_mcp_test`、`ai_sdk_mcp_stdio_test`、`ai_sdk_mcp_http_test` 三项。 |
| MSVC Debug ON MCP CTest | `ctest --test-dir build\\windows-debug -L mcp --output-on-failure` 中单元与 stdio 两项通过；HTTP 目标仅三项 TLS 对照失败，普通 HTTP 9/9 全部通过。 |
| MCP 单元与竞态稳定性 | `ai_sdk_mcp_test` 当前 75/75 通过；包含入站有界队列、提交点计时与目录失效零写入断言。 |
| HTTP 非 TLS 回归 | `MCPHttpInternalTest.*:MCPHttpIntegrationTest.*` 当前 13/13 通过；包含建流后超过请求段的 POST SSE 计时对照，以及 300、400、401、403、404、409、429、500、503 的 Listener 状态映射。 |
| MSVC Release ON | 使用版本控制内 `windows-release` 预设配置并全量构建通过；MCP 单元 75/75、stdio 6/6、普通 HTTP 13/13 通过；HTTP 目标仅三项 TLS 对照被下述平台合同阻塞。 |
| MSVC Debug OFF | 本轮以独立 `build/windows-debug-mcp-off` 覆盖 `AISDK_BUILD_MCP=OFF`，显式指定 `D:/vcpkg/scripts/buildsystems/vcpkg.cmake` 后全量 6/6 CTest 通过；MCP 标签测试精确为零，`compile_commands.json` 不含 `src/mcp/`，`build.ninja` 不含 `ai_sdk_mcp`。 |

## Windows TLS 强制门禁阻塞

三项 TLS 对照均使用同一真实 cpr/libcurl 路径，并在本地 TLS Server 启动阶段稳定失败：

```text
创建 SChannel 入站凭据失败，SChannel 状态码=0x8009030E（未提供可用凭据）
```

夹具已确认 PFX 导入成功、证书与私钥匹配，且 `/W4` 编译通过；失败发生在 `AcquireCredentialsHandleW` 创建 SChannel 入站凭据时。没有采用持久化私钥、修改系统信任库、管理员权限、关闭证书校验或改用不同 TLS 后端的逃生方案。

微软 `PFXImportCertStore` 文档明确说明，`PKCS12_NO_PERSIST_KEY` 使私钥保存在进程内 `CERT_KEY_CONTEXT_PROP_ID`；当密钥需要跨进程封送时，该形式不可用。当前 Windows 的 SChannel 凭据处理返回 `SEC_E_NO_CREDENTIALS`，因此本机无法同时满足 PRD 指定的“SChannel Server + `PKCS12_NO_PERSIST_KEY`”组合：

- `PFXImportCertStore`：<https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-pfximportcertstore>
- SChannel `AcquireCredentialsHandle`：<https://learn.microsoft.com/en-us/windows/win32/secauthn/acquirecredentialshandle--schannel>

该问题不能在不改变已确认安全选型的前提下继续修补。需要用户确认 PRD 修订方向后，才能完成 Windows TLS 三项对照；在此之前任务保持 `in_progress`。

一次增量构建曾因未加载 MSVC 开发环境出现 `fatal error C1083: 无法打开包括文件: “cstdint”: No such file or directory`。该次构建后的旧 CTest 成功结果已作废；加载 `D:\software\VS\Community\VC\Auxiliary\Build\vcvars64.bat` 后重新编译并通过。

## Linux 本地验收阻塞

当前主机没有可用的 Linux 执行环境：

- `wsl.exe --status` 退出码 50，明确提示未安装适用于 Linux 的 Windows 子系统。
- `wsl.exe --list --verbose` 退出码 1，没有已安装发行版。
- `docker.exe`、`podman.exe`、`nerdctl.exe`、`multipass.exe`、`VBoxManage.exe`、`vmrun.exe`、`qemu-system-x86_64.exe` 均无法找到。
- 未发现 Docker、Podman、Rancher Desktop、VirtualBox、VMware、Multipass、QEMU 或 Hyper-V 可用服务与安装目录。
- Git Bash 是 Windows 进程环境，不能证明 POSIX 进程组、OpenSSL TLS Server 或 Linux FD 回收行为。

### 补偿计划与截止条件

1. 先完成全部平台无关实现、Windows Debug/Release、MCP ON/OFF、TLS 和资源门禁。
2. 由用户提供或授权安装本机 WSL/Linux 发行版，或提供本机 Docker/Podman/虚拟机环境；不使用远程 CI 代替。
3. 在该本地 Linux 环境中使用版本控制内 `linux-debug`、`linux-release` 预设执行 Debug ON、Release ON、Debug OFF，并核对三项 MCP CTest 清单、编译数据库和构建图隔离。
4. 截止条件：上述 Linux 矩阵必须在任务被标记完成前全部通过；环境未补齐期间，本任务保持 `in_progress`，不得归档或声明无缺陷交付。

## 待执行最终门禁

- Windows MSVC Debug ON：`mcp` 标签下仅余 TLS 三项对照；MCP 单元 75/75、stdio 6/6、普通 HTTP 13/13 已通过。
- Windows MSVC Release ON：仅余包含 TLS 三项对照的完整 `ai_sdk_mcp_http_test`；构建、MCP 单元 75/75、stdio 6/6 与普通 HTTP 13/13 均通过。
- Windows MSVC Debug OFF：已完成并通过。
- Linux GCC Debug ON、Release ON、Debug OFF：等待本地 Linux 环境。
- 所有新增 C++ 文件：clang-format、UTF-8 无 BOM、尾随空白和本任务确认的统一口径注释率不低于 20%。
- 文档与规格：占位符扫描、相对链接检查、Trellis 包上下文加载。
