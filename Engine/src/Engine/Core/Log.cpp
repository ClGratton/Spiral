#include "Engine/Core/Log.h"

#include <iostream>

namespace Engine
{
    std::mutex Log::s_Mutex;
    Log::Level Log::s_MinimumLevel = Log::Level::Trace;
    bool Log::s_Initialized = false;

    void Log::Init()
    {
        std::scoped_lock lock(s_Mutex);
        s_Initialized = true;
        std::cout << "[Engine] Log initialized" << std::endl;
    }

    void Log::Shutdown()
    {
        std::scoped_lock lock(s_Mutex);
        std::cout << "[Engine] Log shutdown" << std::endl;
        s_Initialized = false;
    }

    void Log::SetMinimumLevel(Level level)
    {
        std::scoped_lock lock(s_Mutex);
        s_MinimumLevel = level;
    }

    void Log::WriteLine(Level level, std::string_view message)
    {
        if (static_cast<int>(level) < static_cast<int>(s_MinimumLevel))
            return;

        std::scoped_lock lock(s_Mutex);
        std::ostream& stream = level == Level::Error ? std::cerr : std::cout;
        stream << "[" << LevelName(level) << "] " << message << std::endl;
    }

    std::string_view Log::LevelName(Level level)
    {
        switch (level)
        {
            case Level::Trace: return "Trace";
            case Level::Info: return "Info";
            case Level::Warn: return "Warn";
            case Level::Error: return "Error";
        }

        return "Unknown";
    }
}
