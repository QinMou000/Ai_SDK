#pragma once

#include <string>

namespace aiSDK {

// Error 统一承载跨模块错误，
// 避免不同层各自返回松散的字符串。
struct Error {
    std::string code;
    std::string message;
};

}  // namespace aiSDK