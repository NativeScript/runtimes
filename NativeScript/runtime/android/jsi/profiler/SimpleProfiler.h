#ifndef SIMPLEPROFILER_H_
#define SIMPLEPROFILER_H_

#include "EngineHost.h"
#include <cstdint>
#include <vector>

namespace tns {
#ifndef SIMPLE_PROFILER
#define SET_PROFILER_FRAME() ((void)0)
#else
#define SET_PROFILER_FRAME() SimpleProfiler __frame(__FILE__, __LINE__)
#endif

class SimpleProfiler {
    public:
        SimpleProfiler(char* fileName, int lineNumber);

        ~SimpleProfiler();

        static void Init(engine::Runtime& rt, engine::Object& global);

        static void PrintProfilerData();

    private:
        struct FrameEntry {
            FrameEntry(char* _fileName, int _lineNumer)
                :
                fileName(_fileName), lineNumber(_lineNumer), time(0), stackCount(0) {
            }
            bool operator<(const FrameEntry& rhs) const {
                return time < rhs.time;
            }
            char* fileName;
            int lineNumber;
            int64_t time;
            int stackCount;
        };

        static engine::Value PrintProfilerDataCallback(engine::Runtime& rt,
                                                       const engine::Value& thisVal,
                                                       const engine::Value* args, size_t count);

        FrameEntry* m_frame;
        int64_t m_time;
        static std::vector<FrameEntry> s_frames;
};
}

#endif /* SIMPLEPROFILER_H_ */
