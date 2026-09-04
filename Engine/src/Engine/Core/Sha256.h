#pragma once

#include "Engine/Core/Base.h"

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace Engine
{
    class Sha256Builder
    {
    public:
        using Digest = std::array<u8, 32>;

        Sha256Builder();

        void Update(std::span<const u8> bytes);
        void Update(std::string_view text);

        Digest FinalizeBytes() const;
        std::string FinalizeHex() const;

        static Digest HashBytes(std::span<const u8> bytes);
        static std::string HashString(std::string_view text);
        static std::string ToHex(const Digest& digest);

    private:
        void ProcessBlock(const u8* block);

        std::array<u32, 8> m_State;
        std::array<u8, 64> m_Buffer {};
        size_t m_BufferedBytes = 0;
        u64 m_TotalBytes = 0;
    };
}
