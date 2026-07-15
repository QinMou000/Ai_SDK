#include "core/Config.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace aiSDK {
namespace {

// trimWhitespace 去掉行首尾空白，便于解析 .env 中的 key/value。
std::string trimWhitespace(std::string text) {
    size_t begin = 0;
    while(begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }

    size_t end = text.size();
    while(end > begin && std::isspace(static_cast<unsigned char>(text[end - 1U])) != 0) {
        --end;
    }

    return text.substr(begin, end - begin);
}

// removeUtf8Bom 兼容某些编辑器保存出的 UTF-8 BOM 文件，避免首行 key 带上不可见字符。
std::string removeUtf8Bom(std::string text) {
    if(text.size() >= 3U && static_cast<unsigned char>(text[0]) == 0xEF &&
       static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        return text.substr(3);
    }

    return text;
}

// stripOptionalQuotes 去掉包裹 value 的同类型引号，保留内部原始内容。
std::string stripOptionalQuotes(std::string text) {
    if(text.size() >= 2U) {
        const char first = text.front();
        const char last = text.back();
        if((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return text.substr(1, text.size() - 2U);
        }
    }

    return text;
}

// setEnvValue 统一封装跨平台环境变量写入逻辑。
void setEnvValue(const std::string& key, const std::string& value, bool overwrite) {
    if(!overwrite && !getEnvValue(key).empty()) {
        return;
    }

#ifdef _WIN32
    if(_putenv_s(key.c_str(), value.c_str()) != 0) {
        throw std::runtime_error("写入环境变量失败: " + key);
    }
#else
    if(setenv(key.c_str(), value.c_str(), overwrite ? 1 : 0) != 0) {
        throw std::runtime_error("写入环境变量失败: " + key);
    }
#endif
}

// parseEnvAssignment 解析单行 KEY=VALUE。
// 支持可选的 export 前缀，遇到格式错误时抛异常，方便尽早暴露配置问题。
std::pair<std::string, std::string> parseEnvAssignment(const std::string& raw_line, size_t line_number) {
    std::string line = trimWhitespace(removeUtf8Bom(raw_line));
    if(line.empty() || line.front() == '#') {
        return {"", ""};
    }

    constexpr const char* kExportPrefix = "export ";
    if(line.rfind(kExportPrefix, 0) == 0) {
        line = trimWhitespace(line.substr(7));
    }

    const size_t equal_position = line.find('=');
    if(equal_position == std::string::npos) {
        throw std::runtime_error(".env 第 " + std::to_string(line_number) + " 行缺少 '='");
    }

    const std::string key = trimWhitespace(line.substr(0, equal_position));
    if(key.empty()) {
        throw std::runtime_error(".env 第 " + std::to_string(line_number) + " 行的 key 为空");
    }

    std::string value = trimWhitespace(line.substr(equal_position + 1U));
    value = stripOptionalQuotes(value);
    return {key, value};
}

}  // namespace

std::string getEnvValue(const std::string& key) {
#ifdef _WIN32
    char* value = nullptr;
    size_t size = 0;
    if(_dupenv_s(&value, &size, key.c_str()) != 0 || value == nullptr) {
        return "";
    }

    const std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(key.c_str());
    return value == nullptr ? "" : std::string(value);
#endif
}

bool loadEnvFile(const std::filesystem::path& path, bool overwrite) {
    if(!std::filesystem::exists(path)) {
        return false;
    }

    std::ifstream input(path);
    if(!input.is_open()) {
        throw std::runtime_error("无法打开 .env 文件: " + path.string());
    }

    std::string line;
    size_t line_number = 0;
    while(std::getline(input, line)) {
        ++line_number;
        const auto [key, value] = parseEnvAssignment(line, line_number);
        if(key.empty()) {
            continue;
        }

        setEnvValue(key, value, overwrite);
    }

    return true;
}

bool loadNearestEnvFile(const std::filesystem::path& start_directory, const std::string& filename, bool overwrite) {
    std::filesystem::path current = start_directory;
    if(current.empty()) {
        current = std::filesystem::current_path();
    }

    current = std::filesystem::absolute(current);
    while(true) {
        const std::filesystem::path candidate = current / filename;
        if(loadEnvFile(candidate, overwrite)) {
            return true;
        }

        if(!current.has_parent_path() || current.parent_path() == current) {
            return false;
        }

        current = current.parent_path();
    }
}

std::string resolveEnvPlaceholders(const std::string& raw_value) {
    std::string resolved;
    resolved.reserve(raw_value.size());

    // 顺序扫描字符串，支持同一个值中出现多个 ${KEY} 片段。
    for(size_t index = 0; index < raw_value.size();) {
        if(raw_value[index] == '$' && index + 1U < raw_value.size() && raw_value[index + 1U] == '{') {
            const size_t closing = raw_value.find('}', index + 2U);
            if(closing == std::string::npos) {
                // 占位符不完整时保留剩余原文，避免静默吞掉用户配置。
                resolved.append(raw_value.substr(index));
                break;
            }

            const std::string key = raw_value.substr(index + 2U, closing - index - 2U);
            resolved.append(getEnvValue(key));
            index = closing + 1U;
            continue;
        }

        resolved.push_back(raw_value[index]);
        ++index;
    }

    return resolved;
}

ProviderConfig providerConfigFromJson(const nlohmann::json& json) {
    ProviderConfig config;
    // api_key 允许来自环境变量或 .env，避免把密钥硬编码到配置文件里。
    config.api_key = resolveEnvPlaceholders(json.value("api_key", ""));
    config.base_url = json.value("base_url", "");
    config.default_model = json.value("default_model", "");
    return config;
}

nlohmann::json providerConfigToJson(const ProviderConfig& config) {
    return nlohmann::json{
        {"api_key",       config.api_key      },
        {"base_url",      config.base_url     },
        {"default_model", config.default_model},
    };
}

Config configFromJson(const nlohmann::json& json) {
    Config config;
    config.default_provider = json.value("default_provider", config.default_provider);
    config.timeout_ms = json.value("timeout_ms", config.timeout_ms);
    config.enable_trace = json.value("enable_trace", config.enable_trace);

    if(json.contains("providers")) {
        // providers 采用对象形式存储，键名就是 provider 的稳定标识。
        for(const auto& [name, provider_json] : json.at("providers").items()) {
            config.providers[name] = providerConfigFromJson(provider_json);
        }
    }

    return config;
}

Config loadConfigFromFile(const std::filesystem::path& path) {
    // 优先加载配置文件所在目录向上的 .env，方便本地把密钥与 JSON 配置分离。
    loadNearestEnvFile(path.parent_path());

    std::ifstream input(path);
    if(!input.is_open()) {
        throw std::runtime_error("failed to open config file");
    }

    nlohmann::json json;
    input >> json;
    return configFromJson(json);
}

nlohmann::json configToJson(const Config& config) {
    nlohmann::json providers = nlohmann::json::object();
    for(const auto& [name, provider] : config.providers) {
        providers[name] = providerConfigToJson(provider);
    }

    return nlohmann::json{
        {"providers",        providers              },
        {"default_provider", config.default_provider},
        {"timeout_ms",       config.timeout_ms      },
        {"enable_trace",     config.enable_trace    },
    };
}

}  // namespace aiSDK
