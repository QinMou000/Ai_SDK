#pragma once

#include <cstdint>
#include <memory>

namespace aiSDK::test {

// 夹具只绑定 127.0.0.1 和端口零，操作系统负责选择不会冲突的临时端口。
// 调用方不得把该对象或证书资源当作通用 HTTPS Server 使用。
// 证书模式只表达测试预言机需要切换的信任关系，不允许注入任意证书。
enum class McpTlsCertificateMode {
    RootSigned,
    SelfSigned,
};

// McpTlsTestServer 是仅供集成测试使用的最小 TLS MCP 回环服务。
// 平台 TLS 类型全部收敛到 Pimpl，避免测试头把 SChannel 或 OpenSSL 传播到其他目标。
class McpTlsTestServer final {
   public:
    // 构造时固定证书模式，保证一次 Server 生命周期内握手身份不会发生变化。
    explicit McpTlsTestServer(McpTlsCertificateMode certificate_mode);

    // 析构等价于幂等 stop，不会修改系统或用户证书信任库。
    ~McpTlsTestServer();

    McpTlsTestServer(const McpTlsTestServer&) = delete;
    McpTlsTestServer& operator=(const McpTlsTestServer&) = delete;
    McpTlsTestServer(McpTlsTestServer&&) = delete;
    McpTlsTestServer& operator=(McpTlsTestServer&&) = delete;

    // start 先完成证书有效期、唯一 SAN 和密钥配对校验，再绑定动态回环端口。
    // 重复 start 属于测试夹具生命周期错误，并以中文 std::logic_error 报告。
    void start();

    // 端口仅在 start 成功后非零；调用方自行用测试名称和该端口构造 HTTPS URL。
    std::uint16_t endpointPort() const noexcept;

    // stop 可重复调用；它会中断监听和活动握手，然后等待后台线程全部退出。
    void stop() noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aiSDK::test
