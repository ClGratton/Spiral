#include "FuzzyMatch.h"

#include <algorithm>
#include <optional>

namespace SpiralEditor
{
    namespace
    {
        using Engine::u32;

        constexpr int kMatchBase = 16;
        constexpr int kBoundaryTextStart = 24;
        constexpr int kBoundaryWordStart = 20;
        constexpr int kBoundaryCamel = 16;
        constexpr int kConsecutive = 14;
        constexpr int kGapOpen = 3;
        constexpr int kLeadingCap = 6;
        constexpr int kInfeasible = -1000000000;

        struct Unit
        {
            u32 Value;
            u32 Offset;
            u32 Length;
        };

        bool Continuation(std::string_view text, size_t index, unsigned char low = 0x80, unsigned char high = 0xBF)
        {
            if (index >= text.size())
                return false;
            const auto byte = static_cast<unsigned char>(text[index]);
            return byte >= low && byte <= high;
        }

        // Strict UTF-8: overlongs, surrogates, and values past U+10FFFF are invalid, and every byte of an
        // invalid sequence becomes its own unit above the Unicode range.
        std::vector<Unit> Decode(std::string_view text)
        {
            std::vector<Unit> units;
            units.reserve(text.size());
            size_t i = 0;
            while (i < text.size())
            {
                const auto b0 = static_cast<unsigned char>(text[i]);
                u32 value = 0;
                size_t length = 0;
                if (b0 < 0x80)
                {
                    value = b0;
                    length = 1;
                }
                else if (b0 >= 0xC2 && b0 <= 0xDF && Continuation(text, i + 1))
                {
                    value = (u32(b0 & 0x1F) << 6) | (u32(text[i + 1]) & 0x3F);
                    length = 2;
                }
                else if (b0 >= 0xE0 && b0 <= 0xEF
                    && Continuation(text, i + 1, b0 == 0xE0 ? 0xA0 : 0x80, b0 == 0xED ? 0x9F : 0xBF)
                    && Continuation(text, i + 2))
                {
                    value = (u32(b0 & 0x0F) << 12) | ((u32(text[i + 1]) & 0x3F) << 6) | (u32(text[i + 2]) & 0x3F);
                    length = 3;
                }
                else if (b0 >= 0xF0 && b0 <= 0xF4
                    && Continuation(text, i + 1, b0 == 0xF0 ? 0x90 : 0x80, b0 == 0xF4 ? 0x8F : 0xBF)
                    && Continuation(text, i + 2) && Continuation(text, i + 3))
                {
                    value = (u32(b0 & 0x07) << 18) | ((u32(text[i + 1]) & 0x3F) << 12)
                        | ((u32(text[i + 2]) & 0x3F) << 6) | (u32(text[i + 3]) & 0x3F);
                    length = 4;
                }
                else
                {
                    value = 0x110000u + b0;
                    length = 1;
                }
                units.push_back({ value, static_cast<u32>(i), static_cast<u32>(length) });
                i += length;
            }
            return units;
        }

        bool IsAsciiUpper(u32 v) { return v >= 'A' && v <= 'Z'; }
        bool IsAsciiLower(u32 v) { return v >= 'a' && v <= 'z'; }
        bool IsAsciiSeparator(u32 v)
        {
            return v < 0x80 && !IsAsciiUpper(v) && !IsAsciiLower(v) && !(v >= '0' && v <= '9');
        }

        bool UnitMatches(u32 query, u32 text)
        {
            if (IsAsciiLower(query))
                return text == query || text == query - 'a' + 'A';
            return text == query;
        }

        int Boundary(const std::vector<Unit>& text, size_t p)
        {
            if (p == 0)
                return kBoundaryTextStart;
            const u32 previous = text[p - 1].Value;
            if (IsAsciiSeparator(previous))
                return kBoundaryWordStart;
            if (IsAsciiLower(previous) && IsAsciiUpper(text[p].Value))
                return kBoundaryCamel;
            return 0;
        }

