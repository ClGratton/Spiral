#pragma once

#include "Engine/Core/Base.h"

#include <string>
#include <string_view>

namespace EditorHistory
{
    enum class HistorySource : Engine::u8
    {
        User,
        Agent,
        System
    };

    // Lower-case word the History panel shows as text next to agent and system
    // entries, so source is never conveyed by colour alone.
    const char* HistorySourceName(HistorySource source);

    // What an entry is called: a verb ("Move", "Rename", "Import Fab Asset")
    // and the target it acted on ("Cube", "3 Entities", "Project").
    struct HistoryLabel
    {
        std::string Verb;
        std::string Target;
        HistorySource Source = HistorySource::User;

        // "Move Cube", or just the verb when there is no target.
        std::string Display() const;

        // FNV-1a 64 over verb, target and source. Identical across processes
        // and platforms, so it can key a merge rule or a saved filter.
        Engine::u64 StableId() const;

        friend bool operator==(const HistoryLabel&, const HistoryLabel&) = default;
    };

    constexpr size_t kMaximumHistoryTargetCodepoints = 32;

    // Replaces control characters with spaces, replaces invalid UTF-8 bytes with
    // '?', trims surrounding whitespace, and shortens to at most
    // maxCodepoints code points, ending in "..." when shortened. The result is
    // always well-formed single-line UTF-8.
    std::string SanitizeHistoryText(std::string_view text, size_t maxCodepoints = kMaximumHistoryTargetCodepoints);

    // Builds a label with the target sanitized to 32 code points and the verb
    // sanitized to 48.
    HistoryLabel MakeHistoryLabel(std::string_view verb, std::string_view target, HistorySource source = HistorySource::User);

    // "512 B", "1.5 KiB", "256 MiB", "2.25 GiB": binary units, at most two
    // fractional digits with trailing zeros removed.
    std::string FormatHistoryBytes(Engine::u64 bytes);

    // "37 oldest entries were dropped to stay within 256 MiB" (budget) or
    // "... to stay within 512 entries" (entry cap); both reasons are joined
    // with " and ".
    std::string FormatEvictionNotice(size_t droppedForBudget, size_t droppedForEntryCap, Engine::u64 budgetBytes, size_t maximumEntries);

    // "Redo history discarded (3)".
    std::string FormatRedoDiscardedNotice(size_t discarded);
}
