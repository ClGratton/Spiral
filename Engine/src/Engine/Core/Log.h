#pragma once

#include "Engine/Core/Base.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace Engine
{
    class Log
    {
    public:
        enum class Level
        {
            Trace,
            Info,
            Warn,
            Error
        };

        // Receives every line that passes the minimum level, after it was written
        // to the console. The view is valid only during the call. Sinks run on the
        // logging thread under the log mutex, so delivery is totally ordered and
        // RemoveSink never returns while its callback is running; keep them short
        // (copy into a bounded queue) and non-throwing (exceptions are swallowed).
        // A sink must not call Init, Shutdown or Log::Add/RemoveSink; the latter two
        // refuse. A line logged from inside a sink is written but not re-delivered.
        using Sink = std::function<void(Level, std::string_view)>;
        using SinkId = u64;
        static constexpr SinkId kInvalidSinkId = 0;

        static void Init();
        // Also removes every registered sink.
        static void Shutdown();
        static void SetMinimumLevel(Level level);
        static SinkId AddSink(Sink sink);
        static bool RemoveSink(SinkId id);

        template<typename... Args>
        static void Trace(Args&&... args)
        {
            Write(Level::Trace, std::forward<Args>(args)...);
        }

        template<typename... Args>
        static void Info(Args&&... args)
        {
            Write(Level::Info, std::forward<Args>(args)...);
        }

        template<typename... Args>
        static void Warn(Args&&... args)
        {
            Write(Level::Warn, std::forward<Args>(args)...);
        }

        template<typename... Args>
        static void Error(Args&&... args)
        {
            Write(Level::Error, std::forward<Args>(args)...);
        }

    private:
        template<typename... Args>
        static void Write(Level level, Args&&... args)
        {
            std::ostringstream stream;
            ((stream << std::forward<Args>(args)), ...);
            WriteLine(level, stream.str());
        }

        static void WriteLine(Level level, std::string_view message);
        static std::string_view LevelName(Level level);

    private:
        static std::mutex s_Mutex;
        static std::atomic<Level> s_MinimumLevel;
        static bool s_Initialized;
    };
}
