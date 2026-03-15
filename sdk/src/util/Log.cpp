#include "../../include/util/Log.h"

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <mutex>

namespace Util {
std::shared_ptr<spdlog::logger> Logger::_instance = nullptr;
std::mutex Logger::_mutex;
Logger::Logger() {}
void Logger::initLogger(const std::string& loggerName, const std::string& loggerFile, spdlog::level::level_enum logLevel) {
    std::shared_ptr<spdlog::logger> instance;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        instance = std::atomic_load(&_instance);
        if(instance == nullptr) {
            if(loggerFile == "stdout") {
                instance = spdlog::stdout_color_mt(loggerName);
            } else {
                spdlog::init_thread_pool(32768, 1);
                instance = spdlog::basic_logger_mt<spdlog::async_factory>(loggerName, loggerFile);
            }
            std::atomic_store(&_instance, instance);
        }
    }
    // 达到这个等级的日志 自动刷新到输出目标
    spdlog::flush_on(logLevel);
    // [时间][日志器名称][日志级别][信息]
    instance->set_pattern("[%Y-%m-%d %H:%M:%S] [%n] [%-7l] %v");
    instance->set_level(logLevel);
}

std::shared_ptr<spdlog::logger> Logger::getInstance() {
    auto instance = std::atomic_load(&_instance);
    if(instance != nullptr) {
        return instance;
    }
    return spdlog::default_logger();
}

}  // namespace Util
