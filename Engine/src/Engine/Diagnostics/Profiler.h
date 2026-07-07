#pragma once

#include "Engine/Core/Log.h"

#include <chrono>
#include <string_view>

namespace Engine::Diagnostics
{
    class ScopedTimer
    {
    public:
        explicit ScopedTimer(std::string_view name)
            : m_Name(name), m_Start(std::chrono::steady_clock::now())
        {
        }

        ~ScopedTimer()
        {
#if defined(GE_ENABLE_PROFILE)
            const auto end = std::chrono::steady_clock::now();
            const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(end - m_Start).count();
            Log::Trace("[Profile] ", m_Name, ": ", microseconds, "us");
#endif
        }

    private:
        std::string_view m_Name;
        std::chrono::steady_clock::time_point m_Start;
    };
}

#define GE_PROFILE_SCOPE(name) ::Engine::Diagnostics::ScopedTimer GE_PROFILE_SCOPE_TIMER_##__LINE__(name)
#define GE_PROFILE_FUNCTION() GE_PROFILE_SCOPE(__func__)
