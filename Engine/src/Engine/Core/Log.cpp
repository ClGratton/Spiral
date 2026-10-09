#include "Engine/Core/Log.h"

#include <iostream>
#include <utility>
#include <vector>

namespace Engine
{
    namespace
    {
        struct SinkEntry
        {
            Log::SinkId Id = Log::kInvalidSinkId;
            Log::Sink Callback;
        };

        std::vector<SinkEntry> s_Sinks;
        Log::SinkId s_NextSinkId = 1;
        thread_local bool t_InsideSink = false;

        struct SinkScope
        {
            SinkScope() { t_InsideSink = true; }
            ~SinkScope() { t_InsideSink = false; }
        };
    }

    std::mutex Log::s_Mutex;
    std::atomic<Log::Level> Log::s_MinimumLevel { Log::Level::Trace };
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
        s_Sinks.clear();
        s_Initialized = false;
    }

    void Log::SetMinimumLevel(Level level)
    {
        s_MinimumLevel.store(level, std::memory_order_relaxed);
    }

    Log::SinkId Log::AddSink(Sink sink)
    {
        if (!sink || t_InsideSink)
            return kInvalidSinkId;

        std::scoped_lock lock(s_Mutex);
        const SinkId id = s_NextSinkId++;
        s_Sinks.push_back({ id, std::move(sink) });
        return id;
    }

    bool Log::RemoveSink(SinkId id)
    {
        if (id == kInvalidSinkId || t_InsideSink)
            return false;

        std::scoped_lock lock(s_Mutex);
        for (auto it = s_Sinks.begin(); it != s_Sinks.end(); ++it)
        {
            if (it->Id == id)
            {
                s_Sinks.erase(it);
                return true;
            }
        }

        return false;
    }

    void Log::WriteLine(Level level, std::string_view message)
    {
        if (static_cast<int>(level) < static_cast<int>(s_MinimumLevel.load(std::memory_order_relaxed)))
            return;

        // A sink that logs already holds the mutex on this thread.
        if (t_InsideSink)
        {
            std::ostream& stream = level == Level::Error ? std::cerr : std::cout;
            stream << "[" << LevelName(level) << "] " << message << std::endl;
            return;
        }

        std::scoped_lock lock(s_Mutex);
        std::ostream& stream = level == Level::Error ? std::cerr : std::cout;
        stream << "[" << LevelName(level) << "] " << message << std::endl;

        if (s_Sinks.empty())
            return;

        SinkScope scope;
        for (const SinkEntry& sink : s_Sinks)
        {
            try
            {
                sink.Callback(level, message);
            }
            catch (...)
            {
            }
        }
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
