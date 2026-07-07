#pragma once

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

        static void Init();
        static void Shutdown();
        static void SetMinimumLevel(Level level);

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
        static Level s_MinimumLevel;
        static bool s_Initialized;
    };
}
