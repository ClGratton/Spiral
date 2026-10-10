#pragma once

#include "Engine/Core/Log.h"

#include <chrono>
#include <string>
#include <string_view>

namespace Engine::Diagnostics
{
    // Measures the enclosing scope. The name is copied, so a temporary such as
    // GE_PROFILE_SCOPE("Job:" + name) stays valid until the destructor logs it.
    // With GE_ENABLE_PROFILE undefined the timer holds no state and reads no clock.
    class ScopedTimer
    {
    public:
#if defined(GE_ENABLE_PROFILE)
        explicit ScopedTimer(std::string_view name)
            : m_Name(name), m_Start(std::chrono::steady_clock::now())
        {
        }

        ~ScopedTimer()
        {
            const auto end = std::chrono::steady_clock::now();
            const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(end - m_Start).count();
            Log::Trace("[Profile] ", m_Name, ": ", microseconds, "us");
        }

    private:
        std::string m_Name;
        std::chrono::steady_clock::time_point m_Start;
#else
        explicit ScopedTimer(std::string_view name) { (void)name; }
#endif
    };
}

// The two-level concatenation expands __COUNTER__ before pasting, so several
// scopes can share one block, even on one line (a single level would paste the
// literal token).
#define GE_PROFILE_CONCAT_IMPL(a, b) a##b
#define GE_PROFILE_CONCAT(a, b) GE_PROFILE_CONCAT_IMPL(a, b)
#define GE_PROFILE_SCOPE(name) ::Engine::Diagnostics::ScopedTimer GE_PROFILE_CONCAT(GE_PROFILE_SCOPE_TIMER_, __COUNTER__)(name)
#define GE_PROFILE_FUNCTION() GE_PROFILE_SCOPE(__func__)