        bool ContiguousAt(const std::vector<Unit>& query, const std::vector<Unit>& text, size_t start)
        {
            if (start + query.size() > text.size())
                return false;
            for (size_t j = 0; j < query.size(); ++j)
                if (!UnitMatches(query[j].Value, text[start + j].Value))
                    return false;
            return true;
        }

        int ContiguousScore(const std::vector<Unit>& query, const std::vector<Unit>& text, size_t start)
        {
            int score = -static_cast<int>(std::min<size_t>(start, kLeadingCap));
            for (size_t j = 0; j < query.size(); ++j)
                score += kMatchBase + Boundary(text, start + j) + (j > 0 ? kConsecutive : 0);
            return score;
        }

        bool IsSubsequence(const std::vector<Unit>& query, const std::vector<Unit>& text)
        {
            size_t j = 0;
            for (size_t p = 0; p < text.size() && j < query.size(); ++p)
                if (UnitMatches(query[j].Value, text[p].Value))
                    ++j;
            return j == query.size();
        }

        // Maximum-score alignment; lexicographically smallest on ties. The table F[j][p] is the best total of
        // the contributions of query units j+1.. given that unit j sits at text position p.
        std::vector<size_t> BestSubsequenceAlignment(const std::vector<Unit>& query, const std::vector<Unit>& text, int& bestScore)
        {
            const size_t m = query.size();
            const size_t n = text.size();
            std::vector<int> boundary(n);
            for (size_t p = 0; p < n; ++p)
                boundary[p] = Boundary(text, p);

            std::vector<std::vector<int>> future(m, std::vector<int>(n, 0));
            std::vector<int> next(n);
            std::vector<int> gap(n);
            for (size_t jj = m - 1; jj-- > 0;)
            {
                // H(p') = score of placing unit jj+1 at p' together with everything after it.
                for (size_t p = 0; p < n; ++p)
                    next[p] = UnitMatches(query[jj + 1].Value, text[p].Value) && future[jj + 1][p] > kInfeasible / 2
                        ? kMatchBase + boundary[p] + future[jj + 1][p]
                        : kInfeasible;
                for (size_t p = n; p-- > 0;)
                {
                    const int viaSkip = p + 2 < n ? next[p + 2] - (kGapOpen + 1) : kInfeasible;
                    const int viaExtend = p + 1 < n && gap[p + 1] > kInfeasible / 2 ? gap[p + 1] - 1 : kInfeasible;
                    gap[p] = std::max(viaSkip, viaExtend);
                    const int viaNext = p + 1 < n && next[p + 1] > kInfeasible / 2 ? next[p + 1] + kConsecutive : kInfeasible;
                    future[jj][p] = std::max(viaNext, gap[p]);
                }
            }

            bestScore = kInfeasible;
            for (size_t p = 0; p < n; ++p)
            {
                if (!UnitMatches(query[0].Value, text[p].Value) || future[0][p] <= kInfeasible / 2)
                    continue;
                const int score = kMatchBase + boundary[p] - static_cast<int>(std::min<size_t>(p, kLeadingCap)) + future[0][p];
                bestScore = std::max(bestScore, score);
            }

            std::vector<size_t> positions;
            for (size_t p = 0; p < n && positions.empty(); ++p)
                if (UnitMatches(query[0].Value, text[p].Value) && future[0][p] > kInfeasible / 2
                    && kMatchBase + boundary[p] - static_cast<int>(std::min<size_t>(p, kLeadingCap)) + future[0][p] == bestScore)
                    positions.push_back(p);
            for (size_t j = 1; j < m; ++j)
            {
                const size_t previous = positions.back();
                const int target = future[j - 1][previous];
                for (size_t p = previous + 1; p < n; ++p)
                {
                    if (!UnitMatches(query[j].Value, text[p].Value) || future[j][p] <= kInfeasible / 2)
                        continue;
                    const int step = p == previous + 1 ? kConsecutive : -static_cast<int>(p - previous + 2);
                    if (kMatchBase + boundary[p] + step + future[j][p] == target)
                    {
                        positions.push_back(p);
                        break;
                    }
                }
            }
            return positions;
        }

