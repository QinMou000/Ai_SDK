#include <spdlog/common.h>

#include <iostream>

#include "../sdk/include/util/Log.h"

int main() {
    Util::Logger::initLogger("main", "stdout", spdlog::level::debug);
    std::cout << "hello " << std::endl;
    TRACE("这是一个日志");
    DBG("这是一个日志");
    INF("这是一个日志");
    WRN("这是一个日志");
    return 0;
}
