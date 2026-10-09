#pragma once

#include "Engine/Core/Base.h"

#include <string>
#include <string_view>

namespace SpiralEditor
{
    constexpr Engine::u32 kEntityClipboardVersion = 1;
    constexpr Engine::u32 kMaxClipboardEntities = 4096;
    constexpr size_t kMaxClipboardBodyBytes = 8ull * 1024ull * 1024ull;

    enum class ClipboardStatus
    {
        Ok,
        NotAClipboard,
        UnsupportedVersion,
        Malformed,
        Truncated,
        TooLarge,
        ChecksumMismatch
    };

    const char* ToString(ClipboardStatus status);

    // The Editor-defined serialized form of the copied entities. The clipboard layer
    // never interprets Body; it is opaque and must not contain NUL so the whole
    // text can travel through an operating-system text clipboard.
    struct EntityClipboardPayload
    {
        Engine::u32 EntityCount = 0;
        std::string Body;

        bool operator==(const EntityClipboardPayload&) const = default;
    };

    // Text form: one canonical header line
    //   SpiralEntityClipboard <version> <entityCount> <bodyBytes> <sha256-lowercase-hex>\n
    // followed by exactly bodyBytes bytes of Body and nothing else. The digest covers
    // "<version> <entityCount> <bodyBytes>\n" followed by Body. Both directions
    // enforce the same limits. A failed call leaves its output argument untouched.
    ClipboardStatus EncodeEntityClipboard(const EntityClipboardPayload& payload, std::string& outText);
    ClipboardStatus DecodeEntityClipboard(std::string_view text, EntityClipboardPayload& outPayload);
}