        std::vector<FuzzyHighlight> ToHighlights(const std::vector<Unit>& text, const std::vector<size_t>& positions)
        {
            std::vector<FuzzyHighlight> highlights;
            highlights.reserve(positions.size());
            for (const size_t p : positions)
                highlights.push_back({ text[p].Offset, text[p].Length });
            return highlights;
        }

        struct Scored
        {
            FuzzyMatchResult Result;
            size_t TextUnits = 0;
        };

        std::optional<Scored> MatchUnits(const std::vector<Unit>& query, std::string_view textBytes)
        {
            const std::vector<Unit> text = Decode(textBytes);
            if (query.size() > kFuzzyMaximumQueryUnits || text.size() > kFuzzyMaximumTextUnits)
                return std::nullopt;
            Scored scored;
            scored.TextUnits = text.size();
            if (query.empty())
            {
                scored.Result = { true, FuzzyTier::Prefix, 0, {} };
                return scored;
            }
            if (!IsSubsequence(query, text))
                return std::nullopt;

            std::vector<size_t> positions;
            const auto contiguous = [&](size_t start)
            {
                positions.clear();
                for (size_t j = 0; j < query.size(); ++j)
                    positions.push_back(start + j);
            };

            FuzzyMatchResult& result = scored.Result;
            result.Matched = true;
            if (ContiguousAt(query, text, 0))
            {
                result.Tier = query.size() == text.size() ? FuzzyTier::Exact : FuzzyTier::Prefix;
                result.Score = ContiguousScore(query, text, 0);
                contiguous(0);
            }
            else
            {
                std::optional<size_t> bestStart;
                int bestScore = 0;
                for (size_t start = 1; start + query.size() <= text.size(); ++start)
                {
                    if (!ContiguousAt(query, text, start))
                        continue;
                    const int score = ContiguousScore(query, text, start);
                    if (!bestStart || score > bestScore)
                    {
                        bestStart = start;
                        bestScore = score;
                    }
                }
                if (bestStart)
                {
                    result.Tier = FuzzyTier::Substring;
                    result.Score = bestScore;
                    contiguous(*bestStart);
                }
                else
                {
                    result.Tier = FuzzyTier::Subsequence;
                    positions = BestSubsequenceAlignment(query, text, result.Score);
                }
            }
            result.Highlights = ToHighlights(text, positions);
            return scored;
        }
    }

    FuzzyMatchResult FuzzyMatch(std::string_view query, std::string_view text)
    {
        const std::vector<Unit> queryUnits = Decode(query);
        std::optional<Scored> scored = MatchUnits(queryUnits, text);
        return scored ? std::move(scored->Result) : FuzzyMatchResult {};
    }

    std::vector<FuzzyRanked> FuzzyRank(std::string_view query, std::span<const std::string_view> candidates, size_t limit)
    {
        struct Entry
        {
            FuzzyRanked Ranked;
            size_t TextUnits;
        };

        const std::vector<Unit> queryUnits = Decode(query);
        std::vector<Entry> entries;
        for (size_t index = 0; index < candidates.size(); ++index)
        {
            std::optional<Scored> scored = MatchUnits(queryUnits, candidates[index]);
            if (!scored)
                continue;
            entries.push_back({ { index, scored->Result.Tier, scored->Result.Score, std::move(scored->Result.Highlights) },
                scored->TextUnits });
        }
        if (!queryUnits.empty())
            std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b)
            {
                if (a.Ranked.Tier != b.Ranked.Tier)
                    return a.Ranked.Tier < b.Ranked.Tier;
                if (a.Ranked.Score != b.Ranked.Score)
                    return a.Ranked.Score > b.Ranked.Score;
                if (a.TextUnits != b.TextUnits)
                    return a.TextUnits < b.TextUnits;
                return a.Ranked.Index < b.Ranked.Index;
            });

        std::vector<FuzzyRanked> ranked;
        ranked.reserve(std::min(limit, entries.size()));
        for (Entry& entry : entries)
        {
            if (ranked.size() == limit)
                break;
            ranked.push_back(std::move(entry.Ranked));
        }
        return ranked;
    }
}
