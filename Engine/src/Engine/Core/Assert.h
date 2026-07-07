#pragma once

#include "Engine/Core/Log.h"

#include <cstdlib>

#if defined(GE_ENABLE_ASSERTS)
    #define GE_ASSERT(check, ...)                                                                 \
        do                                                                                        \
        {                                                                                         \
            if (!(check))                                                                         \
            {                                                                                     \
                ::Engine::Log::Error("Assertion failed: ", #check __VA_OPT__(, " | ", __VA_ARGS__)); \
                std::abort();                                                                     \
            }                                                                                     \
        } while (false)
#else
    #define GE_ASSERT(check, ...) ((void)0)
#endif

#define GE_CORE_ASSERT(check, ...) GE_ASSERT(check __VA_OPT__(,) __VA_ARGS__)
