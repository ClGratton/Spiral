#pragma once

#include "Engine/Core/Base.h"

#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace SpiralEditor
{
    // Command-palette matching. The query must occur in the text as a subsequence of UTF-8 code points.
    // Case: a lowercase ASCII query letter matches either case, an uppercase one matches only itself,
    // and every other code point matches exactly; a byte that is not valid UTF-8 is its own code point.
    //
    // Ranking is lexicographic: tier (Exact, Prefix, Substring, Subsequence), then higher score, then
    // shorter text, then lower input index, so the order is total and independent of the algorithm's
    // internals. Score is the sum, over the chosen alignment, of
    //   16 per matched code point
    //   + 24 for a match at text position 0, else 20 at a word start (the previous code point is an
    //     ASCII non-alphanumeric), else 16 at a camelCase step (lowercase ASCII then uppercase ASCII)
    //   + 14 when the match directly follows the previous match
    //   - (g + 3) for a gap of g >= 1 skipped code points between two matches
    //   - min(p, 6) for a first match at position p.
    // Exact, Prefix, and Substring tiers use the contiguous alignment (the best-scoring occurrence for
    // Substring); Subsequence uses the maximum-score alignment. Ties pick the lexicographically smallest
    // position list.
    enum class FuzzyTier : Engine::u8
    {
        Exact,
        Prefix,
        Substring,
        Subsequence
    };

    // One matched code point of the text, as a byte range.
    struct FuzzyHighlight
    {
        Engine::u32 Offset = 0;
        Engine::u32 Length = 0;

        bool operator==(const FuzzyHighlight&) const = default;
    };

    inline constexpr size_t kFuzzyMaximumQueryUnits = 64;
    inline constexpr size_t kFuzzyMaximumTextUnits = 1024;

    struct FuzzyMatchResult
    {
        bool Matched = false;
        FuzzyTier Tier = FuzzyTier::Subsequence;
        int Score = 0;
        std::vector<FuzzyHighlight> Highlights; // one per query code point, ascending; empty for an empty query
    };

    // An empty query matches everything as Prefix with score 0. A query over kFuzzyMaximumQueryUnits or a
    // text over kFuzzyMaximumTextUnits code points never matches.
    FuzzyMatchResult FuzzyMatch(std::string_view query, std::string_view text);

    struct FuzzyRanked
    {
        size_t Index = 0; // into the candidate span
        FuzzyTier Tier = FuzzyTier::Subsequence;
        int Score = 0;
        std::vector<FuzzyHighlight> Highlights;
    };

    // Matches in ranked order, at most `limit`. An empty query returns the candidates in input order.
    std::vector<FuzzyRanked> FuzzyRank(std::string_view query, std::span<const std::string_view> candidates,
        size_t limit = std::numeric_limits<size_t>::max());
}
