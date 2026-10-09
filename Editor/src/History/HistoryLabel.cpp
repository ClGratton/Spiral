#include "HistoryLabel.h"

#include <array>
#include <vector>

namespace EditorHistory
{
    namespace
    {
        constexpr size_t kMaximumVerbCodepoints = 48;

        // Length in bytes of the well-formed UTF-8 sequence starting at offset,
        // or 0 when the bytes there are not one (overlong, surrogate, beyond
        // U+10FFFF, truncated).
        size_t ValidSequenceLength(std::string_view text, size_t offset)
        {
            const auto byteAt = [&](size_t index) { return static_cast<Engine::u8>(text[index]); };
            const Engine::u8 lead = byteAt(offset);
            if (lead < 0x80)
                return 1;

            size_t length = 0;
            Engine::u32 minimum = 0;
            Engine::u32 codepoint = 0;
            if (lead >= 0xC2 && lead <= 0xDF)
            {
                length = 2;
                minimum = 0x80;
                codepoint = lead & 0x1Fu;
            }
            else if (lead >= 0xE0 && lead <= 0xEF)
            {
                length = 3;
                minimum = 0x800;
                codepoint = lead & 0x0Fu;
            }
            else if (lead >= 0xF0 && lead <= 0xF4)
            {
                length = 4;
                minimum = 0x10000;
                codepoint = lead & 0x07u;
            }
            else
            {
                return 0;
            }

            if (offset + length > text.size())
                return 0;
            for (size_t index = 1; index < length; ++index)
            {
                const Engine::u8 continuation = byteAt(offset + index);
                if ((continuation & 0xC0u) != 0x80u)
                    return 0;
                codepoint = (codepoint << 6) | (continuation & 0x3Fu);
            }
            if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
                return 0;
            return length;
        }

        bool IsControlCodepoint(std::string_view sequence)
        {
            if (sequence.size() == 1)
            {
                const Engine::u8 byte = static_cast<Engine::u8>(sequence[0]);
                return byte < 0x20 || byte == 0x7F;
            }
            // U+0080..U+009F (C1 controls) encode as C2 80..C2 9F.
            return sequence.size() == 2 && static_cast<Engine::u8>(sequence[0]) == 0xC2
                && static_cast<Engine::u8>(sequence[1]) < 0xA0;
        }

        Engine::u64 HashBytes(Engine::u64 hash, std::string_view bytes)
        {
            for (const char byte : bytes)
            {
                hash ^= static_cast<Engine::u8>(byte);
                hash *= 1099511628211ull;
            }
            return hash;
        }
    }

    const char* HistorySourceName(HistorySource source)
    {
        switch (source)
        {
        case HistorySource::User: return "user";
        case HistorySource::Agent: return "agent";
        case HistorySource::System: return "system";
        }
        return "user";
    }

    std::string HistoryLabel::Display() const
    {
        if (Target.empty())
            return Verb;
        if (Verb.empty())
            return Target;
        return Verb + " " + Target;
    }

    Engine::u64 HistoryLabel::StableId() const
    {
        Engine::u64 hash = 14695981039346656037ull;
        hash = HashBytes(hash, Verb);
        hash = HashBytes(hash, std::string_view("\x1f", 1));
        hash = HashBytes(hash, Target);
        hash = HashBytes(hash, std::string_view("\x1f", 1));
        const char source = static_cast<char>(static_cast<Engine::u8>(Source));
        return HashBytes(hash, std::string_view(&source, 1));
    }

    std::string SanitizeHistoryText(std::string_view text, size_t maxCodepoints)
    {
        std::vector<std::string> codepoints;
        for (size_t offset = 0; offset < text.size();)
        {
            const size_t length = ValidSequenceLength(text, offset);
            if (length == 0)
            {
                codepoints.emplace_back("?");
                ++offset;
                continue;
            }
            const std::string_view sequence = text.substr(offset, length);
            codepoints.emplace_back(IsControlCodepoint(sequence) ? std::string(" ") : std::string(sequence));
            offset += length;
        }

        size_t first = 0;
        size_t last = codepoints.size();
        while (first < last && codepoints[first] == " ")
            ++first;
        while (last > first && codepoints[last - 1] == " ")
            --last;

        const size_t count = last - first;
        std::string result;
        if (count <= maxCodepoints)
        {
            for (size_t index = first; index < last; ++index)
                result += codepoints[index];
            return result;
        }

        constexpr size_t kEllipsis = 3;
        const size_t kept = maxCodepoints > kEllipsis ? maxCodepoints - kEllipsis : 0;
        for (size_t index = first; index < first + kept; ++index)
            result += codepoints[index];
        result.append(maxCodepoints >= kEllipsis ? kEllipsis : maxCodepoints, '.');
        return result;
    }

    HistoryLabel MakeHistoryLabel(std::string_view verb, std::string_view target, HistorySource source)
    {
        return { SanitizeHistoryText(verb, kMaximumVerbCodepoints), SanitizeHistoryText(target, kMaximumHistoryTargetCodepoints), source };
    }

    std::string FormatHistoryBytes(Engine::u64 bytes)
    {
        constexpr std::array<const char*, 4> units { "B", "KiB", "MiB", "GiB" };
        size_t unit = 0;
        Engine::u64 divisor = 1;
        while (unit + 1 < units.size() && bytes / divisor >= 1024)
        {
            divisor *= 1024;
            ++unit;
        }
        if (unit == 0)
            return std::to_string(bytes) + " B";

        // Round half up to hundredths in integers so the text is exact.
        const Engine::u64 hundredths = (bytes / divisor) * 100 + ((bytes % divisor) * 100 + divisor / 2) / divisor;
        const Engine::u64 whole = hundredths / 100;
        const Engine::u64 fraction = hundredths % 100;
        std::string text = std::to_string(whole);
        if (fraction != 0)
        {
            text += '.';
            text += static_cast<char>('0' + fraction / 10);
            if (fraction % 10 != 0)
                text += static_cast<char>('0' + fraction % 10);
        }
        return text + " " + units[unit];
    }

    std::string FormatEvictionNotice(size_t droppedForBudget, size_t droppedForEntryCap, Engine::u64 budgetBytes, size_t maximumEntries)
    {
        const size_t dropped = droppedForBudget + droppedForEntryCap;
        std::string text = std::to_string(dropped) + (dropped == 1 ? " oldest entry was" : " oldest entries were") + " dropped to stay within ";
        if (droppedForBudget != 0)
            text += FormatHistoryBytes(budgetBytes);
        if (droppedForBudget != 0 && droppedForEntryCap != 0)
            text += " and ";
        if (droppedForEntryCap != 0)
            text += std::to_string(maximumEntries) + (maximumEntries == 1 ? " entry" : " entries");
        return text;
    }

    std::string FormatRedoDiscardedNotice(size_t discarded)
    {
        return "Redo history discarded (" + std::to_string(discarded) + ")";
    }
}
