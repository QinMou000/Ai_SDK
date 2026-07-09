#pragma once

#include <utility>

#include "trace/Trace.h"

namespace aiSDK {

// TraceRecorder 当前只负责累积步骤，
// 后续可以继续扩展时间戳、落盘与导出策略。
class TraceRecorder {
   public:
    explicit TraceRecorder(Trace trace) : trace_(std::move(trace)) {}

    void addStep(TraceStep step) {
        trace_.steps.push_back(std::move(step));
    }

    const Trace& snapshot() const {
        return trace_;
    }

   private:
    Trace trace_;
};

}  // namespace aiSDK