#include "EntityNaming.h"

#include <algorithm>
#include <cstdint>

namespace SpiralEditor
{
    namespace
    {
        constexpr std::array<std::string_view, 5> kReservedNames {
            "Prototype Mesh",
            "Directional Light",
            "Player Start",
            "Editor Control Shared Peer",
            "Authored Entity"
        };

        // Length of the strictly valid UTF-8 sequence at `index`, or 0.
        size_t DecodeUtf8(std::string_view text, size_t index, std::uint32_t& codePoint)
        {
            const auto byte = [&](size_t offset) { return static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + offset])); };
            const std::uint32_t lead = byte(0);
            size_t length = 0;
            std::uint32_t minimum = 0;
            if (lead < 0x80)
            {
                codePoint = lead;
                return 1;
            }
            if (lead >= 0xC2 && lead <= 0xDF)
            {
                length = 2;
                codePoint = lead & 0x1F;
                minimum = 0x80;
            }
            else if (lead >= 0xE0 && lead <= 0xEF)
            {
                length = 3;
                codePoint = lead & 0x0F;
                minimum = 0x800;
            }
            else if (lead >= 0xF0 && lead <= 0xF4)
            {
                length = 4;
                codePoint = lead & 0x07;
                minimum = 0x10000;
            }
            else
            {
                return 0;
            }

            if (index + length > text.size())
                return 0;
            for (size_t offset = 1; offset < length; ++offset)
            {
                if ((byte(offset) & 0xC0) != 0x80)
                    return 0;
                codePoint = (codePoint << 6) | (byte(offset) & 0x3F);
            }
            if (codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
                return 0;
            return length;
        }

        bool IsControl(std::uint32_t codePoint)
        {
            return codePoint < 0x20 || (codePoint >= 0x7F && codePoint <= 0x9F);
        }

        // Splits a trailing " (N)" with canonical decimal N >= 1 off `name`.
        std::string_view StripCopySuffix(std::string_view name)
        {
            if (name.size() < 5 || name.back() != ')')
                return name;
            const size_t open = name.rfind(" (");
            if (open == std::string_view::npos || open == 0)
                return name;
            const std::string_view digits = name.substr(open + 2, name.size() - open - 3);
            if (digits.empty() || digits.size() > 9 || digits[0] == '0')
                return name;
            for (const char digit : digits)
            {
                if (digit < '0' || digit > '9')
                    return name;
            }
            return name.substr(0, open);
        }

        // Longest prefix of `text` within `limit` bytes that ends on a code point boundary.
        std::string_view TruncateAtBoundary(std::string_view text, size_t limit)
        {
            if (text.size() <= limit)
                return text;
            size_t end = limit;
            while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
                --end;
            return text.substr(0, end);
        }
    }

    const char* ToString(EntityNameStatus status)
    {
        switch (status)
        {
            case EntityNameStatus::Valid: return "valid";
            case EntityNameStatus::Empty: return "the name is empty";
            case EntityNameStatus::TooLong: return "the name is longer than 127 bytes";
            case EntityNameStatus::InvalidUtf8: return "the name is not valid UTF-8";
            case EntityNameStatus::ControlCharacter: return "the name contains a control character";
            case EntityNameStatus::EdgeSpace: return "the name starts or ends with a space";
        }
        return "unknown";
    }

    std::string_view TrimEntityName(std::string_view name)
    {
        while (!name.empty() && name.front() == ' ')
            name.remove_prefix(1);
        while (!name.empty() && name.back() == ' ')
            name.remove_suffix(1);
        return name;
    }

    EntityNameStatus ValidateEntityName(std::string_view name)
    {
        if (name.empty())
            return EntityNameStatus::Empty;
        if (name.size() > kMaxEntityNameBytes)
            return EntityNameStatus::TooLong;
        bool hasControl = false;
        for (size_t index = 0; index < name.size();)
        {
            std::uint32_t codePoint = 0;
            const size_t length = DecodeUtf8(name, index, codePoint);
            if (length == 0)
                return EntityNameStatus::InvalidUtf8;
            hasControl |= IsControl(codePoint);
            index += length;
        }
        if (hasControl)
            return EntityNameStatus::ControlCharacter;
        if (name.front() == ' ' || name.back() == ' ')
            return EntityNameStatus::EdgeSpace;
        return EntityNameStatus::Valid;
    }

    std::string SanitizeEntityName(std::string_view name)
    {
        std::string repaired;
        repaired.reserve(std::min(name.size(), kMaxEntityNameBytes * 2));
        for (size_t index = 0; index < name.size();)
        {
            std::uint32_t codePoint = 0;
            const size_t length = DecodeUtf8(name, index, codePoint);
            if (length == 0)
            {
                repaired.push_back('?');
                ++index;
            }
            else if (IsControl(codePoint))
            {
                repaired.push_back(' ');
                index += length;
            }
            else
            {
                repaired.append(name.substr(index, length));
                index += length;
            }
            if (repaired.size() > kMaxEntityNameBytes + 4)
                break;
        }

        std::string_view trimmed = TrimEntityName(repaired);
        trimmed = TrimEntityName(TruncateAtBoundary(trimmed, kMaxEntityNameBytes));
        return trimmed.empty() ? std::string(kDefaultEntityName) : std::string(trimmed);
    }

    std::span<const std::string_view> LookupReservedEntityNames()
    {
        return kReservedNames;
    }

    EntityNamePool::EntityNamePool(std::span<const std::string_view> reserved)
    {
        for (const std::string_view name : reserved)
            m_Reserved.emplace(name);
    }

    void EntityNamePool::Add(std::string_view name)
    {
        ++m_Occupied[std::string(name)];
    }

    bool EntityNamePool::Remove(std::string_view name)
    {
        const auto found = m_Occupied.find(name);
        if (found == m_Occupied.end())
            return false;
        if (--found->second == 0)
            m_Occupied.erase(found);
        m_CopyHints.clear();
        return true;
    }

    bool EntityNamePool::IsTaken(std::string_view name) const
    {
        return m_Occupied.contains(name) || m_Reserved.contains(name);
    }

    std::string EntityNamePool::Resolve(std::string_view requested) const
    {
        const std::string sanitized = SanitizeEntityName(requested);
        if (!IsTaken(sanitized))
            return sanitized;

        const std::string_view stem = StripCopySuffix(sanitized);
        size_t& hint = m_CopyHints.try_emplace(std::string(stem), 2).first->second;
        for (size_t copy = hint;; ++copy)
        {
            const std::string suffix = " (" + std::to_string(copy) + ")";
            const std::string_view room = TrimEntityName(TruncateAtBoundary(stem, kMaxEntityNameBytes - suffix.size()));
            const std::string candidate = std::string(room) + suffix;
            if (!IsTaken(candidate))
            {
                hint = copy;
                return candidate;
            }
        }
    }

    std::string EntityNamePool::Claim(std::string_view requested)
    {
        std::string name = Resolve(requested);
        Add(name);
        return name;
    }
}
