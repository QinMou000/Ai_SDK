#pragma once
#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <mutex>
#include <string>

namespace Util {
class Logger {
   public:
    static void initLogger(const std::string& loggerName, const std::string& loggerFile, spdlog::level::level_enum logLevel = spdlog::level::info);
    static std::shared_ptr<spdlog::logger> getInstance();

   private:
    Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

   private:
    static std::shared_ptr<spdlog::logger> _instance;
    static std::mutex _mutex;
};
// fmt 库
// incldue <format>
// string s = std::format("hello {}", "world")
#define TRACE(format, ...) Util::Logger::getInstance()->trace(std::string("[{}:{}] ") + (format), __FILE__, __LINE__, ##__VA_ARGS__)
#define DBG(format, ...) Util::Logger::getInstance()->debug(std::string("[{}:{}] ") + (format), __FILE__, __LINE__, ##__VA_ARGS__)
#define INFO(format, ...) Util::Logger::getInstance()->info(std::string("[{}:{}] ") + (format), __FILE__, __LINE__, ##__VA_ARGS__)
#define WARN(format, ...) Util::Logger::getInstance()->warn(std::string("[{}:{}] ") + (format), __FILE__, __LINE__, ##__VA_ARGS__)
#define ERR(format, ...) Util::Logger::getInstance()->error(std::string("[{}:{}] ") + (format), __FILE__, __LINE__, ##__VA_ARGS__)
#define CRIT(format, ...) Util::Logger::getInstance()->critical(std::string("[{}:{}] ") + (format), __FILE__, __LINE__, ##__VA_ARGS__)

}  // namespace Util
