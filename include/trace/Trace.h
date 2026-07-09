#pragma once

#include <string>
#include <vector>

namespace aiSDK {

struct TraceStep {
    std::string type;
    std::string status;
    std::string detail;
    long long duration_ms = 0;
};

// Trace 先定义为简单可序列化结构，
// 方便后续直接导出为 JSON 做调试和可观测。
struct Trace {
    std::string trace_id;
    std::string provider;
    std::string model;
    std::vector<TraceStep> steps;
};

}  // namespace aiSDK