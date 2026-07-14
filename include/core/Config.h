#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

namespace aiSDK {

// ProviderConfig 保存单个模型提供方的最小连接参数。
// 当前阶段聚焦鉴权、基础地址和默认模型，便于多个 Provider 共享同一套配置结构。
struct ProviderConfig {
    // api_key 支持直接写值，也支持通过环境变量或 .env 占位注入。
    std::string api_key;
    // base_url 保存服务根地址，不包含具体接口路径。
    std::string base_url;
    // default_model 用于请求未显式指定模型时的兜底选择。
    std::string default_model;
};

// Config 是 AIClient 的顶层运行配置。
// 它统一承载 Provider 选择、超时和可观测性等跨模块参数。
struct Config {
    // providers 以 provider 名称为键，集中管理多模型接入配置。
    std::unordered_map<std::string, ProviderConfig> providers;
    // default_provider 决定 AIClient 初始化后的默认 Provider。
    std::string default_provider = "deepseek";
    // timeout_ms 会传递到网络层，统一控制请求超时。
    int timeout_ms = 30000;
    // enable_trace 控制 AIClient 是否允许创建和写入显式 TraceSession。
    // 关闭后即使传入外部有效会话，当前客户端也不会追加步骤。
    bool enable_trace = false;
};

// getEnvValue 读取单个环境变量。
// 该函数统一封装平台差异，避免调用方直接依赖 CRT 的环境变量接口。
std::string getEnvValue(const std::string& key);

// loadEnvFile 从指定 .env 文件加载键值对到当前进程环境变量中。
// 默认不覆盖已有环境变量；文件不存在时返回 false，而不是抛异常。
bool loadEnvFile(const std::filesystem::path& path, bool overwrite = false);

// loadNearestEnvFile 从起始目录向上查找指定文件名，找到后立即加载。
// 这适合示例程序和本地测试在任意子目录启动时自动发现仓库根目录的 .env。
bool loadNearestEnvFile(const std::filesystem::path& start_directory, const std::string& filename = ".env", bool overwrite = false);

// resolveEnvPlaceholders 解析 ${KEY} 形式的占位符。
// 未找到环境变量时会替换为空字符串，保持配置加载流程可继续执行。
std::string resolveEnvPlaceholders(const std::string& raw_value);

// providerConfigFromJson 把单个 provider 配置块转换为强类型结构。
ProviderConfig providerConfigFromJson(const nlohmann::json& json);

// providerConfigToJson 用于测试断言、调试输出和未来的配置导出。
nlohmann::json providerConfigToJson(const ProviderConfig& config);

// configFromJson 把顶层 JSON 配置转换为运行时 Config。
// provider 的 api_key 会在这里继续经过环境变量占位解析。
Config configFromJson(const nlohmann::json& json);

// loadConfigFromFile 直接从磁盘读取 JSON 配置文件。
// 在解析配置前，它会先尝试从配置文件所在目录向上发现并加载 .env。
Config loadConfigFromFile(const std::filesystem::path& path);

// configToJson 主要用于测试、日志和未来的配置回写场景。
nlohmann::json configToJson(const Config& config);

}  // namespace aiSDK
