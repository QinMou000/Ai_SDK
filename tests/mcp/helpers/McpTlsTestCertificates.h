#pragma once

#include <string>
#include <string_view>

#include "McpTlsTestServer.h"

namespace aiSDK::test {

// 固定名称同时用于证书 SAN 与测试 Session 的仅本地名称解析。
// 证书不包含 IP SAN、通配符或第二个 DNS 名称，便于隔离主机名校验变量。
inline constexpr std::string_view kMcpTlsTestServerName = "mcp.test.local";

// PFX 密码只保护版本库内无生产用途的测试资源，不属于秘密或产品配置。
// Windows 导入时仍使用无持久化标志，密码不会进入系统证书存储。
inline constexpr std::string_view kMcpTlsTestPkcs12Password = "mcp-test-only";

// 每组资源同时保留跨平台格式：POSIX 读取证书和 PKCS#8，Windows 读取 PFX。
// ASCII Base64 避免源文件携带二进制字节，也保证 UTF-8 无 BOM 检查稳定。
struct McpTlsCertificateResource {
    std::string_view certificate_der_base64;
    std::string_view private_key_pkcs8_base64;
    std::string_view pkcs12_base64;
};

// 证书资源合同：这些字面量仅服务本地回环 TLS 夹具，绝不属于 SDK 默认信任配置。
// 资源采用 Base64 文本，是为了让源码审阅、跨平台编译和 UTF-8 无 BOM 检查都可重复。
// DER 证书值供客户端测试工厂转换为 CA PEM，不依赖系统证书库状态。
// PKCS#8 私钥值只传入 POSIX 测试 Server，客户端接口没有获取或导出它的路径。
// PKCS#12 值只传入 Windows SChannel 夹具，导入必须使用不持久化私钥的标志。
// 三个字段被组合为资源结构，是为了避免不同证书和私钥交叉配对造成难定位握手失败。
// 根 CA 资源只能用于“受信且名称正确”的成功控制，不应被用于自签名拒绝场景。
// 自签名资源使用同一个 DNS SAN，故障比较能够隔离为信任链差异而不是主机名差异。
// 错误主机名测试复用受信根和同一 Server，以确保失败变量只有名称校验。
// 所有证书都有有限有效期；运行时 TLS 库仍负责判断当前时间，测试不绕开该生产行为。
// 文件不包含任何外部地址、生产域名、真实用户身份或可迁移到产品环境的凭据。
// 测试密码只保护仓库内的固定 PFX 包，不提供安全边界，也不能当作用户秘密处理。
// Base64 分段必须只在字符串字面量边界调整，修改资源时需要同步验证 DER、PFX 与私钥配对。
// 资源常量使用 string_view 且具有静态存储期，测试 Server 在异步线程中不会借用临时缓冲区。
// 夹具启动会将这些文本解码为进程内字节，不向临时目录、环境变量或调试日志写入私钥。
// 客户端成功用例仅把根证书作为当前 Transport 的 CA Blob，不能安装到 Windows 或 POSIX 信任库。
// 资源替换后必须同时重验自签名拒绝、正确名称成功和错误名称拒绝三条控制路径。
// 若 SChannel 无法获取入站凭据，这是夹具启动阻塞，不能通过降低客户端 TLS 校验来绕过。
// 阻塞状态需要保留原始 Windows 状态码，环境修复后仍以同一真实 TLS 矩阵复验。
// 这些测试资源不放入 public include，避免下游使用者把它们误解为 SDK 支持的 API。
// 所有字面量均为 ASCII，避免不同源代码页在 Base64 解码前改变输入字节。
// 证书链验证与代理禁用分别断言，受信根注入不能改变生产请求的代理安全设置。
// 客户端拒绝用例不读取远端证书详情，公开 MCP 错误只保留稳定的传输错误类别。
// 测试响应不会回显证书主题、序列号或私钥材料，失败诊断不因握手而扩大敏感输出。
// SAN 被限制为一个 DNS 名，避免通配符、IP SAN 或多名称掩盖主机名校验回归。
// 密钥算法与长度只为本地夹具选择，不构成 SDK 对生产 MCP Server 算法的支持承诺。
// 资源与测试目标一起编译，截断或配对错误必须在本地 Server 启动时显式失败。
// Windows 与 POSIX 读取不同容器格式只是系统 API 差异，三组验证场景的证书语义保持一致。
// 测试不使用运行时生成或网络下载的证书，因此离线重复运行的信任输入固定。
// 生成日期只用于资源审计；证书是否过期仍由当前 TLS 栈在握手时实际判断。
// MCPClient 不会自动加载此根 CA，只有内部测试工厂显式传递时它才用于当前 Transport。
// 本头文件不创建线程、Socket、证书库条目或其他全局初始化副作用。
// PFX 密码与 Base64 值不得出现在断言、异常、请求摘要或测试日志中。
// 成功控制和两个负向控制共同构成 TLS 验证矩阵，不能用其中单个用例代替整体判断。
// 资源位于测试帮助目录，生产目标的 CMake include 路径不应包含此目录。
// 源码格式化只能调整字符串字面量边界，不能改变 Base64 字节序列或证书资源组合。
// 资源替换应保持根 CA、根签名 Server 证书、私钥与 PFX 四者的明确对应关系。
// 测试 Server 的私钥只在当前进程 TLS 握手期间存在，close 后不会有后台任务继续持有它。
// 根 CA 注入不改变 HTTPS 请求禁用环境代理、禁止重定向和强制主机名校验的生产选项。
// 错误主机名场景复用同一回环端口，网络连通性或监听实现不能解释其预期失败。
// 自签名拒绝场景复用相同 DNS SAN，信任链之外的变量不能被错误归因。
// 正确主机名成功场景是两个拒绝场景的可用性对照，避免把夹具启动失败误判为安全通过。
// 所有 TLS 场景只连接本机回环地址，不查询公网 DNS、证书状态服务或系统信任网络资源。
// 若当前平台无法启动测试 Server，任务验证记录必须明确标记为阻塞而不是跳过该安全矩阵。
// 以下资源生成于 2026-07-14 UTC，有效期约 30 年，明显高于 20 年门槛。
// 测试启动仍会解析证书并校验当前时间，不能把生成时事实当作运行时假设。
// 资源行刻意保持为不可拆分的 Base64 字面量，禁止人工局部编辑或复用到生产。
inline constexpr std::string_view kMcpTlsRootCaDerBase64 =
    "MIIDdjCCAl6gAwIBAgIUZgW253ByvHTmh0vdUtXGSo6NiVkwDQYJKoZIhvcNAQELBQAwQDELMAkGA1UEBhMCQ04xDzANBgNVBAoMBmFpX3NkazEgMB"
    "4GA1UEAwwXYWlfc2RrLU1DUC1UZXN0LVJvb3QtQ0"
    "EwIBcNMjYwNzE0MjIxNzM5WhgPMjA1NjA3MTQyMjE3MzlaMEAxCzAJBgNVBAYTAkNOMQ8wDQYDVQQKDAZhaV9zZGsxIDAeBgNVBAMMF2FpX3Nkay1N"
    "Q1AtVGVzdC1Sb290LUNBMIIBIjANBgkqhkiG9w0B"
    "AQEFAAOCAQ8AMIIBCgKCAQEAwSluKPz9lx4vehVP3fOGj73bfWp4tFvkknDHwvhFk2VPaaZiJGrjwCpYqlZegK4q0P/"
    "mrHCm1pjiFTD81tSc9ZOX86PqCbfFN8GRSw9hbVoRWP+jx63fz4rwFji4P6RC2di/LuKBffiyysHnSmxG/RFnF76W/"
    "vDZwUxpQKwxy9TZ1519Jd5GFUfXcJV7LA+wmAlNAICGZo6FGrNE2kbvS57s4dbzWeEZfM7qtCHOakfhn6H/"
    "qsW2Fu3EjbaX+"
    "WQIrzdn4CDBOOPR40gcshYLvVXPQNZ4JqksjB92DfnS7xuyr0KfSQFTtvw5RdZoq07bbIxrSgS6srLM6oCsNmD9vwIDAQABo2YwZDAfBgNVHSMEGDA"
    "WgBRWtAKBlEFEMPv9rob2Yv2zqqdjUjASBgNVHRM"
    "BAf8ECDAGAQH/AgEAMA4GA1UdDwEB/wQEAwIBBjAdBgNVHQ4EFgQUVrQCgZRBRDD7/"
    "a6G9mL9s6qnY1IwDQYJKoZIhvcNAQELBQADggEBAIBtmdgcUy+QO5rPbYrxNzyMVW9gnL5a+"
    "OBwJv4oJCELRoBC3bPd1Z0CuHxm6kcLkb7UP7LBfFFfnjRCSx+xJz4OFfNJDXjd0Dts+"
    "kNET9bvAKbQDECPBoJcl/"
    "xmfHd86EHSwg0a3Thol2dc21xzis0qf0x5VeQkRyhNG1uMpDcpbqakqIYvt4sfJOMn1r96fR1tMfTE5tewMUYONIAXHCiwExh1T/"
    "1kWNxeJ0XFLNrsKS9rnDVaLkb1R045kXJugSHyhvOCU5DXe9+bsnTRQrdBZrKkcBA/"
    "EqcOnHwrv0MRjgmMhPVMiNv7FTL5CW1iht7dQmjQUXoFXKWExp2yNE8=";

// 根签名 Server 证书只有 DNS:mcp.test.local SAN，且不携带根 CA 私钥。
inline constexpr std::string_view kMcpTlsRootSignedServerCertificateDerBase64 =
    "MIIDmTCCAoGgAwIBAgIUCYLUg08reGTCfNbL5M2/"
    "kPymOvgwDQYJKoZIhvcNAQELBQAwQDELMAkGA1UEBhMCQ04xDzANBgNVBAoMBmFpX3NkazEgMB4GA1UEAwwXYWlfc2RrLU1DUC1UZXN0LVJvb3QtQ0"
    "EwIBcNMjYwNzE0MjIxNzQ5WhgPMjA1NjA3MDYyMj"
    "E3NDlaMDcxCzAJBgNVBAYTAkNOMQ8wDQYDVQQKDAZhaV9zZGsxFzAVBgNVBAMMDm1jcC50ZXN0LmxvY2FsMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8A"
    "MIIBCgKCAQEAiNdtHt7SHywNWT2cjFTQoIO8esRW"
    "y0Mw//LbQelKqvbs0fALYMrdFQv10lojkwY6R1zvSa8ol9EAILdHymzfRocEQ2kz2RjbK8/"
    "dsFMkBA6CXv9KkAtktpge1dm42CKymq4WXV97NG29va6f+d8jwhSAeDzvxUqEj3oTNs1LrbUfqXwUqpBfVI+zH1cpOvuo6iXNBKpv/chVyeu/"
    "Sm6MutZv3lcUd3BxRF3eEOHJZW9yTDOzRDA1NQUrIUdHS2iMFlHp7vbEOZsanXMFEBPa0uVyuqldOeDYGyVggYmxMcxG4I64LQLQs9TV0PBoDHfI75"
    "/"
    "6tl5iB0HvK6CIt0ghBQIDAQABo4GRMIGOMAwGA1UdEwEB/wQCMAAwDgYDVR0PAQH/"
    "BAQDAgWgMBMGA1UdJQQMMAoGCCsGAQUFBwMBMBkGA1UdEQQSMBCCDm1jcC50ZXN0LmxvY2FsMB0GA1UdDgQWBBSioINDtPv3E8TBKKgU0W/"
    "Db85S0TAfBgNVHSMEGDAWgBRWtAKBlEFEMPv9rob2Yv2zqqdjUjANBgkqhkiG9w0BAQsFAAOCAQEAouhQhV+"
    "qO6GtK3hkUIFiCwl9tIlzKRpbzZ8l8gKO+puU4NerOy4rijE/Xf97/"
    "x40ujhr2NE6Mdm2cIiEZOFCGDhitAsLOAhgbGgZvDlaXhiu1wRmye/+Xp6rZmDJCcqWsTr49hmXNmow3tG3y/uifmi8IDh3GOQfozLEy/"
    "f3BE1AGKPYctTJjj+jW5GUzl6Y9Unxtbr7JJG8vqAJd0XffOgljAMYJU5lUCa0+"
    "NmDjXmu0wIAgPtJCjjZjjw4vfXhP9tUv10e1bFuVCg2GodGnDgUWK9Mdry2N8PPgLy2/"
    "UENA+5ab408M0nBF8zJhHIsTqtel+zCdvjJvApnIaYctw==";

// 与根签名证书配对的 PKCS#8 私钥仅用于本进程回环握手。
inline constexpr std::string_view kMcpTlsRootSignedServerPrivateKeyPkcs8Base64 =
    "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCI120e3tIfLA1ZPZyMVNCgg7x6xFbLQzD/8ttB6Uqq9uzR8Atgyt0VC/"
    "XSWiOTBjpHXO9JryiX0QAgt0fKbN9GhwRDaTPZGNsrz92wUyQEDoJe/0qQC2S2mB7V2bjYIrKarhZdX3s0bb29rp/53yPCFIB4PO/"
    "FSoSPehM2zUuttR+pfBSqkF9Uj7MfVyk6+6jqJc0Eqm/"
    "9yFXJ679Kboy61m/"
    "eVxR3cHFEXd4Q4cllb3JMM7NEMDU1BSshR0dLaIwWUenu9sQ5mxqdcwUQE9rS5XK6qV054NgbJWCBibExzEbgjrgtAtCz1NXQ8GgMd8jvn/"
    "q2XmIHQe8roIi3SCEFAgMBAAECggEAAIDVjvljDK5AV43wnpGxYRw4ojVVEsPenVXKGYvJ+pUIIr5gqasP/o/"
    "83WG6rKszgQyWwiEFI8sg6K6nr4f4ETJ0EFikP90fnBVIN79KuFroo4CQI9oYx5sJkWwzWSPDJa3dahijlcNS/"
    "KBWPDudKZmWarMrDYcyUiIAKGw5Sp9qUECOXR+PYrwtwireqefP32Pd9FUx/"
    "2sOjMYXuRUe9Akf6B5C7NQsYYOEuTSfHCIfekuizUwaR1L3WeVUoIFQN4kLel1f3n/ASBaTv/8aoWhBn5BEmQAV1KNNFtyWU45e+SBl3GT/"
    "MmtbzyEODRJ5pzD5GjO6P43ni3yLfm43cQKBgQDA46cTK1Agi1UU79VYPROP3E0Gm0TUBQ62N3CFTlFsNCT6vYkJxP3IXvg9TRtoTRq2fQPfUxDEwJ"
    "+"
    "DNJTr5CmfjnIC1bUbHuUr243F4O6WsoGloZxX9kjb/tFPZBORWRag2bmoABwY0a7UOpG0nkmUMckITqU+ygYKm088ndxImQKBgQC1nTrl+Byg/46b/"
    "z764aJVboZYssczyXpb5t7xuasjF1BVVbupShO0YX7eB4/tBrxGbcePLSI3YSUifrL3DeMxU1/"
    "6ItiQbiM8pBp5kvBnXgm9dVG8hrfz22Xus3CQOuYyj1o/"
    "gloXKPL1QmdCbk3oHcDJDHv9vWNJ770TBeCDTQKBgH5wvISET/IFY7BlxqQg5UHYV2WNQoD5D7vCe6/"
    "mttkHFXaH59zPlMxT9MW6Vcz5PhmBuZgbC1LuMaIARd6boe1FotbY3+73QaKOOVENMVj3iqTYW3QNrZUIZIx2PZ15mKu4DUtjRwe966yS+"
    "BA98l2ChRU8+"
    "HVAWNV7GWXS1SOBAoGBAJc0xOja3dsdanpu1NzEJwfB8ZC/qCJxyLHjUhMnZljc/"
    "EFQE+IqjqVL1vi8ixPo27A3jkKibS52bxh9LDuNEG7s36e+g1gcmrHOX0yBDi7BmJJorxirgVRCgmdudnVe1HGb8KOXv7IwobeZBafFdo+"
    "e1feNsV3c5GGWfHDuH7KxAoGAClcuQa9jS1igZIt2o3/"
    "XEFAOwy6rvO3LoMIcHj5776Z3bnkuaZFCqr+W4lRc4Nd7oBKHr27bPqzgvDr/"
    "T39QMYDngNzYPHpOcZu1bGAEG3RHuEtKGTvjvkq0br0Hl9CS3lw14bDaAPxulQ4WKS4rFuGlO/XlguNpWYlGQyFQ8qU=";

// 自签名 Server 证书使用同一唯一 SAN，使拒绝变量只剩证书链不受信。
inline constexpr std::string_view kMcpTlsSelfSignedServerCertificateDerBase64 =
    "MIIDkDCCAnigAwIBAgIUfxaKufI4VDR2HZhfF+"
    "GbJe7oNVEwDQYJKoZIhvcNAQELBQAwNzELMAkGA1UEBhMCQ04xDzANBgNVBAoMBmFpX3NkazEXMBUGA1UEAwwObWNwLnRlc3QubG9jYWwwIBcNMjYw"
    "NzE0MjIxNzU0WhgPMjA1NjA3MDYyMjE3NTRaMDcx"
    "CzAJBgNVBAYTAkNOMQ8wDQYDVQQKDAZhaV9zZGsxFzAVBgNVBAMMDm1jcC50ZXN0LmxvY2FsMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQ"
    "EA4/"
    "4qDE4KEwcG+iHUF4o3qWHTuTKJhUpMqjtChNKEMYh/THF/CfGgBjwIJwYyqoNGwRjtfx1c3PiJ/i91prK0kCj6xbtLz6rUgzUwI//"
    "pmIoys7K8vR1dCyqi7oyKYt/k8DYHveVeTKB7c5/"
    "33znrO4onoibLPccyY53mzwuxPREEPI0P8gBlKj//XnDwnv+7Qz7lZeQYhP9VkmhMx3aMp0iEO8zvt/"
    "87QMjw+HvgYBZ0l3AnJ5hlCJsuOgbQl8XwmiMrpDjZ/4VbPgnjnNg7Sm/"
    "q96bege3JC8Tcsnm8q/"
    "rEC+WEsiFahoczszuLgMZauqXjd8b7ogiegAR+"
    "JpkwqwIDAQABo4GRMIGOMB8GA1UdIwQYMBaAFB4F6BMcNQRG8Vkvw3bdQfaPRUtDMAwGA1UdEwEB/wQCMAAwDgYDVR0PAQH/"
    "BAQDAgWgMBMGA1UdJQQMMAoGCCsGAQUFBwMBMBkGA1UdEQQSMBCCDm1jcC50ZXN0LmxvY2FsMB0GA1UdDgQWBBQeBegTHDUERvFZL8N23UH2j0VLQz"
    "ANBgkqhkiG9w0BAQsFAAOCAQEAPVQsQHxeYNx7D5"
    "FaAUwo1nI25Fpb1xx3HtFriR17qF10apkmpd1o3AJufDEc7R4RLQz9o9GrN7/U5W0eH/"
    "uvNsEtu0j1AvnomCkzK8fIZHJ5rWJqVC0UJd9zQXHpjOcfhi+vlyevUqgsHgi5Gk0ZyN3/bKqt/"
    "c24gl7Vp2hC2syr0MBQ4v82qpO4LbJFnDIh8iwToplQnOtPGqUDyJ6Z7S4fRrsfbp7izCBgu0e54hFdbAdB+/Fi9z8Rnf6dJKtVPUB/3R/"
    "77moUcdnbXW0RXRs4eDUtW4S1rc2HxApU1B4g6FYjddf1am/"
    "cpu/IN+2wTo1hw6hQM7P82SrSPfy+bg==";

// 与自签名证书配对的私钥只服务于“不注入测试根 CA”失败对照。
inline constexpr std::string_view kMcpTlsSelfSignedServerPrivateKeyPkcs8Base64 =
    "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDj/"
    "ioMTgoTBwb6IdQXijepYdO5MomFSkyqO0KE0oQxiH9McX8J8aAGPAgnBjKqg0bBGO1/HVzc+In+L3WmsrSQKPrFu0vPqtSDNTAj/"
    "+mYijKzsry9HV0LKqLujIpi3+TwNge95V5MoHtzn/ffOes7iieiJss9xzJjnebPC7E9EQQ8jQ/yAGUqP/9ecPCe/7tDPuVl5BiE/"
    "1WSaEzHdoynSIQ7zO+3/"
    "ztAyPD4e+BgFnSXcCcnmGUImy46BtCXxfCaIyukONn/"
    "hVs+CeOc2DtKb+r3pt6B7ckLxNyyebyr+"
    "sQL5YSyIVqGhzOzO4uAxlq6peN3xvuiCJ6ABH4mmTCrAgMBAAECggEABUpXASyfvcEnyjlrZVnF6UcDhIAn6KI3mVWBOXD9GknxSblp0XGV1K80U5/"
    "CD3vH8jEfa9OeqlrIMPI6ttyIclgu9LIs6U3XbCkQ7egfLGOzweLINkLugGGhwujF8/VDyfqecm/N5Rn2nx4qayagcRLpjKhsU03s/"
    "1Zc2w8SMqgumipgeCPnS7wPU83AQ63WDFtjygRjk2JXZVi3Yfr4LZG/SSp1i6IJ7iLF5mmU1GGrTROi3wYcqrx/ILDQWLsDjRRVTXO/"
    "5WKsqQ+NwAQXjGSi5DK1mAHALo68neGi3Zq5FKYRxMJAy3qblDKht9N+cxIbbn1yH+zXOcJUuQE7wQKBgQD9qCutRqhMESmamwBWg+"
    "cCibSyFm9YUwkAG8SzvoBdCjPkskI70AQutS2MDUftcJ/XduXc/"
    "FiJNdfxgJqAW42LCLpqS/K4QchuDENaVW+1OUixtUBbJCrPBda1ktWNw4Latya2lSZ4t06mlIzmo7sYDhTFsDX3Ks/"
    "Y2lsXiqyVCwKBgQDmGU4ZWryDJUtAy5uj/"
    "8lBRaA3lcUJ8Zpg9owWRliu2uszyr5Up7vN552K/QtiGiBgTzbs5gfrwFdX0WSv3JOFrsmCQOZMjiitw3/U6dxB+d5od6Eehbk/"
    "nnt5C8HR6Qrs1ld8h+2iy2Hf4tGbAu39gOsD2C93t08XSr46PtnW4QKBgBswmKX1SfsFZ/FDpjCf6PZTroPcdckA9ZkGYPpndDkE6/UaypDAxoH/"
    "N8eocMpZ5ThibVdX0WG73M7tWyJhLfX2VI0qYPUxT/"
    "vD4sBfIh9J6anq5OAgK1qPpKlH+Q4+I9uwAD1YFYyhEZQ3njbm3BpDACUEs3QwKaF0HomSetdrAoGBALu+D/"
    "QuDbwv1Lv0pXRlgu9oAPb5k8CuShktcVXHZhqdcuVaWnu2NE0n22qJVx1fVeVfHOCkO/"
    "hGgr2bkzzOi+u6lp5Zl88E+vn0lrvBaL7IQokYevTthR38kKBOUIGWVRyzMsTgH6wBqEVFy2JgPJDn/"
    "FmxreFPRxApIxdhKtrhAoGBAMjBABNV526x9Foa+q437du/"
    "gTNEAwPB5nJRd5qVXRcVjzNdFiY4RnrtX7R21JQOR1mIhE79WoAKZuywHoe1Oa2vMjQUf4bCBNDK5a8BWPHBT4C13YCXW53x2vxEF55j2YVxLxqkDl"
    "vETH1KsWDgy7M0CY39b5HtWjs5QXUqKz2+";

// Windows 根签名 PFX 含 Server 私钥、Server 证书和公开根证书链。
// 它由 PFXImportCertStore 以 PKCS12_NO_PERSIST_KEY 导入临时内存 Store。
inline constexpr std::string_view kMcpTlsRootSignedServerPkcs12Base64 =
    "MIINgQIBAzCCDUcGCSqGSIb3DQEHAaCCDTgEgg00MIINMDCCB+"
    "cGCSqGSIb3DQEHBqCCB9gwggfUAgEAMIIHzQYJKoZIhvcNAQcBMBwGCiqGSIb3DQEMAQMwDgQIENENb42w+"
    "o8CAggAgIIHoC7oi6QQmXLQtVxD4fI40358OXnrrYGc/"
    "+nsb2kVzVx87okdwemRMsccwD9HN7v4DWumifrh2rns6q+B1sv1funnyWzDspnKkkpw0dgaM/QVFN8ykikxVZ+0e1nef3NFwnj3dk/"
    "oE4HBbgs6G9GOOEck6L4qgfzk8zfq8s2uQUwCuie+"
    "0539ypaxeUe6TpQuAypgZj4WcQqQGNArhbn7HbbmnEpvpMowMXGVDar4AvV2qDofLN46NzBlSbEdr9ce1ZpxhExzwp2l1RVWDPAzh/"
    "w8yRx6PldgL7gzjKR6CSFbWbVUqDttXCdV850sJ6ZRsDeokmJAwJFsoOxeQrW0Wr0GkZndT5r1CexJoZqaqgmL5ySBFapcxc6sVH6DJBzQ+I/"
    "B4oZWtvczhoneDdBCNjIS9j7ARzLgQBI+w0Z0R213aUOKv4ghsJq94vR8PhtSVBNW+NuG6cU+"
    "69Ca0cZxESj5gmEtjQ8xZ8ni2xTGTXwFE3rNjQCKedEBO24qPQ5HavIf9uQNY89jeKSBL9LryqPBQHqjHwMX3d44BcbnQrcMvZchhLI0pNj9wvJc/"
    "OtrycxqjfCLn7FmurmNo4If7ZWHWH79tXQLHy75Xu9AJm1gh096Q7ZkZ88cZ6DCDGKp+i4/"
    "wqr1BNb701jhXg87XqS09NgE6WJqPGh8uKrU+5F32GO9BaalOqi2PoiFAYQzGBWwnhuUG+l9lNnhL2/"
    "maKqrD7t6EUmuMvuYseget3u92cCVJW6g9JCuKPl7wpJZhrydw26msOSvsKAbtGALSB7g19YU0fQLVVgeUEefVG/ILMaLDOKt2v7+p+V/"
    "auReSW+0bncZVdrYlznh5pG6cxscHuQBA9/"
    "KZ3PLlTaOl4fndwIOCpsRf72w0jbHC3eQyI2nxQ0GVMnXc0y6bX2SNqk4tPtberomyAbo6rU8Svrk5IvLiCHF1cfpiJj67Zw5NpRKBI1qbuqymy5u9"
    "dxWAvLeofyAOgmBDlTwCt7HaelyOGqJzmB5WZzMo"
    "nu/VUlTM5MRqyFJN4mWDsgdo/"
    "xGBEX2SkexPtn5XLcvGH2dQwXP+4GuS7zPVhYMcigNjU32c1Y2khcE8SFDPwF5oXO43LkEYBSedS6zhzmP8sopGwNycIgGbxk6gdNegT/eK/"
    "h1OsER1Mim6S7CTrTqfHa1bwqof0/jL6CuYrIU1f6zdOat0Nz6za0f6Yduv1l5ophHSaJo40FAf/RpEGtD44R9AM9upcxAa5rRxdLOm1i5/"
    "ZepJ+2XqV7OWCpldFPfedIUPdv2xDvnuiOIgVSNvWBWQzLi/KoTBlrh0MkL67Bigjvh/"
    "PGOZ97I4AdXx8Spl3kWn5RMegVv+k5Y+OCGPDz++9iSPxifp/WwRjSeUGTzs6/0HzH5XlYjvLika6oi/otzcd/"
    "4L+AcvJudrVp+qFLoXAResnw/ixPKUZVilu3gULDTVCXieIQVhJL9SCj4kBfFq+nGl/"
    "SWPbAePKH1n5FtfwE3FsslDwwbQOFuzGD8giGQZE7UU5vf0KIXaoqPxAlazIqNZTV/"
    "RGvJ4mHq0T8iFFviOQxHDZyd98azZykHtON9V32Xck7c9hDkfyc1oD6ujmUJ5VpV6hY76DJx9u0fJq7txe60UU6wz2XbwpSGlF9lNRzrx++6QL/"
    "i5mO2OVURT+f0TX+Xb9FwOwQqdFq92HhSAWKgvy6+Y7s+"
    "F9FZMDozydm0MfBLZsrTRiffJWw9ydO2CJ7uVh6jRpSsHlfpVWD11Jf3dtsPW2NL456oiwH7IM5Zg8HDgJ+LhiLNr+"
    "T2DejZFW3dIoJCiJQBWlg8LIJuctT3UfDr8y3Iz4n9KCNNLq0Ona25byA9FPsfW3XrMk3AYTHfyKQqAEgx2OEpOFN/8m0lL/"
    "T9zP03KfDIYQooL8LtmdB80n/"
    "dQb0Hbt3981bhuunsqcJRVRWA3u9Ziwb1X0LD+VZpIm+BAqfKTf+HKkKWVULMLtas4Fzh7OikLPxhuvS1JQHB5ktPWxCIoBGu73zPz+J+"
    "Ihshl0Jmy4R6kDtmr3Keu8DaK3tbDZ6gsH/"
    "MeA8o2brJcrnHj/ZdFoths916BljE39kt9zjvinEZ/v7keUDGHr9PjSIk7LxvpiOOjovgE0JUKrzk/g5k6C5HMYfzeOC/"
    "c9TX9eCVXvHyNjZ4Q8rDzM0GD/+GUzxLNHzPobon+q80/"
    "Qr6UoN72AhEaZIBq27VqS51gelqrXxHOiEl5u97bCVZd9cBgA0eq8c154capaOLt8XvmIRu6W5MeAcRocPX6QXaNCDH1Ur4O+"
    "4NwuhnzQP0nXKrpg5jV2kUm5vB0X8vXg6olCJLmapVE+"
    "XQi9GFhk58QUhAebLmefWdKpVz1NHdLvsHw+2iFsaaUSNsGruODFScoCRgeTBeo3EcMeP9nH2sbDI2+"
    "v7akwUSMFvSAL7K3x9sZzbVgnQBKaCo2S3wkE15i35WrCoapaw6/"
    "NxhR5I2vxHONcNWzwqswTguKJSORzoQ8HAdkIL8Sr0qGOou1Chwm+XrP+LBJBzYMIQm/m4pZg3T0agNHYNoRL/4E/"
    "Sw5Cy1xSpWp7NGKU44vDIMR+"
    "dyjDtJt709sNPatGbdVyouKZRk8XT9MIIFQQYJKoZIhvcNAQcBoIIFMgSCBS4wggUqMIIFJgYLKoZIhvcNAQwKAQKgggTuMIIE6jAcBgoqhkiG9w0B"
    "DAEDMA4ECLRprhkiNYsXAgIIAASCBMhmjHRsTQsR"
    "6f0/aQYZjqVHKSjrfwYekbVvM+m6xnoN/29Ds8di0AtizUut+HKYiM7D0/KEQVCjM6ucfkm7aZY70EphK3jZ4TirPQTXHCveDlNoN6lqSeylB6XBL/"
    "yPCkuy+XOUCXwWOrxIx1sDbhblFuHldixIofsThl+"
    "f5J0c9WA9kHe3zjVO8eZtl2tAEtODpbQ021HbtOS3Z8plpS8NAOUjcGrwlLLBkM57DE7OVEdXPR2IrYzxvH1CgBQEtK2e4sehSufM3AAXvBeZMXhPq"
    "36BSntEw5Oz3Bay1OfmRZInF1rt58n2//"
    "RwORSIZ7iL00rigp5vNBxpdxKx8+WwMXrKCiGvptd4EKffMbUVa6YIVhHVpI8d0WvndUmyi/KdwT8jGxK2SQ9xOADVya17RdPEDuXgxSN/"
    "61GJgmimvFIOZbORbQd3aQzd4w+/"
    "oDG+HY60q+wjeZQJiChMD/e19CIqlZgAYxl9CrbkS4j28j6APzQr5Js9iHxhwsAwddUkUMlMce8xTFa5LsaGjXzOORZikzzIjANuqfGNkPKQoo/"
    "prQB7H4IILTjCmsJw5906AhXWoKN/"
    "4rRbMMmBGE2d9oBVtLtVOmLNhM+"
    "LcbVRkUDVbksFuDoi1l0o0TlyB9tvCjM9eJANKvtxsVUB2r5wbjaDlEyAbLgXHu3v6152GR15q3Ik88ZC1U4FY2K85P+TucI+vHdvJfSfV/"
    "aWHavCzVJnIbB51ylUo9X88tVKDpSXqNc9wBSr2K7mTyF4KBszS6yv3e3bgnrqlhiub6CD1zf1ye8assodj8lZSvd8n+GVSmIxzlIgRNFtx+i/"
    "l8OU6zLJXoI/"
    "eGBIpL+hoqrzM889W4swS6chCGpE1EncGsxIQWIsSaVHjewq0gUJx9K7fxfkcgl+"
    "9jtoTU4fNTpMbvByij9zXggogj1kfXXmkEru5YIwO07Z5yYBZtN9zjTeODHTrOB0dqm8Bn7Kp/"
    "s5UXzkKJxi6rDWWadWMzqAKlNtTs8nTc5RFOgQyMXN00K2hht7ldE1Inb8l8v3r+"
    "BqzvhIrOIEKht1wzR5pfP5KXhKJ8Qy3OdQqGC6wFMmitiT4Z9zYxHW/"
    "h293aRIrTJ+MWeMN+"
    "C2gbvOBvw9vjMaYLAcUTT3LX6PkFvHXlkAEs7AECpXhWOzxdUmH6llWbLVtaUQ6jHf360jBzmDKbeHKcXgbsd6gTnMXmzbg4vD30si5Tptpwd4V64g"
    "utx9HZMieIV8Z6rhyr077lspfdHtRFzUWAfE0r5h"
    "Sq/+ZBENFYIA5VHB9OnqKgiK+kYBaIDP1z3QQhwP6MHtg9DopPj8otDiS5qhuvOwQxYUTtqdOJXU+PwUH/"
    "2Osy4066BKKr855+9XPkFUMc60Npr8jAiUB6MXtHX33M0fJqpla2v63HKA+kAo9y542jyUid3qvfZ/"
    "6riJKY4FT2STLKmy9T4h8fEnkSk3eFTO1kAfeu7gg9FKH8E8pe04vz8r1IUTC3sohSPiGGKzZEf5HR2p4TSmH5h846zwcczmKKpRcYbIHOJXhbFWyP"
    "K4aL9gZph2LERRdPEcpVrAZsJ0da6gUg6x0y5sy2"
    "33KCYT1/"
    "WUiv2aadnbUowmkD50e6ujKxaXA3nlc0WDs+VLVg4xJTAjBgkqhkiG9w0BCRUxFgQUpE6aQCZBq+Qi71+"
    "up2bwIj0UbQkwMTAhMAkGBSsOAwIaBQAEFLcaJ9Gnz4HE37z/"
    "ztlAxICuiyUbBAi0l09/xzfe2AICCAA=";

// Windows 自签名 PFX 不包含测试根，确保证书链拒绝用例不会被意外补链。
inline constexpr std::string_view kMcpTlsSelfSignedServerPkcs12Base64 =
    "MIIJ0QIBAzCCCZcGCSqGSIb3DQEHAaCCCYgEggmEMIIJgDCCBDcGCSqGSIb3DQEHBqCCBCgwggQkAgEAMIIEHQYJKoZIhvcNAQcBMBwGCiqGSIb3DQ"
    "EMAQMwDgQILjWwHic+"
    "XPgCAggAgIID8EaM3I14YOOWr2QFTYGBNuimgYersxc6H2fBw3TIg+"
    "RJNgEerOImJwHYftIZ5OrtD4n56IfJUqXJKQV9upRQtl0TDftQI1nEy7VwoVxFoUnPHVVBQ/"
    "0D5QAAy9evCWw6kit9XsrHT1WsSGURAhb+5uVzTO7rNJ9Utee0A/9CNE/4e8yMaInlFPtKlp1VwSvEdonE/"
    "37hhUYEiKwFhQdqufm8RxumFtjVeIHIcvM2PE88Pvm2Pfx9mI1IOZ8N9nWKt/"
    "LKBKLWUa2beCfNP2VUhEe++dKf3fjrtJ3dHKSIaNaaKX+iWMEqzyiCLBXPflVtGK9WSd5BHhNC8N+1oL+fBJQyY/"
    "oHMt+I3RqnomKGtgFCmzDXx21zZYwd37y6K4R15MfUkzwh5Py10vbGB3JCO5pgCN1KfE2TRT/"
    "FkLeCMbtOV+aFGQ4jFwKIZg30n77ycLoUpVHNiUxc+gTTXJ7HnnfzgrXtj0fEpSVPxtRunPwOSXM/"
    "qgsS9pP00+C25uRgc9tsonpp2ftBsUHmEY4mhEYMccNCg6x6t5T0i76veV1GhSRbHrlWJvcpNdM0F0meGkZ3+"
    "Udk0HqPFXGzErDmPgmHp8KVnRYjOuI4oiQBW6p/"
    "CvPgJGkL8ypFqGwAkmW+y4m9wZxor4QeoakOcG2aLwSWrbRJ46KzKsRHDxQDRYMwQsJ3o/"
    "A7oGdxHlXlJNYAY6jCjx+RS9NM5s5rtcQxgxrZQXmBfNC+QORP8ZfqJSUCu+ufvL3CtrM1lTotb5/"
    "XZaWbq9UT/lI6M+4pMk2wb8MT+Wmpd5PW/8zIH1dYnZEauewH9VVFPspdhm/XQc4VizYzFha9kHqrQV5vB9Jo6z8zn6yY/"
    "8dglAKRPeh15Y3oxffXQISG7uzC50GrHSWsEO0uzTMt50qWJB4OVVeDMXpD6CkN5fl5Vr+S3+QCbicIVA3vnywVjmUfRYwXWmaJtU+"
    "qJxTir6pSAhL32rlGeKoaAvYqmB5aJfi9Ieja4Yp00hLZiK2NA5tyYS3RtGnW+vEK0jeIl1GEYIJXQqKRjnUTp7jpS8CQ+cS8ZIrwGdNXCdeW3e/"
    "18lfqCDeU4r88rPJUY1kTxsJZsahSxc7pZjaAIYTnai4b6wi9Em74U1ibg8GOvs4cCs9IozqwdjM8lmEHpS6PLkYlLh7q998a8UNI2WcYUxyrRDuKH"
    "0UyrhFNlr5Ofceb21e3V0Zag94P92Wcuq0GUEORa"
    "sIDTSrNFiQ2y0/zo3mvc0Kv5fp4mv//"
    "MujqPrhIfuL0zt5RFhb4jyc8PD4u9tK2MykLxyal8aNLpeTMjScRNYGpwS+"
    "qBjPO1mNYOwRp9IU0OFt4kXrgdDCCBUEGCSqGSIb3DQEHAaCCBTIEggUuMIIFKjCCBSYGCyqGSIb3DQEMCgECoIIE7jCCBOowHAYKKoZIhvcNAQwBA"
    "zAOBAhih8O1KaZY1QICCAAEggTI84W5fMTXrtB+"
    "DJhW5inUeJOx52Sq8kByRGK5bELlF34ErIIbWHY3LJT7YfrHy2Y4ydI/k8/"
    "Z38gbOfNinBHDYqCdmNobYM2LyA4fyJGwF4xmleArkKTVMnsyCHoRZ7lAoS+6gHdax8Plx3oLiBbeFgi7iv/"
    "lRsfFUocOh4R5VWQ/grlikDRwgsPrNbypxgIE8uz7HmZ9voiPgP8byPFa2fT7fTkIS88HAKqMMEfOQuNkJQhC13EuoZFPQm//zaYq431diV2iumd/"
    "TKP7RPf2TLZcqEMLog5qLu27tohs0wHcroyEZHl8GB8wtcdi+6oNKloNHWGrF+"
    "zyyzg2OcCe8k6qYwshiCXcFI2ojPqTTKOv2MKGNh58tgDyMBWlvSNDxQwUQ79hMre8+ClBk0Ah9Mf+"
    "g63geHXHA2agmg8VWXLPs8WDNB1GnrpF+Zooau2XqabVIj6bVGMQYxT8c4OkCJrlV3FzDiHecwdmC+OI66nnmqHK9cWIZ30rCOHZdq7oafYH/ZdtT/"
    "h5UFiYjN82xKbG/nhVgKT/"
    "tUTWeFVZo4LHY9MRK4PiwSDKyqAKVrJBpkQbJHvBT0L9sOEBUeDEWF+"
    "wyPxajzHUWEWyOEUJurLRp7y6a8Td9m52CQKTjNnhI8XwIuCEmAyFamNMECWLiLbjifWUSJHmrZI243r42463VhAdCSjMmZwbtD8huKnYGqnQ45F+"
    "ksTZj9uzAzuFbLmywNVkEF98L3r8bga6XwRnsdxv1i9oiB+KwzJjV8WQ+42Ft5GdKZxrE20jAicjPxi6hQefIQq52pPY+"
    "iOIdrQJPdShto6mZA9ZZb5ZDUWqC35514h0mMtrh/"
    "LjV5RRfIwi6wqOoGZMgMsVKaY6An/eFyx6ayNlefSMsIvLIZlKcDUgtT3KsXfoNLnzsyxbKqoJY3ClHxUPr1W9aMhfJ9wrTx65jDrF/"
    "lxjHDbBZ9i2wTsqglrkpFfHxF5Kvhde2YTwUQo5jwY7HCdbHMj7wQhXKGNW9MEOlRLgpaBPmKNeFKluWy3904q4l4xxb3C3Mq4eYRPe3S5REUI2yp/"
    "EOWy1D7mc93T8xDAg0mpbQlQYzYdqFtBrz0T4BZZNZsTR9CWywcfmcBXwFi44Jq9PAk1iKciCvP1mD4fqYPORtWHzmY0e8pUa5BFbu/"
    "+8Kh2geNyFemRNdraaz2hdRb/"
    "1DtugIUKOW0Ccs4S5FMlHr7trt6FKU8FkbC7jfNZ4aO4whOIcG3KLFd9yoI+Bk6HlvnZg45blydo32iz/"
    "kFNpfRcXoDJnUuSwrCgmir+bGg5eAif62lb++6vbvMG5Fo8ggZ/"
    "8fFDQpNzkrpfdcsReFKukSp+ASQeWOmUWuMhYHxzsWTGx0Bu++Khuxu810il8FuTo3MxeBfDB280wpMkJlpsmWIV9Kc6yuKN7Kk/"
    "jsa9fKBairPTIjNbzgsCDb2XU+m4xYHlwwo4A9C9H0r2CA8Xh9rcv714hcW8XSgtVJhK+"
    "fHpiQfV7IfQU09oYCkdrPAubC4MukcWm5SBFpNLVi7TUpvNj/3pG3fXENXB1qtJVtsoWIPGkEtG7J3/"
    "4zzGHcVsLbDTaSilyh3o6/cEsDnusqPapIVyWodcK1b2L/vXzX3BqN5k748p/"
    "MSUwIwYJKoZIhvcNAQkVMRYEFIewdV7J3ufP80GbTouL3VTuqqJbMDEwITAJBgUrDgMCGgUABBS0PCuxVrfdGAOv/"
    "aFM+xYAc5/YRwQIPYajdNjBE9QCAggA";

// 返回值引用静态只读资源，Server 不拥有或擦写证书字节。
inline const McpTlsCertificateResource& mcpTlsCertificateResource(McpTlsCertificateMode mode) noexcept {
    static constexpr McpTlsCertificateResource root_signed{kMcpTlsRootSignedServerCertificateDerBase64,
                                                           kMcpTlsRootSignedServerPrivateKeyPkcs8Base64,
                                                           kMcpTlsRootSignedServerPkcs12Base64};
    static constexpr McpTlsCertificateResource self_signed{kMcpTlsSelfSignedServerCertificateDerBase64,
                                                           kMcpTlsSelfSignedServerPrivateKeyPkcs8Base64,
                                                           kMcpTlsSelfSignedServerPkcs12Base64};
    return mode == McpTlsCertificateMode::RootSigned ? root_signed : self_signed;
}

// 测试 HTTP Backend 需要 PEM 形式的 CA Blob；这里只做确定性 ASCII 包装。
// 换行宽度固定为 64，兼容 libcurl/OpenSSL 与 Windows 测试诊断工具。
inline std::string mcpTlsTestRootCaPem() {
    std::string pem = "-----BEGIN CERTIFICATE-----\n";
    for(std::size_t offset = 0U; offset < kMcpTlsRootCaDerBase64.size(); offset += 64U) {
        pem.append(kMcpTlsRootCaDerBase64.substr(offset, 64U));
        pem.push_back('\n');
    }
    pem.append("-----END CERTIFICATE-----\n");
    return pem;
}

}  // namespace aiSDK::test
