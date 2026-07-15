#pragma once

#include <memory>

#include "mcp/IMCPTransport.h"
#include "mcp/MCPServerConfig.h"

namespace aiSDK::detail {

// 工厂把平台进程与 HTTP 库封闭在实现目标内部，公共 Client 只依赖窄传输接口。
// 两个工厂都只构造对象，不得在返回前启动子进程、建连或调用凭据 Provider。
// config 和 limits 按值进入具体实现，调用方随后修改原对象不会改变传输合同。
std::shared_ptr<IMCPTransport> createStdioMCPTransport(const MCPStdioServerConfig& config,
                                                       const MCPCommonLimits& limits);
// HTTP 工厂沿用生产 TLS、空代理和禁重定向策略；测试覆盖走独立内部工厂。
std::shared_ptr<IMCPTransport> createStreamableHttpMCPTransport(const MCPStreamableHttpConfig& config,
                                                                const MCPCommonLimits& limits);

}  // namespace aiSDK::detail
