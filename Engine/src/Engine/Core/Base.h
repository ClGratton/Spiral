#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#if defined(_WIN32) && !defined(GE_PLATFORM_WINDOWS)
    #define GE_PLATFORM_WINDOWS
#endif

#if defined(__linux__) && !defined(GE_PLATFORM_LINUX)
    #define GE_PLATFORM_LINUX
#endif

#if defined(__APPLE__) && !defined(GE_PLATFORM_MACOS)
    #define GE_PLATFORM_MACOS
#endif

#define GE_EXPAND_MACRO(x) x
#define GE_STRINGIFY_MACRO(x) #x

#define GE_BIND_EVENT_FN(fn) [this](auto&&... args) -> decltype(auto) { return this->fn(std::forward<decltype(args)>(args)...); }

namespace Engine
{
    using u8 = std::uint8_t;
    using u16 = std::uint16_t;
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;

    template<typename T>
    using Scope = std::unique_ptr<T>;

    template<typename T, typename... Args>
    constexpr Scope<T> CreateScope(Args&&... args)
    {
        return std::make_unique<T>(std::forward<Args>(args)...);
    }

    template<typename T>
    using Ref = std::shared_ptr<T>;

    template<typename T, typename... Args>
    constexpr Ref<T> CreateRef(Args&&... args)
    {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }
}
