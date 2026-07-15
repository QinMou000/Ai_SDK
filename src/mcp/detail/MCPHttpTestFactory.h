#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "mcp/IMCPTransport.h"
#include "mcp/MCPServerConfig.h"

namespace aiSDK {
namespace detail {

// MCPHttpTestActivityCounters 只为资源回收测试观察 Transport 内部活动资源。
// 计数器不参与生产控制流；close 返回后全部字段必须归零。
struct MCPHttpTestActivityCounters {
    std::atomic<std::size_t> active_requests{0U};
    std::atomic<std::size_t> active_sse_workers{0U};
    std::atomic<std::size_t> active_sessions{0U};
};

// MCPHttpTestOverrides 只为真实回环 TLS 测试覆盖单个 Transport 的信任根和名称解析。
// 该类型位于 src/ 内部目录，不进入安装头、公开配置或生产示例。
// ca_pem 为空时继续使用系统信任库；非空时通过当前 Session 的 CAINFO_BLOB 注入。
// resolve_entries 使用 libcurl 的 host:port:address 语法，每个 Session 都建立独立 slist。
// 覆盖项不得改变 TLS 校验、代理、重定向、Endpoint 或凭据策略。
struct MCPHttpTestOverrides {
    // 所有覆盖字段按值进入测试 Transport，测试栈对象销毁不会留下悬空引用。
    std::string ca_pem;
    std::vector<std::string> resolve_entries;
    // activity_counters 为空时不采集；非空计数也不得改变请求成败。
    std::shared_ptr<MCPHttpTestActivityCounters> activity_counters;
};

// createStreamableHttpMCPTransportForTest 复用生产 Transport 的全部请求和状态机路径。
// 工厂构造阶段仍然没有 I/O；覆盖数据在每次物理 Session 建立时按值读取。
std::shared_ptr<IMCPTransport> createStreamableHttpMCPTransportForTest(const MCPStreamableHttpConfig& config,
                                                                       const MCPCommonLimits& limits,
                                                                       MCPHttpTestOverrides overrides);

}  // namespace detail
}  // namespace aiSDK
