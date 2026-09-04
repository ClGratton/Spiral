#include "Engine/Core/Sha256.h"

#include <algorithm>
#include <bit>

namespace Engine
{
    namespace
    {
        constexpr std::array<u32, 64> kConstants = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
        };

        constexpr char kHexDigits[] = "0123456789abcdef";
    }

    Sha256Builder::Sha256Builder()
        : m_State {
            0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
        }
    {
    }

    void Sha256Builder::Update(std::span<const u8> bytes)
    {
        m_TotalBytes += static_cast<u64>(bytes.size());
        if (m_BufferedBytes != 0)
        {
            const size_t copiedBytes = std::min(bytes.size(), m_Buffer.size() - m_BufferedBytes);
            std::copy_n(bytes.data(), copiedBytes, m_Buffer.data() + m_BufferedBytes);
            m_BufferedBytes += copiedBytes;
            bytes = bytes.subspan(copiedBytes);
            if (m_BufferedBytes == m_Buffer.size())
            {
                ProcessBlock(m_Buffer.data());
                m_BufferedBytes = 0;
            }
        }

        while (bytes.size() >= m_Buffer.size())
        {
            ProcessBlock(bytes.data());
            bytes = bytes.subspan(m_Buffer.size());
        }
        if (!bytes.empty())
        {
            std::copy(bytes.begin(), bytes.end(), m_Buffer.begin());
            m_BufferedBytes = bytes.size();
        }
    }

    void Sha256Builder::Update(std::string_view text)
    {
        Update(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
    }

    Sha256Builder::Digest Sha256Builder::FinalizeBytes() const
    {
        Sha256Builder final = *this;
        std::array<u8, 128> tail {};
        std::copy_n(final.m_Buffer.data(), final.m_BufferedBytes, tail.data());
        tail[final.m_BufferedBytes] = 0x80;
        const size_t tailBytes = final.m_BufferedBytes + 1 + sizeof(u64) <= 64 ? 64 : 128;
        const u64 bitLength = final.m_TotalBytes * 8;
        for (size_t index = 0; index < sizeof(bitLength); ++index)
            tail[tailBytes - 1 - index] = static_cast<u8>(bitLength >> (index * 8));
        final.ProcessBlock(tail.data());
        if (tailBytes == 128)
            final.ProcessBlock(tail.data() + 64);

        Digest digest {};
        for (size_t wordIndex = 0; wordIndex < final.m_State.size(); ++wordIndex)
        {
            for (size_t byteIndex = 0; byteIndex < 4; ++byteIndex)
                digest[wordIndex * 4 + byteIndex] = static_cast<u8>(
                    final.m_State[wordIndex] >> ((3 - byteIndex) * 8));
        }
        return digest;
    }

    std::string Sha256Builder::FinalizeHex() const
    {
        return ToHex(FinalizeBytes());
    }

    Sha256Builder::Digest Sha256Builder::HashBytes(std::span<const u8> bytes)
    {
        Sha256Builder builder;
        builder.Update(bytes);
        return builder.FinalizeBytes();
    }

    std::string Sha256Builder::HashString(std::string_view text)
    {
        Sha256Builder builder;
        builder.Update(text);
        return builder.FinalizeHex();
    }

    std::string Sha256Builder::ToHex(const Digest& digest)
    {
        std::string result(digest.size() * 2, '0');
        for (size_t index = 0; index < digest.size(); ++index)
        {
            result[index * 2] = kHexDigits[digest[index] >> 4];
            result[index * 2 + 1] = kHexDigits[digest[index] & 0x0f];
        }
        return result;
    }

    void Sha256Builder::ProcessBlock(const u8* block)
    {
        std::array<u32, 64> words {};
        for (size_t index = 0; index < 16; ++index)
        {
            const size_t offset = index * 4;
            words[index] = (static_cast<u32>(block[offset]) << 24)
                | (static_cast<u32>(block[offset + 1]) << 16)
                | (static_cast<u32>(block[offset + 2]) << 8)
                | static_cast<u32>(block[offset + 3]);
        }
        for (size_t index = 16; index < words.size(); ++index)
        {
            const u32 s0 = std::rotr(words[index - 15], 7) ^ std::rotr(words[index - 15], 18)
                ^ (words[index - 15] >> 3);
            const u32 s1 = std::rotr(words[index - 2], 17) ^ std::rotr(words[index - 2], 19)
                ^ (words[index - 2] >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }

        u32 a = m_State[0];
        u32 b = m_State[1];
        u32 c = m_State[2];
        u32 d = m_State[3];
        u32 e = m_State[4];
        u32 f = m_State[5];
        u32 g = m_State[6];
        u32 h = m_State[7];
        for (size_t index = 0; index < words.size(); ++index)
        {
            const u32 sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const u32 choice = (e & f) ^ (~e & g);
            const u32 temporary1 = h + sum1 + choice + kConstants[index] + words[index];
            const u32 sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const u32 majority = (a & b) ^ (a & c) ^ (b & c);
            const u32 temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        m_State[0] += a;
        m_State[1] += b;
        m_State[2] += c;
        m_State[3] += d;
        m_State[4] += e;
        m_State[5] += f;
        m_State[6] += g;
        m_State[7] += h;
    }
}
