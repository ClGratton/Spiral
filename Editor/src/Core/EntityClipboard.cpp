#include "EntityClipboard.h"

#include "Engine/Core/Sha256.h"

#include <charconv>

namespace SpiralEditor
{
    namespace
    {
        constexpr std::string_view kMagic = "SpiralEntityClipboard";
        // magic, three decimal fields, a 64-character digest, separators, newline.
        constexpr size_t kMaxHeaderBytes = kMagic.size() + 3 * 11 + 64 + 8;

        bool ParseCanonicalU32(std::string_view token, Engine::u32& outValue)
        {
            if (token.empty() || token.size() > 10)
                return false;
            Engine::u32 value = 0;
            const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
            if (error != std::errc {} || end != token.data() + token.size())
                return false;
            if (token != std::to_string(value))
                return false;
            outValue = value;
            return true;
        }

        // Covers the version, count, and length fields as well as the body.
        std::string Digest(std::string_view fields, std::string_view body)
        {
            Engine::Sha256Builder builder;
            builder.Update(fields);
            builder.Update(std::string_view("\n"));
            builder.Update(body);
            return builder.FinalizeHex();
        }

        bool IsLowercaseHex64(std::string_view token)
        {
            if (token.size() != 64)
                return false;
            for (const char digit : token)
            {
                if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f')))
                    return false;
            }
            return true;
        }
    }

    const char* ToString(ClipboardStatus status)
    {
        switch (status)
        {
            case ClipboardStatus::Ok: return "ok";
            case ClipboardStatus::NotAClipboard: return "the text is not an entity clipboard";
            case ClipboardStatus::UnsupportedVersion: return "the clipboard was written by an unsupported version";
            case ClipboardStatus::Malformed: return "the clipboard header or payload is malformed";
            case ClipboardStatus::Truncated: return "the clipboard payload is truncated";
            case ClipboardStatus::TooLarge: return "the clipboard payload exceeds its limit";
            case ClipboardStatus::ChecksumMismatch: return "the clipboard payload is corrupt";
        }
        return "unknown";
    }

    ClipboardStatus EncodeEntityClipboard(const EntityClipboardPayload& payload, std::string& outText)
    {
        if (payload.Body.size() > kMaxClipboardBodyBytes || payload.EntityCount > kMaxClipboardEntities)
            return ClipboardStatus::TooLarge;
        if (payload.EntityCount == 0 || payload.Body.empty()
            || payload.Body.find('\0') != std::string::npos)
        {
            return ClipboardStatus::Malformed;
        }

        const std::string fields = std::to_string(kEntityClipboardVersion) + ' ' + std::to_string(payload.EntityCount) + ' '
            + std::to_string(payload.Body.size());
        std::string text = std::string(kMagic) + ' ' + fields + ' ' + Digest(fields, payload.Body) + '\n';
        text += payload.Body;
        outText = std::move(text);
        return ClipboardStatus::Ok;
    }

    ClipboardStatus DecodeEntityClipboard(std::string_view text, EntityClipboardPayload& outPayload)
    {
        if (!text.starts_with(kMagic) || text.size() == kMagic.size() || text[kMagic.size()] != ' ')
            return ClipboardStatus::NotAClipboard;

        const size_t newline = text.substr(0, kMaxHeaderBytes + 1).find('\n');
        if (newline == std::string_view::npos)
            return text.size() > kMaxHeaderBytes ? ClipboardStatus::Malformed : ClipboardStatus::Truncated;

        std::string_view header = text.substr(kMagic.size() + 1, newline - kMagic.size() - 1);
        std::string_view fields[4];
        for (size_t index = 0; index < 4; ++index)
        {
            const size_t space = header.find(' ');
            if ((index < 3) != (space != std::string_view::npos))
                return ClipboardStatus::Malformed;
            fields[index] = header.substr(0, space);
            header.remove_prefix(space == std::string_view::npos ? header.size() : space + 1);
        }

        Engine::u32 version = 0;
        Engine::u32 entityCount = 0;
        Engine::u32 bodyBytes = 0;
        if (!ParseCanonicalU32(fields[0], version) || !ParseCanonicalU32(fields[1], entityCount)
            || !ParseCanonicalU32(fields[2], bodyBytes) || !IsLowercaseHex64(fields[3]))
        {
            return ClipboardStatus::Malformed;
        }
        if (version != kEntityClipboardVersion)
            return ClipboardStatus::UnsupportedVersion;
        if (bodyBytes > kMaxClipboardBodyBytes || entityCount > kMaxClipboardEntities)
            return ClipboardStatus::TooLarge;
        if (entityCount == 0 || bodyBytes == 0)
            return ClipboardStatus::Malformed;

        const std::string_view body = text.substr(newline + 1);
        if (body.size() < bodyBytes)
            return ClipboardStatus::Truncated;
        if (body.size() > bodyBytes || body.find('\0') != std::string_view::npos)
            return ClipboardStatus::Malformed;
        const std::string fieldsText = std::string(fields[0]) + ' ' + std::string(fields[1]) + ' ' + std::string(fields[2]);
        if (Digest(fieldsText, body) != fields[3])
            return ClipboardStatus::ChecksumMismatch;

        outPayload.EntityCount = entityCount;
        outPayload.Body.assign(body);
        return ClipboardStatus::Ok;
    }
}
