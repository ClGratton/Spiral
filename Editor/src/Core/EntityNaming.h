#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace SpiralEditor
{
    // Matches the 128-byte Inspector name buffer, including its terminator.
    constexpr size_t kMaxEntityNameBytes = 127;
    constexpr std::string_view kDefaultEntityName = "Entity";

    enum class EntityNameStatus
    {
        Valid,
        Empty,
        TooLong,
        InvalidUtf8,
        ControlCharacter,
        EdgeSpace
    };

    // A name is valid when it is canonical: non-empty, at most kMaxEntityNameBytes,
    // strictly valid UTF-8 without C0, DEL, or C1 control characters, and without
    // leading or trailing ASCII spaces.
    EntityNameStatus ValidateEntityName(std::string_view name);
    const char* ToString(EntityNameStatus status);

    // Trims ASCII spaces from both ends. Rename input is trimmed, then validated.
    std::string_view TrimEntityName(std::string_view name);

    // Total, idempotent repair for names that did not come from rename validation
    // (legacy scenes, pasted payloads): invalid UTF-8 bytes become '?', control
    // characters become spaces, edges are trimmed, the result is cut at a code
    // point boundary to kMaxEntityNameBytes, and an empty result becomes
    // kDefaultEntityName. The result always passes ValidateEntityName.
    std::string SanitizeEntityName(std::string_view name);

    // Names the Editor re-finds with Scene::FindEntityByName after undo, project
    // load, or smoke setup. A user-driven create, rename, duplicate, or paste must
    // never produce one of them, even while the entity that owns it is deleted,
    // because undoing that deletion would bring the name back twice.
    std::span<const std::string_view> LookupReservedEntityNames();

    // Occupied names plus the reserved set. Scene::FindEntityByName is an exact
    // byte comparison, so uniqueness is exact too.
    class EntityNamePool
    {
    public:
        explicit EntityNamePool(std::span<const std::string_view> reserved = LookupReservedEntityNames());

        void Add(std::string_view name);
        // Releases one occurrence; false when `name` was not occupied.
        bool Remove(std::string_view name);
        // Occupied or reserved.
        bool IsTaken(std::string_view name) const;

        // The name to use for `requested`: its sanitized form when that is free,
        // otherwise the lowest "Stem (N)" with N >= 2 that is free, where Stem is
        // the sanitized name without a trailing " (N)" and the whole result stays
        // within kMaxEntityNameBytes. Idempotent: resolving a result returns it.
        // Does not occupy the result.
        std::string Resolve(std::string_view requested) const;
        // Resolve, then occupy the result.
        std::string Claim(std::string_view requested);

    private:
        struct StringHash
        {
            using is_transparent = void;
            size_t operator()(std::string_view value) const { return std::hash<std::string_view> {}(value); }
        };

        std::unordered_map<std::string, size_t, StringHash, std::equal_to<>> m_Occupied;
        // Lowest copy number that can still be free for a stem. Names are only ever added
        // between removals, so numbers below the hint stay taken until Remove clears it.
        mutable std::unordered_map<std::string, size_t, StringHash, std::equal_to<>> m_CopyHints;
        std::unordered_set<std::string, StringHash, std::equal_to<>> m_Reserved;
    };
}
