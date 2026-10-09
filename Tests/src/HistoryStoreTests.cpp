#include "HistoryStoreTests.h"

#include "EditGesture.h"
#include "HistoryLabel.h"
#include "HistoryStore.h"
#include "ShortcutDispatch.h"
#include "TestSupport/GeneratedTest.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace EditorHistory;
    using Engine::u32;
    using Engine::u64;
    using Engine::u8;

    // Failure hypotheses, oracles, and non-claims for the whole file:
    // - The store must behave like a vector of full states with a cursor. The
    //   reference model below is exactly that, written with naive loops that
    //   recompute byte totals from scratch, and shares no code with the store.
    // - Hand-computed tables pin revisions, eviction order, byte accounting,
    //   announcement text, and restore counts for fixed scenarios, so a bug
    //   shared by the store and a model derived from it cannot hide.
    // - Properties are deterministic and replayable through
    //   SPIRAL_HISTORY_SEED / SPIRAL_HISTORY_REPLAY; a failure prints the seed,
    //   the original and minimised choice traces, and writes a counterexample
    //   JSON under the system temp directory.
    // - Tier: fast, in-process, no filesystem fixtures, no GPU, no ImGui.
    // - Not claimed: ImGui item activation semantics, the Editor's real
    //   HistoryState, real key delivery, or the accuracy of any byte estimator
    //   for the Editor's state types.

    struct Checker
    {
        const char* Suite;
        bool Ok = true;

        void Expect(bool condition, const std::string& message)
        {
            if (!condition)
            {
                std::cerr << "History test failed [" << Suite << "]: " << message << '\n';
                Ok = false;
            }
        }
    };

    struct TestState
    {
        u64 Id = 0;
        u64 Cost = 0;
    };

    using Ptr = std::shared_ptr<const TestState>;

    Ptr Make(u64 id, u64 cost)
    {
        return std::make_shared<const TestState>(TestState { id, cost });
    }

    class TestAdapter final : public IHistoryStateAdapter<TestState>
    {
    public:
        Ptr Live;
        bool FailRestore = false;
        size_t RestoreCalls = 0;
        mutable std::map<const TestState*, size_t> EstimateCalls;

        bool Restore(const Ptr& snapshot) override
        {
            if (FailRestore)
                return false;
            Live = snapshot;
            ++RestoreCalls;
            return true;
        }

        u64 EstimateBytes(const TestState& state) const override
        {
            ++EstimateCalls[&state];
            return state.Cost;
        }

        bool Equal(const TestState& first, const TestState& second) const override
        {
            return first.Id == second.Id;
        }
    };

    using Store = HistoryStore<TestState>;

    HistoryLabel EditLabel(u64 n)
    {
        return MakeHistoryLabel("Edit", "T" + std::to_string(n));
    }

    bool RunProperty(std::string_view name, const Spiral::Tests::Property& property, size_t iterations)
    {
        Spiral::Tests::CampaignOptions options;
        options.Iterations = iterations;
        if (const char* seed = std::getenv("SPIRAL_HISTORY_SEED"))
            options.Seed = std::strtoull(seed, nullptr, 10);
        Spiral::Tests::ChoiceTrace replay;
        if (const char* trace = std::getenv("SPIRAL_HISTORY_REPLAY"); trace && Spiral::Tests::ParseTrace(trace, replay))
            options.Replay = replay;

        Spiral::Tests::Counterexample failure;
        if (Spiral::Tests::RunCampaign(options, property, failure))
            return true;

        const std::string minimized = Spiral::Tests::SerializeTrace(failure.MinimizedTrace);
        const std::string rerun = "SPIRAL_HISTORY_SEED=" + std::to_string(failure.Seed) + " SPIRAL_HISTORY_REPLAY=\"" + minimized
            + "\" EngineTests --test <registered name of " + std::string(name) + ">";
        const std::filesystem::path artifact = std::filesystem::temp_directory_path()
            / ("spiral-history-counterexample-" + std::string(name) + ".json");
        std::string artifactError;
        const bool written = Spiral::Tests::WriteCounterexample(artifact, name, failure, rerun, artifactError);
        std::cerr << "History property failed [" << name << "]: " << failure.Message << "\n  seed=" << failure.Seed
            << " iteration=" << failure.Iteration << "\n  original trace (" << failure.OriginalTrace.size()
            << " choices) minimized to " << failure.MinimizedTrace.size() << "\n  rerun: " << rerun << '\n';
        if (written)
            std::cerr << "  counterexample: " << artifact.string() << '\n';
        else
            std::cerr << "  counterexample not written: " << artifactError << '\n';
        return false;
    }

    // ---- Labels ----

    // Independent UTF-8 recognizer from the RFC 3629 byte-range table.
    size_t NaiveSequenceLength(const std::string& text, size_t i)
    {
        const auto byteAt = [&](size_t k) { return static_cast<u8>(text[i + k]); };
        const auto within = [&](size_t k, u8 low, u8 high) { return i + k < text.size() && byteAt(k) >= low && byteAt(k) <= high; };
        const u8 lead = byteAt(0);
        if (lead <= 0x7F) return 1;
        if (lead >= 0xC2 && lead <= 0xDF) return within(1, 0x80, 0xBF) ? 2 : 0;
        if (lead == 0xE0) return within(1, 0xA0, 0xBF) && within(2, 0x80, 0xBF) ? 3 : 0;
        if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) return within(1, 0x80, 0xBF) && within(2, 0x80, 0xBF) ? 3 : 0;
        if (lead == 0xED) return within(1, 0x80, 0x9F) && within(2, 0x80, 0xBF) ? 3 : 0;
        if (lead == 0xF0) return within(1, 0x90, 0xBF) && within(2, 0x80, 0xBF) && within(3, 0x80, 0xBF) ? 4 : 0;
        if (lead >= 0xF1 && lead <= 0xF3) return within(1, 0x80, 0xBF) && within(2, 0x80, 0xBF) && within(3, 0x80, 0xBF) ? 4 : 0;
        if (lead == 0xF4) return within(1, 0x80, 0x8F) && within(2, 0x80, 0xBF) && within(3, 0x80, 0xBF) ? 4 : 0;
        return 0;
    }

    bool NaiveIsControl(const std::string& sequence)
    {
        if (sequence.size() == 1)
            return static_cast<u8>(sequence[0]) < 0x20 || static_cast<u8>(sequence[0]) == 0x7F;
        return sequence.size() == 2 && static_cast<u8>(sequence[0]) == 0xC2 && static_cast<u8>(sequence[1]) <= 0x9F;
    }

    std::vector<std::string> NaiveCodepoints(const std::string& text)
    {
        std::vector<std::string> result;
        for (size_t i = 0; i < text.size();)
        {
            const size_t length = NaiveSequenceLength(text, i);
            if (length == 0)
            {
                result.push_back("?");
                ++i;
                continue;
            }
            std::string sequence = text.substr(i, length);
            result.push_back(NaiveIsControl(sequence) ? " " : sequence);
            i += length;
        }
        return result;
    }

    std::string NaiveSanitize(const std::string& text, size_t maximum)
    {
        std::vector<std::string> points = NaiveCodepoints(text);
        while (!points.empty() && points.back() == " ")
            points.pop_back();
        size_t skip = 0;
        while (skip < points.size() && points[skip] == " ")
            ++skip;
        points.erase(points.begin(), points.begin() + static_cast<std::ptrdiff_t>(skip));

        std::string result;
        if (points.size() <= maximum)
        {
            for (const std::string& point : points)
                result += point;
            return result;
        }
        const size_t kept = maximum >= 3 ? maximum - 3 : 0;
        for (size_t i = 0; i < kept; ++i)
            result += points[i];
        return result + std::string(std::min<size_t>(maximum, 3), '.');
    }

    u64 NaiveFnv1a64(const std::string& bytes)
    {
        u64 hash = 0xcbf29ce484222325ull;
        for (const char byte : bytes)
        {
            hash ^= static_cast<u8>(byte);
            hash *= 0x100000001b3ull;
        }
        return hash;
    }
}

namespace SpiralTests
{
    bool TestHistoryLabelFormatting()
    {
        Checker c { "label" };

        c.Expect(HistoryLabel { "Move", "Cube", HistorySource::User }.Display() == "Move Cube", "verb and target");
        c.Expect(HistoryLabel { "Undo barrier", "", HistorySource::System }.Display() == "Undo barrier", "verb only");
        c.Expect(HistoryLabel { "", "Cube", HistorySource::User }.Display() == "Cube", "target only");
        c.Expect(std::string(HistorySourceName(HistorySource::User)) == "user"
                && std::string(HistorySourceName(HistorySource::Agent)) == "agent"
                && std::string(HistorySourceName(HistorySource::System)) == "system",
            "source words");

        c.Expect(NaiveFnv1a64("") == 0xcbf29ce484222325ull && NaiveFnv1a64("a") == 0xaf63dc4c8601ec8cull
                && NaiveFnv1a64("foobar") == 0x85944171f73967e8ull,
            "the independent FNV-1a reference matches the published vectors");
        const std::vector<HistoryLabel> labels {
            { "Move", "Cube", HistorySource::User }, { "Move", "Cube", HistorySource::Agent }, { "Mov", "eCube", HistorySource::User },
            { "Move", "", HistorySource::User }, { "", "", HistorySource::User }, { "Rename", "Light", HistorySource::System }
        };
        std::set<u64> ids;
        for (const HistoryLabel& label : labels)
        {
            const std::string bytes = label.Verb + "\x1f" + label.Target + "\x1f" + static_cast<char>(label.Source);
            c.Expect(label.StableId() == NaiveFnv1a64(bytes), "stable id for " + label.Display());
            ids.insert(label.StableId());
        }
        c.Expect(ids.size() == labels.size(), "stable ids differ by verb/target boundary and source");

        struct SanitizeCase { const char* Input; size_t Maximum; const char* Expected; };
        const std::vector<SanitizeCase> sanitize {
            { "Cube", 32, "Cube" },
            { "  padded \t name \n", 32, "padded   name" },
            { "line1\nline2", 32, "line1 line2" },
            { "abcdefghijklmnopqrstuvwxyz012345", 32, "abcdefghijklmnopqrstuvwxyz012345" },
            { "abcdefghijklmnopqrstuvwxyz0123456", 32, "abcdefghijklmnopqrstuvwxyz012..." },
            { "abcdef", 5, "ab..." },
            { "abcdef", 3, "..." },
            { "abcdef", 2, ".." },
            { "abcdef", 0, "" },
            { "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 5, "\xC3\xA9\xC3\xA9..." },
            { "\xE2\x82\xAC", 1, "\xE2\x82\xAC" },
            { "a\xFFz", 32, "a?z" },
            { "\xC0\x80", 32, "??" },
            { "\xED\xA0\x80", 32, "???" },
            { "\xE2\x82", 32, "??" },
            { "\xF0\x9F\x98\x80x", 32, "\xF0\x9F\x98\x80x" },
            { "a\xC2\x85z", 32, "a z" },
            { "   ", 32, "" },
        };
        for (const SanitizeCase& item : sanitize)
        {
            const std::string actual = SanitizeHistoryText(item.Input, item.Maximum);
            c.Expect(actual == item.Expected, std::string("sanitize '") + item.Input + "' max " + std::to_string(item.Maximum) + " gave '" + actual + "'");
        }

        const HistoryLabel built = MakeHistoryLabel("Move", std::string(100, 'x'), HistorySource::Agent);
        c.Expect(built.Target == std::string(29, 'x') + "..." && built.Source == HistorySource::Agent, "target truncated to 32 code points");
        c.Expect(MakeHistoryLabel(std::string(100, 'v'), "t").Verb == std::string(45, 'v') + "...", "verb truncated to 48 code points");

        struct ByteCase { u64 Bytes; const char* Expected; };
        const std::vector<ByteCase> bytes {
            { 0, "0 B" }, { 1, "1 B" }, { 1023, "1023 B" }, { 1024, "1 KiB" }, { 1029, "1 KiB" }, { 1030, "1.01 KiB" },
            { 1536, "1.5 KiB" }, { 1126, "1.1 KiB" }, { 1048576, "1 MiB" }, { 268435456, "256 MiB" },
            { 2415919104ull, "2.25 GiB" }, { 1073741824ull, "1 GiB" },
            { std::numeric_limits<u64>::max(), "17179869184 GiB" }
        };
        for (const ByteCase& item : bytes)
            c.Expect(FormatHistoryBytes(item.Bytes) == item.Expected, "bytes " + std::to_string(item.Bytes) + " gave " + FormatHistoryBytes(item.Bytes));

        c.Expect(FormatEvictionNotice(1, 0, 268435456, 512) == "1 oldest entry was dropped to stay within 256 MiB", "single budget eviction");
        c.Expect(FormatEvictionNotice(37, 0, 268435456, 512) == "37 oldest entries were dropped to stay within 256 MiB", "budget eviction");
        c.Expect(FormatEvictionNotice(0, 2, 268435456, 512) == "2 oldest entries were dropped to stay within 512 entries", "cap eviction");
        c.Expect(FormatEvictionNotice(0, 1, 268435456, 1) == "1 oldest entry was dropped to stay within 1 entry", "single cap eviction");
        c.Expect(FormatEvictionNotice(3, 2, 700, 8) == "5 oldest entries were dropped to stay within 700 B and 8 entries", "both reasons");
        c.Expect(FormatRedoDiscardedNotice(3) == "Redo history discarded (3)", "redo notice");

        c.Expect(EstimateStringHeapBytes(std::string()) == 0 && EstimateStringHeapBytes("short") == 0, "small strings own no heap");
        const std::string large(200, 'x');
        c.Expect(EstimateStringHeapBytes(large) == large.capacity() + 1 && large.capacity() >= 200, "large strings report capacity plus terminator");
        return c.Ok;
    }

    bool TestHistoryLabelSanitizeProperty()
    {
        const std::array<u8, 30> alphabet {
            'a', 'Z', ' ', '\t', '\n', 0x7F, 0xC2, 0x80, 0x9F, 0xA0, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80, 0xFF, 0xC0,
            0xC1, 0xED, 0xA0, 0xF4, 0x90, 0xF5, 0xE0, 0xBF, 0x8F, '.', '?'
        };
        const std::array<size_t, 8> maximums { 0, 1, 2, 3, 4, 5, 8, 32 };
        return RunProperty("HistoryLabelSanitize", [&](Spiral::Tests::ChoiceStream& stream, std::string& message)
        {
            std::string input;
            const size_t length = stream.NextSize(0, 64);
            for (size_t i = 0; i < length; ++i)
                input.push_back(static_cast<char>(alphabet[stream.NextSize(0, alphabet.size() - 1)]));
            const size_t maximum = maximums[stream.NextSize(0, maximums.size() - 1)];

            const std::string actual = SanitizeHistoryText(input, maximum);
            const std::string expected = NaiveSanitize(input, maximum);
            if (actual != expected)
            {
                message = "sanitized text differs from the naive model";
                return false;
            }
            size_t count = 0;
            for (size_t i = 0; i < actual.size(); ++count)
            {
                const size_t sequence = NaiveSequenceLength(actual, i);
                if (sequence == 0 || NaiveIsControl(actual.substr(i, sequence)))
                {
                    message = "output has an invalid sequence or a control character";
                    return false;
                }
                i += sequence;
            }
            if (count > maximum)
            {
                message = "output exceeds the code point limit";
                return false;
            }
            if (!actual.empty() && (actual.front() == ' ' || (actual.back() == ' ' )))
            {
                message = "output is not trimmed";
                return false;
            }
            if (SanitizeHistoryText(actual, maximum) != actual)
            {
                message = "sanitizing is not idempotent";
                return false;
            }
            return true;
        }, 512);
    }
}

namespace SpiralTests
{
    bool TestHistoryStoreAccountingEvictionAndInvalidation()
    {
        Checker c { "store-accounting" };

        // A. Shared snapshots are counted once, the estimator runs once per
        // distinct resident snapshot, and redo invalidation releases the tail.
        {
            TestAdapter adapter;
            std::vector<Ptr> s;
            for (u64 i = 0; i < 6; ++i)
                s.push_back(Make(i, 10 * (i + 1)));
            adapter.Live = s[0];
            Store store(adapter);

            c.Expect(store.HeadRevision() == 1 && store.UsedBytes() == 0 && store.Cursor() == 0, "fresh store: base revision 1, nothing used");
            for (size_t i = 0; i < 3; ++i)
            {
                adapter.Live = s[i + 1];
                const HistoryResult result = store.Record(EditLabel(i + 1), s[i], s[i + 1]);
                c.Expect(result.Status == HistoryStatus::Recorded && result.Revision == i + 2 && result.Head == i + 2, "record revision " + std::to_string(i + 1));
            }
            c.Expect(store.UsedBytes() == 10 + 20 + 30 + 40, "four distinct snapshots 10+20+30+40");
            c.Expect(store.TopUndo() && store.TopUndo()->Bytes == 30 + 40, "entry bytes are before plus after");
            for (size_t i = 0; i < 4; ++i)
                c.Expect(adapter.EstimateCalls[s[i].get()] == 1, "estimator called once for snapshot " + std::to_string(i));

            c.Expect(store.Undo().Succeeded() && store.Undo().Succeeded(), "two undos");
            c.Expect(adapter.Live == s[1] && store.Cursor() == 1 && store.RedoDepth() == 2, "undo twice restored snapshot 1");
            c.Expect(store.UsedBytes() == 100, "undo does not change accounting");

            adapter.Live = s[4];
            const HistoryResult edit = store.Record(EditLabel(4), s[1], s[4]);
            c.Expect(edit.Status == HistoryStatus::Recorded && edit.RedoDiscarded == 2 && edit.Announcement == "Redo history discarded (2)",
                "a new edit discards the redo tail and says so");
            c.Expect(store.RedoDepth() == 0 && store.TopRedo() == nullptr && store.EntryCount() == 2, "redo stack is empty");
            c.Expect(store.UsedBytes() == 10 + 20 + 50, "snapshots 2 and 3 were released: 10+20+50");
            c.Expect(store.Redo().Status == HistoryStatus::NothingToRedo && !store.RedoAvailability().Enabled, "redo is unavailable");

            adapter.Live = s[2];
            c.Expect(store.Record(EditLabel(5), s[4], s[2]).Succeeded() && adapter.EstimateCalls[s[2].get()] == 2, "a released snapshot is estimated again when it returns");
            c.Expect(store.UsedBytes() == 10 + 20 + 50 + 30, "used after re-adding snapshot 2");

            c.Expect(store.Record(EditLabel(6), nullptr, s[1]).Status == HistoryStatus::InvalidArgument, "null before rejected");
            c.Expect(store.Record(EditLabel(6), s[1], nullptr).Status == HistoryStatus::InvalidArgument, "null after rejected");
            c.Expect(store.Record(EditLabel(6), s[1], s[1]).Status == HistoryStatus::NoChange && store.EntryCount() == 3, "identical snapshots record nothing");
        }

        // B. Exact eviction order, revisions, notices, and rows.
        {
            TestAdapter adapter;
            std::vector<Ptr> s;
            for (u64 i = 0; i < 8; ++i)
                s.push_back(Make(i, 30));
            adapter.Live = s[0];
            Store store(adapter, HistoryConfig { 100, 512 });

            const HistoryResult e1 = store.Record(EditLabel(1), s[0], s[1]);
            const HistoryResult e2 = store.Record(EditLabel(2), s[1], s[2]);
            c.Expect(e1.Revision == 2 && e2.Revision == 3 && store.UsedBytes() == 90 && e2.Evicted.Entries() == 0, "no eviction at 90 of 100");
            const HistoryResult e3 = store.Record(EditLabel(3), s[2], s[3]);
            c.Expect(e3.Evicted.ForBudget == 1 && e3.Evicted.ForEntryCap == 0 && e3.Evicted.Bytes == 30, "third record drops exactly the oldest entry freeing 30");
            c.Expect(e3.Revision == 5, "base took revision 4 before the new entry took 5");
            c.Expect(e3.Announcement == "1 oldest entry was dropped to stay within 100 B", "eviction announcement: " + e3.Announcement);
            c.Expect(store.EntryCount() == 2 && store.UsedBytes() == 90 && store.Evicted().Entries == 1 && store.Evicted().Bytes == 30 && store.Evicted().Events == 1,
                "eviction stats");

            const std::vector<HistoryRow> rows = store.Rows();
            c.Expect(rows.size() == 3, "base row plus two survivors");
            if (rows.size() != 3)
                return false;
            c.Expect(rows[0].IsBase && rows[0].Display == "Earlier history dropped" && rows[0].Revision == 4 && !rows[0].IsBarrier
                    && rows[0].Label.Source == HistorySource::System,
                "base row after eviction");
            c.Expect(rows[1].Display == "Edit T2" && rows[1].Revision == 3 && rows[2].Display == "Edit T3" && rows[2].Revision == 5, "surviving rows are the newest two");
            c.Expect(rows[2].Current && rows[1].Applied && !rows[1].Current && rows[0].Applied, "markers");

            c.Expect(store.Undo().Succeeded() && store.Undo().Succeeded(), "undo both survivors");
            c.Expect(adapter.Live == s[1] && store.HeadRevision() == 4, "state at the new base is the oldest survivor's Before; head is the base revision");
            const HistoryResult blocked = store.Undo();
            c.Expect(blocked.Status == HistoryStatus::NothingToUndo
                    && blocked.Announcement == "Nothing to undo: earlier history was dropped to stay within the history limit",
                "undo past dropped history: " + blocked.Announcement);
            c.Expect(!store.BaseIsBarrier(), "dropped history is not a barrier");
        }

        // C. The newest entry is kept even when it alone exceeds the budget.
        {
            TestAdapter adapter;
            std::vector<Ptr> s;
            for (u64 i = 0; i < 5; ++i)
                s.push_back(Make(i, 30));
            adapter.Live = s[0];
            Store store(adapter, HistoryConfig { 10, 512 });
            c.Expect(store.Record(EditLabel(1), s[0], s[1]).Evicted.Entries() == 0 && store.EntryCount() == 1 && store.UsedBytes() == 60, "single oversize entry kept");
            const HistoryResult second = store.Record(EditLabel(2), s[1], s[2]);
            c.Expect(second.Evicted.ForBudget == 1 && second.Evicted.Bytes == 30 && store.EntryCount() == 1 && store.UsedBytes() == 60, "second record evicts only the first");
            c.Expect(store.TopUndo()->Label.Target == "T2", "newest survives");
            Store zero(adapter, HistoryConfig { 0, 0 });
            c.Expect(zero.Config().MaximumEntries == 1, "a zero entry cap is clamped to one");
            c.Expect(zero.Record(EditLabel(1), s[0], s[1]).Succeeded() && zero.Record(EditLabel(2), s[1], s[2]).Succeeded() && zero.EntryCount() == 1, "budget zero and cap one keep the newest only");
        }

        // D. Entry cap and exact budget boundaries.
        for (const size_t cap : { size_t(1), size_t(2), size_t(3), size_t(8) })
        {
            for (const size_t count : { size_t(0), cap - 1, cap, cap + 1, cap + 5 })
            {
                TestAdapter adapter;
                std::vector<Ptr> s;
                for (u64 i = 0; i <= count; ++i)
                    s.push_back(Make(i, 1));
                adapter.Live = s[0];
                Store store(adapter, HistoryConfig { kDefaultHistoryBudgetBytes, cap });
                size_t cappedNotices = 0;
                for (size_t i = 0; i < count; ++i)
                    cappedNotices += store.Record(EditLabel(i + 1), s[i], s[i + 1]).Evicted.ForEntryCap;
                const size_t kept = std::min(count, cap);
                const std::string where = " cap " + std::to_string(cap) + " count " + std::to_string(count);
                c.Expect(store.EntryCount() == kept && cappedNotices == count - kept && store.Evicted().Entries == count - kept, "entry count" + where);
                if (count != 0 && store.Rows().size() > 1 && store.TopUndo())
                    c.Expect(store.Rows()[1].Label.Target == "T" + std::to_string(count - kept + 1) && store.TopUndo()->Label.Target == "T" + std::to_string(count),
                        "oldest evicted first" + where);
                c.Expect(store.UsedBytes() == (count == 0 ? 0 : kept + 1), "bytes track the survivors" + where);
            }
        }
        for (const bool exact : { true, false })
        {
            TestAdapter adapter;
            std::vector<Ptr> s;
            for (u64 i = 0; i < 4; ++i)
                s.push_back(Make(i, 10));
            adapter.Live = s[0];
            Store store(adapter, HistoryConfig { exact ? 40ull : 39ull, 512 });
            for (size_t i = 0; i < 3; ++i)
                store.Record(EditLabel(i + 1), s[i], s[i + 1]);
            c.Expect(store.Evicted().Entries == (exact ? 0u : 1u), exact ? "budget exactly equal to usage does not evict" : "one byte over evicts");
        }

        // E. Redo discard and eviction announcements join.
        {
            TestAdapter adapter;
            std::vector<Ptr> s;
            for (u64 i = 0; i < 6; ++i)
                s.push_back(Make(i, 10));
            adapter.Live = s[0];
            Store store(adapter, HistoryConfig { 60, 512 });
            for (size_t i = 0; i < 4; ++i)
                store.Record(EditLabel(i + 1), s[i], s[i + 1]);
            store.Undo();
            store.Undo();
            const Ptr fresh = Make(99, 10);
            adapter.Live = fresh;
            const HistoryResult result = store.Record(EditLabel(9), s[2], fresh);
            c.Expect(result.RedoDiscarded == 2 && result.Evicted.ForBudget == 0 && result.Announcement == "Redo history discarded (2)", "discard without eviction");
            const Ptr big = Make(100, 40);
            adapter.Live = big;
            const HistoryResult both = store.Record(EditLabel(10), fresh, big);
            c.Expect(both.Evicted.ForBudget == 2 && both.Evicted.Bytes == 20 && store.UsedBytes() == 60 && both.Announcement.find("; ") == std::string::npos && both.Announcement.find("oldest") != std::string::npos,
                "eviction announcement present");
        }
        return c.Ok;
    }

    bool TestHistoryStoreBarrierJumpResetAndRestoreFailure()
    {
        Checker c { "store-navigation" };
        TestAdapter adapter;
        std::vector<Ptr> s;
        for (u64 i = 0; i < 8; ++i)
            s.push_back(Make(i, 10));
        adapter.Live = s[0];
        Store store(adapter);

        // Menu text with nothing recorded.
        c.Expect(store.UndoAvailability().MenuLabel == "Undo (nothing to undo)" && store.UndoAvailability().Block == HistoryBlock::NothingToUndo
                && store.UndoAvailability().Detail == "Nothing to undo" && !store.UndoAvailability().Enabled,
            "empty undo availability");
        c.Expect(store.RedoAvailability().MenuLabel == "Redo (nothing to redo)" && store.RedoAvailability().Detail == "Nothing to redo", "empty redo availability");
        c.Expect(store.Undo().Status == HistoryStatus::NothingToUndo && store.Undo().Announcement == "Nothing to undo" && adapter.RestoreCalls == 0, "undo with nothing does not restore");

        for (size_t i = 0; i < 5; ++i)
        {
            adapter.Live = s[i + 1];
            store.Record(EditLabel(i + 1), s[i], s[i + 1]);
        }
        c.Expect(store.UndoAvailability().MenuLabel == "Undo Edit T5" && store.UndoAvailability().Enabled && store.UndoAvailability().Detail.empty(), "undo menu names the entry");
        c.Expect(store.RedoAvailability().Enabled == false, "no redo yet");
        const HistoryResult undo = store.Undo();
        c.Expect(undo.Announcement == "Undo: Edit T5" && undo.Steps == 1 && undo.Label.Display() == "Edit T5" && undo.Revision == 6 && undo.Head == 5, "undo announcement and result");
        c.Expect(store.RedoAvailability().MenuLabel == "Redo Edit T5", "redo menu names the entry");
        const HistoryResult redo = store.Redo();
        c.Expect(redo.Announcement == "Redo: Edit T5" && adapter.Live == s[5] && store.Cursor() == 5, "redo announcement and state");

        // Jump: one restore per jump, in both directions.
        const std::vector<HistoryRow> rows = store.Rows();
        c.Expect(rows.size() == 6, "base plus five entries");
        if (rows.size() != 6)
            return false;
        const size_t callsBefore = adapter.RestoreCalls;
        const HistoryResult back = store.JumpTo(rows[2].Revision);
        c.Expect(back.Succeeded() && back.Steps == 3 && adapter.RestoreCalls == callsBefore + 1 && adapter.Live == s[2] && store.Cursor() == 2,
            "jump back three entries is one restore of the entry's Before");
        c.Expect(back.Announcement == "Jumped to: Edit T2 (3 steps back)", "jump back announcement: " + back.Announcement);
        const HistoryResult forward = store.JumpTo(rows[4].Revision);
        c.Expect(forward.Succeeded() && forward.Steps == 2 && adapter.Live == s[4] && store.Cursor() == 4 && adapter.RestoreCalls == callsBefore + 2,
            "jump forward restores the After of the previous entry");
        c.Expect(forward.Announcement == "Jumped to: Edit T4 (2 steps forward)", "jump forward announcement: " + forward.Announcement);
        c.Expect(store.JumpTo(rows[1].Revision).Announcement == "Jumped to: Edit T1 (3 steps back)" && adapter.Live == s[1], "jump to first entry");
        const HistoryResult toBase = store.JumpTo(rows[0].Revision);
        c.Expect(toBase.Succeeded() && toBase.Steps == 1 && adapter.Live == s[0] && store.Cursor() == 0 && toBase.Announcement == "Jumped to: Project opened (1 step back)",
            "jump to the base row: " + toBase.Announcement);
        const size_t calls = adapter.RestoreCalls;
        c.Expect(store.JumpTo(rows[0].Revision).Status == HistoryStatus::NoChange && adapter.RestoreCalls == calls, "jump to the current row restores nothing");
        c.Expect(store.JumpTo(0).Status == HistoryStatus::UnknownRevision && store.JumpTo(9999).Status == HistoryStatus::UnknownRevision
                && adapter.RestoreCalls == calls && store.Cursor() == 0,
            "unknown revisions change nothing");
        c.Expect(store.JumpTo(rows[5].Revision).Succeeded() && adapter.Live == s[5] && store.Cursor() == 5, "jump from the base to the newest entry");
        c.Expect(store.JumpToRow(2).Succeeded() && adapter.Live == s[2] && store.Cursor() == 2 && store.JumpToRow(2).Status == HistoryStatus::NoChange, "jump by row index");
        c.Expect(store.JumpToRow(6).Status == HistoryStatus::UnknownRevision && store.JumpToRow(1000).Status == HistoryStatus::UnknownRevision && store.Cursor() == 2,
            "a row index past the end is unknown");
        c.Expect(store.JumpToRow(5).Succeeded() && store.Cursor() == 5, "back to the newest row");

        // Restore failure leaves history and live state untouched.
        {
            const Ptr liveBefore = adapter.Live;
            const HistoryRevision head = store.HeadRevision();
            adapter.FailRestore = true;
            c.Expect(store.Undo().Status == HistoryStatus::RestoreFailed, "undo restore failure");
            store.JumpTo(rows[1].Revision);
            c.Expect(store.JumpTo(rows[1].Revision).Status == HistoryStatus::RestoreFailed, "jump restore failure");
            adapter.FailRestore = false;
            c.Expect(store.Undo().Succeeded(), "undo works again");
            adapter.FailRestore = true;
            c.Expect(store.Redo().Status == HistoryStatus::RestoreFailed, "redo restore failure");
            adapter.FailRestore = false;
            c.Expect(store.Redo().Succeeded() && adapter.Live == liveBefore && store.HeadRevision() == head && store.Cursor() == 5, "failed restores moved nothing");
        }

        // Barrier: both stacks cleared, undo blocked with a reason, redo gone.
        store.Undo();
        store.Undo();
        c.Expect(store.RedoDepth() == 2, "two redo entries before the barrier");
        const HistoryRevision beforeBarrier = store.HeadRevision();
        const HistoryResult barrier = store.Barrier(MakeHistoryLabel("Import Fab Asset", "", HistorySource::System), "project changes were committed to disk");
        c.Expect(barrier.Succeeded() && store.UndoDepth() == 0 && store.RedoDepth() == 0 && store.EntryCount() == 0 && store.UsedBytes() == 0, "barrier cleared both stacks and accounting");
        c.Expect(store.HeadRevision() > beforeBarrier && store.BaseIsBarrier(), "barrier takes a fresh revision");
        const HistoryAvailability blocked = store.UndoAvailability();
        c.Expect(!blocked.Enabled && blocked.Block == HistoryBlock::UndoBarrier && blocked.MenuLabel == "Undo (blocked: Import Fab Asset)"
                && blocked.Detail == "Cannot undo past: Import Fab Asset - project changes were committed to disk",
            "barrier reason: " + blocked.Detail);
        const size_t restoreCalls = adapter.RestoreCalls;
        const HistoryResult refused = store.Undo();
        c.Expect(refused.Status == HistoryStatus::UndoBarrier && refused.Announcement == blocked.Detail && adapter.RestoreCalls == restoreCalls, "undo past a barrier restores nothing");
        c.Expect(store.Redo().Status == HistoryStatus::NothingToRedo, "no redo after a barrier");
        c.Expect(store.Rows().size() == 1 && store.Rows()[0].Display == "Import Fab Asset (barrier)" && store.Rows()[0].IsBarrier && store.Rows()[0].Current,
            "barrier base row text");
        adapter.Live = s[6];
        c.Expect(store.Record(EditLabel(1), s[5], s[6]).Succeeded() && store.Undo().Succeeded() && adapter.Live == s[5], "entries after a barrier undo normally");
        c.Expect(store.Undo().Status == HistoryStatus::UndoBarrier, "but not past the barrier");
        c.Expect(store.JumpTo(store.Rows()[0].Revision).Status == HistoryStatus::NoChange, "the barrier row is the current row");

        // Barrier is rejected while an edit is open and changes nothing.
        EditGestureKey key { 1, 2, 3 };
        store.BeginGesture(key, EditLabel(7), s[5]);
        c.Expect(store.Barrier(MakeHistoryLabel("X", ""), "r").Status == HistoryStatus::GestureOpen && store.EntryCount() == 1 && store.BaseIsBarrier(), "barrier rejected during a gesture");
        store.CancelGesture();

        // Reset re-bases and discards open edits.
        store.BeginTransaction(EditLabel(8), s[5]);
        const HistoryRevision beforeReset = store.HeadRevision();
        c.Expect(store.Reset(MakeHistoryLabel("Project opened", "Alpha", HistorySource::System)).Succeeded(), "reset");
        c.Expect(store.EntryCount() == 0 && store.TransactionDepth() == 0 && !store.GestureOpen() && store.UsedBytes() == 0 && store.Evicted().Entries == 0
                && !store.BaseIsBarrier() && store.HeadRevision() > beforeReset && store.Rows()[0].Display == "Project opened Alpha",
            "reset state");
        c.Expect(store.UndoAvailability().Detail == "Nothing to undo", "reset base is a plain start");

        // Marks: a pointer copy that puts the history back exactly, including
        // accounting and eviction stats, without touching live state.
        {
            TestAdapter markAdapter;
            std::vector<Ptr> m;
            for (u64 i = 0; i < 12; ++i)
                m.push_back(Make(i, 10));
            markAdapter.Live = m[0];
            Store marked(markAdapter, HistoryConfig { 50, 512 });
            c.Expect(!marked.LoadMark(Store::Mark {}), "a default mark is invalid");
            for (size_t i = 0; i < 3; ++i)
                marked.Record(EditLabel(i + 1), m[i], m[i + 1]);
            marked.Undo();
            const Store::Mark mark = marked.SaveMark();
            const std::vector<HistoryRow> savedRows = marked.Rows();
            const u64 savedUsed = marked.UsedBytes();
            const HistoryRevision savedHead = marked.HeadRevision();
            c.Expect(mark.Valid, "mark saved");
            for (size_t i = 3; i < 9; ++i)
                marked.Record(EditLabel(i + 1), m[i], m[i + 1]);
            c.Expect(marked.Evicted().Entries > 0 && marked.RedoDepth() == 0, "later edits discarded redo and evicted");
            c.Expect(marked.LoadMark(mark), "load");
            const std::vector<HistoryRow> restoredRows = marked.Rows();
            bool same = restoredRows.size() == savedRows.size();
            for (size_t i = 0; same && i < restoredRows.size(); ++i)
                same = restoredRows[i].Revision == savedRows[i].Revision && restoredRows[i].Display == savedRows[i].Display
                    && restoredRows[i].Current == savedRows[i].Current;
            c.Expect(same && marked.UsedBytes() == savedUsed && marked.HeadRevision() == savedHead && marked.RedoDepth() == 1 && marked.Evicted().Entries == 0,
                "load puts back rows, accounting, head, redo tail, and eviction stats");
            c.Expect(marked.Redo().Succeeded() && markAdapter.Live == m[3], "restored history is usable");
            marked.BeginGesture(EditGestureKey { 1, 1, 1 }, EditLabel(1), markAdapter.Live);
            c.Expect(!marked.LoadMark(mark) && !marked.SaveMark().Valid, "no mark load or save while an edit is open");
            marked.CancelGesture();
            const HistoryRevision revisionBefore = marked.HeadRevision();
            marked.Record(EditLabel(20), m[3], m[10]);
            c.Expect(marked.HeadRevision() > revisionBefore && marked.HeadRevision() > savedHead, "revisions keep increasing after a load");
            marked.LoadMark(mark);
            marked.Record(EditLabel(21), m[2], m[11]);
            c.Expect(marked.TopUndo()->Revision > revisionBefore + 1, "a revision is never reused after rolling back");
        }
        return c.Ok;
    }
}

namespace SpiralTests
{
    bool TestHistoryStoreGesturesAndTransactions()
    {
        Checker c { "store-gestures" };
        const EditGestureKey keyA { 11, 100, 1 };

        // A thousand in-place updates are one entry whose Before was captured
        // before the first edit.
        {
            TestAdapter adapter;
            const Ptr original = Make(1, 8);
            adapter.Live = original;
            Store store(adapter);
            c.Expect(store.BeginGesture(keyA, MakeHistoryLabel("Move", "Cube"), adapter.Live).Succeeded() && store.GestureOpen(), "begin");
            for (u64 i = 0; i < 1000; ++i)
            {
                adapter.Live = Make(2 + i, 8);
                c.Expect(store.UpdateGesture(keyA).Succeeded(), "update " + std::to_string(i));
            }
            c.Expect(store.GestureUpdates() == 1000 && store.EntryCount() == 0 && store.UsedBytes() == 0 && store.HeadRevision() == 1, "nothing is recorded while the gesture is open");
            c.Expect(store.Undo().Status == HistoryStatus::GestureOpen && store.Redo().Status == HistoryStatus::GestureOpen
                    && store.UndoAvailability().MenuLabel == "Undo (finish the current edit)" && store.RedoAvailability().Detail == "Cannot redo: finish the current edit",
                "undo and redo are blocked with a reason during a gesture");
            c.Expect(store.Record(EditLabel(1), original, adapter.Live).Status == HistoryStatus::GestureOpen && store.JumpTo(1).Status == HistoryStatus::GestureOpen,
                "record and jump are rejected during a gesture");
            c.Expect(!store.SaveMark().Valid, "no mark while an edit is open");
            const Ptr last = adapter.Live;
            const HistoryResult end = store.EndGesture(keyA, last);
            c.Expect(end.Status == HistoryStatus::Recorded && end.Label.Display() == "Move Cube" && store.EntryCount() == 1 && !store.GestureOpen(),
                "1000 updates are one entry");
            c.Expect(store.UsedBytes() == 16, "entry holds Before and After");
            c.Expect(store.Undo().Succeeded() && adapter.Live == original, "undo restores the exact Before pointer");
            c.Expect(store.Redo().Succeeded() && adapter.Live == last, "redo restores the exact After pointer");
        }

        // No change: the gesture closes, records nothing, keeps the redo tail.
        {
            TestAdapter adapter;
            const Ptr s0 = Make(1, 1);
            const Ptr s1 = Make(2, 1);
            adapter.Live = s0;
            Store store(adapter);
            store.Record(EditLabel(1), s0, s1);
            adapter.Live = s1;
            store.Undo();
            c.Expect(store.RedoDepth() == 1, "one redo entry");
            store.BeginGesture(keyA, MakeHistoryLabel("Move", "Cube"), adapter.Live);
            adapter.Live = Make(1, 1);
            const HistoryResult end = store.EndGesture(keyA, adapter.Live);
            c.Expect(end.Status == HistoryStatus::NoChange && !store.GestureOpen() && store.EntryCount() == 1 && store.RedoDepth() == 1, "equal After records nothing and keeps redo");
        }

        // Cancel restores Before exactly and records nothing.
        {
            TestAdapter adapter;
            const Ptr original = Make(1, 1);
            adapter.Live = original;
            Store store(adapter);
            store.BeginGesture(keyA, MakeHistoryLabel("Move", "Cube"), original);
            adapter.Live = Make(2, 1);
            store.UpdateGesture(keyA);
            c.Expect(store.CancelGesture().Succeeded() && adapter.Live == original && !store.GestureOpen() && store.EntryCount() == 0, "cancel restores the Before pointer");
            c.Expect(store.CancelGesture().Status == HistoryStatus::NoGesture, "cancel with nothing open");
            adapter.FailRestore = true;
            store.BeginGesture(keyA, MakeHistoryLabel("Move", "Cube"), original);
            c.Expect(store.CancelGesture().Status == HistoryStatus::RestoreFailed && store.GestureOpen(), "a failed cancel leaves the gesture open");
            adapter.FailRestore = false;
            c.Expect(store.CancelGesture().Succeeded() && !store.GestureOpen(), "retry cancel");
        }

        // Interleaved gestures are rejected and the open one is untouched.
        {
            TestAdapter adapter;
            const Ptr s0 = Make(1, 1);
            adapter.Live = s0;
            Store store(adapter);
            store.BeginGesture(keyA, MakeHistoryLabel("Move", "Cube"), s0);
            const std::vector<EditGestureKey> others { { 12, 100, 1 }, { 11, 101, 1 }, { 11, 100, 2 } };
            for (const EditGestureKey& other : others)
            {
                c.Expect(store.BeginGesture(other, MakeHistoryLabel("Scale", "Cube"), s0).Status == HistoryStatus::GestureOpen, "second begin rejected");
                c.Expect(store.UpdateGesture(other).Status == HistoryStatus::KeyMismatch, "foreign update rejected");
                c.Expect(store.EndGesture(other, Make(2, 1)).Status == HistoryStatus::KeyMismatch && store.GestureOpen() && *store.OpenGestureKey() == keyA,
                    "foreign end rejected, gesture still open");
            }
            c.Expect(store.BeginTransaction(EditLabel(1), s0).Status == HistoryStatus::GestureOpen, "no transaction inside a gesture");
            c.Expect(store.EndGesture(keyA, Make(2, 1)).Status == HistoryStatus::Recorded && store.UpdateGesture(keyA).Status == HistoryStatus::NoGesture
                    && store.EndGesture(keyA, s0).Status == HistoryStatus::NoGesture,
                "the owner key ends it; nothing is open afterwards");
            c.Expect(store.BeginGesture(keyA, MakeHistoryLabel("Move", "Cube"), nullptr).Status == HistoryStatus::InvalidArgument, "null before rejected");
        }

        // Label refinement lands in the recorded entry.
        {
            TestAdapter adapter;
            const Ptr s0 = Make(1, 1);
            adapter.Live = s0;
            Store store(adapter);
            c.Expect(!store.RefineOpenLabel(MakeHistoryLabel("x", "")), "nothing to refine");
            store.BeginGesture(keyA, MakeHistoryLabel("Move", ""), s0);
            c.Expect(store.RefineOpenLabel(MakeHistoryLabel("Move", "Cube")), "refine gesture");
            const HistoryResult end = store.EndGesture(keyA, Make(2, 1));
            c.Expect(end.Label.Display() == "Move Cube" && store.TopUndo()->Label.Display() == "Move Cube", "refined label recorded");
        }

        // Transactions: nesting records one entry with the outermost label.
        {
            TestAdapter adapter;
            const Ptr s0 = Make(1, 1);
            const Ptr s1 = Make(2, 1);
            const Ptr s2 = Make(3, 1);
            const Ptr s3 = Make(4, 1);
            adapter.Live = s0;
            Store store(adapter);
            c.Expect(store.BeginTransaction(MakeHistoryLabel("Create", "Entity 2"), s0).Succeeded(), "outer");
            adapter.Live = s1;
            c.Expect(store.BeginTransaction(MakeHistoryLabel("Assign", "Mesh"), s1).Succeeded() && store.BeginTransaction(MakeHistoryLabel("Select", ""), s1).Succeeded()
                    && store.TransactionDepth() == 3,
                "nested levels");
            c.Expect(store.Record(EditLabel(1), s1, s2).Status == HistoryStatus::Absorbed && store.EntryCount() == 0, "inner record is absorbed");
            c.Expect(store.Undo().Status == HistoryStatus::TransactionOpen && store.UndoAvailability().MenuLabel == "Undo (finish the current edit)", "undo blocked in a transaction");
            c.Expect(store.BeginGesture(keyA, EditLabel(1), s1).Status == HistoryStatus::TransactionOpen, "no gesture inside a transaction");
            c.Expect(store.Barrier(MakeHistoryLabel("X", ""), "r").Status == HistoryStatus::TransactionOpen, "no barrier inside a transaction");
            adapter.Live = s2;
            c.Expect(store.EndTransaction(s2).Succeeded() && store.EndTransaction(s2).Succeeded() && store.EntryCount() == 0 && store.TransactionDepth() == 1, "inner levels close without entries");
            adapter.Live = s3;
            const HistoryResult done = store.EndTransaction(s3);
            c.Expect(done.Status == HistoryStatus::Recorded && done.Label.Display() == "Create Entity 2" && store.EntryCount() == 1 && store.TransactionDepth() == 0,
                "outermost close records once");
            c.Expect(store.Undo().Succeeded() && adapter.Live == s0, "one undo reverts the whole transaction to the outermost Before");
            c.Expect(store.EndTransaction(s3).Status == HistoryStatus::NoTransaction && store.AbortTransaction().Status == HistoryStatus::NoTransaction, "unbalanced end");
        }

        // Aborting an inner level restores that level's Before only.
        {
            TestAdapter adapter;
            const Ptr s0 = Make(1, 1);
            const Ptr s1 = Make(2, 1);
            const Ptr s2 = Make(3, 1);
            const Ptr s3 = Make(4, 1);
            adapter.Live = s0;
            Store store(adapter);
            store.BeginTransaction(MakeHistoryLabel("Outer", ""), s0);
            adapter.Live = s1;
            store.BeginTransaction(MakeHistoryLabel("Inner", ""), s1);
            adapter.Live = s2;
            adapter.FailRestore = true;
            c.Expect(store.AbortTransaction().Status == HistoryStatus::RestoreFailed && store.TransactionDepth() == 2 && adapter.Live == s2, "failed abort leaves the level open");
            adapter.FailRestore = false;
            c.Expect(store.AbortTransaction().Succeeded() && adapter.Live == s1 && store.TransactionDepth() == 1, "inner abort restores the inner Before");
            adapter.Live = s3;
            const HistoryResult done = store.EndTransaction(s3);
            c.Expect(done.Label.Display() == "Outer" && store.EntryCount() == 1, "outer still records");
            store.Undo();
            c.Expect(adapter.Live == s0, "entry Before is the outer Before");
            store.BeginTransaction(MakeHistoryLabel("Again", ""), s0);
            adapter.Live = s1;
            c.Expect(store.AbortTransaction().Succeeded() && adapter.Live == s0 && store.TransactionDepth() == 0 && store.RedoDepth() == 1, "outer abort restores and records nothing; redo kept");
            store.BeginTransaction(MakeHistoryLabel("Same", ""), s0);
            c.Expect(store.EndTransaction(Make(1, 1)).Status == HistoryStatus::NoChange && store.RedoDepth() == 1, "an unchanged transaction records nothing");
            c.Expect(store.RefineOpenLabel(MakeHistoryLabel("x", "")) == false, "refine with nothing open");
        }

        // Budget eviction on a gesture end reports through the result.
        {
            TestAdapter adapter;
            std::vector<Ptr> s;
            for (u64 i = 0; i < 6; ++i)
                s.push_back(Make(i, 10));
            adapter.Live = s[0];
            Store store(adapter, HistoryConfig { 30, 512 });
            store.Record(EditLabel(1), s[0], s[1]);
            store.BeginGesture(keyA, MakeHistoryLabel("Move", "Cube"), s[1]);
            store.EndGesture(keyA, s[2]);
            store.BeginGesture(keyA, MakeHistoryLabel("Move", "Cube"), s[2]);
            const HistoryResult end = store.EndGesture(keyA, s[3]);
            c.Expect(end.Status == HistoryStatus::Recorded && end.Evicted.ForBudget >= 1 && store.UsedBytes() <= 30, "gesture end evicts");
        }
        return c.Ok;
    }

    namespace
    {
        EditItemFrame Frame(const EditGestureKey& key, bool activated, bool edited, bool deactivated, bool deactivatedAfterEdit)
        {
            return { key, activated, edited, deactivated, deactivatedAfterEdit };
        }

        // Reference for the tracker written as intervals over the whole stream:
        // pass one finds where each gesture opens and closes, pass two reads the
        // answer for every frame. It does not step a state machine.
        struct ReferenceStep
        {
            EditGestureAction Action = EditGestureAction::None;
            EditGestureKey Key;
            bool Edited = false;
        };

        std::vector<ReferenceStep> ReferenceTracker(const std::vector<EditItemFrame>& frames)
        {
            const auto released = [](const EditItemFrame& f) { return f.Deactivated || f.DeactivatedAfterEdit; };
            const auto changed = [](const EditItemFrame& f) { return f.Edited || f.DeactivatedAfterEdit; };
            struct Span { size_t Start; size_t End; };
            std::vector<Span> spans;
            std::vector<ReferenceStep> out(frames.size());

            size_t i = 0;
            while (i < frames.size())
            {
                const EditItemFrame& frame = frames[i];
                if (!frame.Activated || released(frame))
                {
                    if ((frame.Activated && released(frame) && changed(frame)) || (!frame.Activated && frame.Edited))
                        out[i] = { EditGestureAction::Discrete, frame.Key, true };
                    ++i;
                    continue;
                }
                // The gesture opens at i; find its closing frame: the first later
                // frame of the same key that releases.
                size_t close = frames.size();
                for (size_t j = i + 1; j < frames.size(); ++j)
                {
                    if (frames[j].Key == frame.Key && released(frames[j]))
                    {
                        close = j;
                        break;
                    }
                }
                spans.push_back({ i, close });
                bool edited = frame.Edited;
                for (size_t j = i + 1; j < close; ++j)
                {
                    if (frames[j].Key == frame.Key)
                        edited = edited || changed(frames[j]);
                    else if (frames[j].Activated || frames[j].Edited)
                        out[j] = { EditGestureAction::Rejected, frames[j].Key, false };
                }
                out[i] = { EditGestureAction::Begin, frame.Key, frame.Edited };
                if (close < frames.size())
                {
                    edited = edited || changed(frames[close]);
                    out[close] = { EditGestureAction::End, frame.Key, edited };
                }
                i = close < frames.size() ? close + 1 : frames.size();
            }
            return out;
        }

        // Applies tracker results to a store the way the Inspector will: capture
        // Before at Begin, then apply the edit; End records.
        struct Driver
        {
            TestAdapter& Adapter;
            Store& History;
            EditGestureTracker Tracker;
            u64 NextId = 1000;
            size_t BeginCalls = 0;

            void ApplyEdit()
            {
                Adapter.Live = Make(NextId++, 1);
            }

            void Handle(const EditGestureResult& result, bool applyEdit)
            {
                switch (result.Action)
                {
                case EditGestureAction::Begin:
                    History.BeginGesture(result.Key, MakeHistoryLabel("Edit", "Item" + std::to_string(result.Key.Item)), Adapter.Live);
                    ++BeginCalls;
                    if (applyEdit)
                        ApplyEdit();
                    break;
                case EditGestureAction::End:
                    History.EndGesture(result.Key, Adapter.Live);
                    break;
                case EditGestureAction::Cancel:
                    History.CancelGesture();
                    break;
                case EditGestureAction::Discrete:
                case EditGestureAction::Rejected:
                case EditGestureAction::None:
                    break;
                }
            }
        };
    }

    bool TestHistoryEditGestureTracker()
    {
        Checker c { "gesture-tracker" };
        const EditGestureKey a { 1, 10, 1 };
        const EditGestureKey b { 2, 10, 2 };

        // A drag: activation with the first edit, a thousand edits, release.
        {
            TestAdapter adapter;
            adapter.Live = Make(1, 1);
            const Ptr original = adapter.Live;
            Store store(adapter);
            Driver driver { adapter, store, {}, 2000, 0 };
            const EditGestureResult begin = driver.Tracker.OnItem(Frame(a, true, true, false, false));
            c.Expect(begin.Action == EditGestureAction::Begin && begin.Key == a && begin.Edited, "activation with an edit begins");
            driver.Handle(begin, true);
            for (int i = 0; i < 1000; ++i)
            {
                c.Expect(driver.Tracker.OnItem(Frame(a, false, true, false, false)).Action == EditGestureAction::None, "updates produce no action");
                driver.ApplyEdit();
                store.UpdateGesture(a);
            }
            const EditGestureResult end = driver.Tracker.OnItem(Frame(a, false, false, true, true));
            c.Expect(end.Action == EditGestureAction::End && end.Key == a && end.Edited && !driver.Tracker.Open(), "release after edit ends");
            driver.Handle(end, false);
            c.Expect(store.EntryCount() == 1 && driver.BeginCalls == 1, "a drag is one entry");
            c.Expect(store.Undo().Succeeded() && adapter.Live == original, "its Before is the state before the first edit");
        }

        // Activation without an edit then release: End, but the store records nothing.
        {
            TestAdapter adapter;
            adapter.Live = Make(1, 1);
            Store store(adapter);
            Driver driver { adapter, store, {}, 2000, 0 };
            driver.Handle(driver.Tracker.OnItem(Frame(a, true, false, false, false)), false);
            const EditGestureResult end = driver.Tracker.OnItem(Frame(a, false, false, true, false));
            c.Expect(end.Action == EditGestureAction::End && !end.Edited, "click without a change ends unedited");
            driver.Handle(end, false);
            c.Expect(store.EntryCount() == 0 && !store.GestureOpen(), "no entry for a click without a change");
        }

        // Discrete widgets and same-frame press-release.
        {
            EditGestureTracker tracker;
            c.Expect(tracker.OnItem(Frame(a, false, true, false, false)).Action == EditGestureAction::Discrete, "edit without activation is discrete");
            c.Expect(tracker.OnItem(Frame(a, true, true, true, true)).Action == EditGestureAction::Discrete && !tracker.Open(), "activate+edit+release in one frame is discrete");
            c.Expect(tracker.OnItem(Frame(a, true, false, true, false)).Action == EditGestureAction::None && !tracker.Open(), "activate+release without change is nothing");
            c.Expect(tracker.OnItem(Frame(a, false, false, true, true)).Action == EditGestureAction::None, "stray release is ignored");
            c.Expect(tracker.OnItem(Frame(a, false, false, false, false)).Action == EditGestureAction::None, "idle frame");
        }

        // A second widget cannot interleave; only the opener ends the gesture.
        {
            EditGestureTracker tracker;
            tracker.OnItem(Frame(a, true, false, false, false));
            c.Expect(tracker.OnItem(Frame(b, true, true, false, false)).Action == EditGestureAction::Rejected && tracker.Open() && tracker.ActiveKey() == a, "second activation rejected");
            c.Expect(tracker.OnItem(Frame(b, false, true, false, false)).Action == EditGestureAction::Rejected, "second edit rejected");
            c.Expect(tracker.OnItem(Frame(b, false, false, true, true)).Action == EditGestureAction::None && tracker.Open(), "a foreign release does not end the gesture");
            const EditGestureResult end = tracker.OnItem(Frame(a, false, true, true, true));
            c.Expect(end.Action == EditGestureAction::End && end.Edited && !tracker.Open(), "opener ends");
            EditGestureKey sameItemOtherEntity = a;
            sameItemOtherEntity.Entity = 11;
            tracker.OnItem(Frame(a, true, false, false, false));
            c.Expect(tracker.OnItem(Frame(sameItemOtherEntity, true, true, false, false)).Action == EditGestureAction::Rejected, "same widget on another entity is another gesture");
        }

        // Frame-end flush and cancel.
        {
            EditGestureTracker tracker;
            c.Expect(tracker.OnFrameEnd(false).Action == EditGestureAction::None && tracker.Cancel().Action == EditGestureAction::None, "nothing open");
            tracker.OnItem(Frame(a, true, true, false, false));
            c.Expect(tracker.OnFrameEnd(true).Action == EditGestureAction::None && tracker.Open(), "an active item keeps the gesture");
            const EditGestureResult flush = tracker.OnFrameEnd(false);
            c.Expect(flush.Action == EditGestureAction::End && flush.Key == a && flush.Edited && !tracker.Open(), "an unsubmitted widget ends at frame end");
            tracker.OnItem(Frame(a, true, true, false, false));
            const EditGestureResult cancel = tracker.Cancel();
            c.Expect(cancel.Action == EditGestureAction::Cancel && cancel.Key == a && !tracker.Open(), "cancel");
            tracker.OnItem(Frame(a, true, false, false, false));
            tracker.Reset();
            c.Expect(!tracker.Open() && tracker.OnFrameEnd(false).Action == EditGestureAction::None, "reset drops the gesture");
        }

        // Cancel through the store restores Before and records nothing.
        {
            TestAdapter adapter;
            adapter.Live = Make(1, 1);
            const Ptr original = adapter.Live;
            Store store(adapter);
            Driver driver { adapter, store, {}, 2000, 0 };
            driver.Handle(driver.Tracker.OnItem(Frame(a, true, true, false, false)), true);
            adapter.Live = Make(77, 1);
            driver.Handle(driver.Tracker.Cancel(), false);
            c.Expect(adapter.Live == original && store.EntryCount() == 0 && !store.GestureOpen(), "escape restores exactly");
        }

        // Generated streams against the interval reference.
        const bool streamOk = RunProperty("HistoryEditGestureTracker", [](Spiral::Tests::ChoiceStream& stream, std::string& message)
        {
            const size_t count = stream.NextSize(1, 60);
            std::vector<EditItemFrame> frames;
            for (size_t i = 0; i < count; ++i)
            {
                EditItemFrame frame;
                frame.Key = { 1 + stream.NextSize(0, 2), 5, static_cast<u32>(stream.NextSize(0, 1)) };
                frame.Activated = stream.NextSize(0, 3) == 0;
                frame.Edited = stream.NextSize(0, 1) == 0;
                frame.Deactivated = stream.NextSize(0, 3) == 0;
                frame.DeactivatedAfterEdit = frame.Deactivated && frame.Edited && stream.NextSize(0, 1) == 0;
                frames.push_back(frame);
            }
            EditGestureTracker tracker;
            const std::vector<ReferenceStep> expected = ReferenceTracker(frames);
            for (size_t i = 0; i < frames.size(); ++i)
            {
                const EditGestureResult actual = tracker.OnItem(frames[i]);
                if (actual.Action != expected[i].Action
                    || (expected[i].Action != EditGestureAction::None && (actual.Key != expected[i].Key || actual.Edited != expected[i].Edited)))
                {
                    message = "frame " + std::to_string(i) + ": action " + std::to_string(static_cast<int>(actual.Action)) + " expected "
                        + std::to_string(static_cast<int>(expected[i].Action));
                    return false;
                }
            }
            return true;
        }, 512);
        return c.Ok && streamOk;
    }
}

namespace
{
    // The reference: a vector of full states and an integer cursor, with every
    // total recomputed from scratch after every step.
    struct Model
    {
        enum class BaseKind
        {
            Opened,
            Barrier,
            Dropped
        };

        struct Entry
        {
            u64 Revision = 0;
            std::string Display;
            Ptr Before;
            Ptr After;
        };

        struct BaseRow
        {
            u64 Revision = 0;
            std::string Display;
            BaseKind Kind = BaseKind::Opened;
            std::string Reason;
        };

        struct OpenGesture
        {
            EditGestureKey Key;
            std::string Display;
            Ptr Before;
            size_t Updates = 0;
        };

        struct Level
        {
            std::string Display;
            Ptr Before;
        };

        struct Saved
        {
            std::vector<Entry> Entries;
            size_t Cursor = 0;
            BaseRow Base;
            size_t EvictedEntries = 0;
            u64 EvictedBytes = 0;
            size_t EvictedEvents = 0;
        };

        struct Out
        {
            HistoryStatus Status = HistoryStatus::Ok;
            u64 Revision = 0;
            size_t Steps = 0;
            size_t RedoDiscarded = 0;
            size_t ForBudget = 0;
            size_t ForCap = 0;
            u64 EvictedBytes = 0;
            std::string Display;
            std::string Announcement;
        };

        u64 Budget = 0;
        size_t Cap = 0;
        std::vector<Entry> Entries;
        size_t Cursor = 0;
        BaseRow Base { 1, "Project opened", BaseKind::Opened, {} };
        u64 NextRevision = 2;
        size_t EvictedEntries = 0;
        u64 EvictedBytes = 0;
        size_t EvictedEvents = 0;
        std::optional<OpenGesture> Open;
        std::vector<Level> Levels;
        Ptr Live;
        bool FailNext = false;

        static Out Status(HistoryStatus status, std::string announcement = {})
        {
            Out out;
            out.Status = status;
            out.Announcement = std::move(announcement);
            return out;
        }

        u64 Head() const { return Cursor == 0 ? Base.Revision : Entries[Cursor - 1].Revision; }

        u64 Used() const
        {
            std::set<const TestState*> seen;
            u64 total = 0;
            for (const Entry& entry : Entries)
            {
                for (const Ptr& state : { entry.Before, entry.After })
                {
                    if (seen.insert(state.get()).second)
                        total += state->Cost;
                }
            }
            return total;
        }

        std::string EvictionText(size_t forBudget, size_t forCap) const
        {
            const size_t n = forBudget + forCap;
            std::string text = std::to_string(n) + (n == 1 ? " oldest entry was" : " oldest entries were") + " dropped to stay within ";
            if (forBudget != 0)
                text += std::to_string(Budget) + " B";
            if (forBudget != 0 && forCap != 0)
                text += " and ";
            if (forCap != 0)
                text += std::to_string(Cap) + (Cap == 1 ? " entry" : " entries");
            return text;
        }

        Out Commit(const std::string& display, const Ptr& before, const Ptr& after)
        {
            Out out;
            out.RedoDiscarded = Entries.size() - Cursor;
            Entries.resize(Cursor);
            Entries.push_back({ 0, display, before, after });
            Cursor = Entries.size();
            while (Entries.size() > 1)
            {
                const u64 used = Used();
                const bool overBudget = used > Budget;
                const bool overCap = Entries.size() > Cap;
                if (!overBudget && !overCap)
                    break;
                Entries.erase(Entries.begin());
                --Cursor;
                out.EvictedBytes += used - Used();
                (overBudget ? out.ForBudget : out.ForCap) += 1;
            }
            if (out.ForBudget + out.ForCap != 0)
            {
                EvictedEntries += out.ForBudget + out.ForCap;
                EvictedBytes += out.EvictedBytes;
                ++EvictedEvents;
                Base = { NextRevision++, "Earlier history dropped", BaseKind::Dropped, "earlier history was dropped to stay within the history limit" };
            }
            Entries.back().Revision = NextRevision++;
            out.Status = HistoryStatus::Recorded;
            out.Revision = Entries.back().Revision;
            out.Display = display;
            if (out.RedoDiscarded != 0)
                out.Announcement = "Redo history discarded (" + std::to_string(out.RedoDiscarded) + ")";
            if (out.ForBudget + out.ForCap != 0)
                out.Announcement += (out.Announcement.empty() ? "" : "; ") + EvictionText(out.ForBudget, out.ForCap);
            return out;
        }

        Out Record(const std::string& display, const Ptr& before, const Ptr& after)
        {
            if (Open) return Status(HistoryStatus::GestureOpen);
            if (!Levels.empty()) return Status(HistoryStatus::Absorbed);
            if (before == after) return Status(HistoryStatus::NoChange);
            return Commit(display, before, after);
        }

        Out BeginGesture(const EditGestureKey& key, const std::string& display, const Ptr& before)
        {
            if (Open) return Status(HistoryStatus::GestureOpen);
            if (!Levels.empty()) return Status(HistoryStatus::TransactionOpen);
            Open = OpenGesture { key, display, before, 0 };
            return Status(HistoryStatus::Ok);
        }

        Out UpdateGesture(const EditGestureKey& key)
        {
            if (!Open) return Status(HistoryStatus::NoGesture);
            if (!(Open->Key == key)) return Status(HistoryStatus::KeyMismatch);
            ++Open->Updates;
            return Status(HistoryStatus::Ok);
        }

        Out EndGesture(const EditGestureKey& key, const Ptr& after)
        {
            if (!Open) return Status(HistoryStatus::NoGesture);
            if (!(Open->Key == key)) return Status(HistoryStatus::KeyMismatch);
            OpenGesture gesture = *Open;
            Open.reset();
            if (gesture.Before == after || gesture.Before->Id == after->Id)
                return Status(HistoryStatus::NoChange);
            return Commit(gesture.Display, gesture.Before, after);
        }

        Out CancelGesture()
        {
            if (!Open) return Status(HistoryStatus::NoGesture);
            if (FailNext) return Status(HistoryStatus::RestoreFailed, "Cancel failed: could not restore the previous state");
            Live = Open->Before;
            Open.reset();
            return Status(HistoryStatus::Ok);
        }

        Out BeginTransaction(const std::string& display, const Ptr& before)
        {
            if (Open) return Status(HistoryStatus::GestureOpen);
            Levels.push_back({ display, before });
            return Status(HistoryStatus::Ok);
        }

        Out EndTransaction(const Ptr& after)
        {
            if (Levels.empty()) return Status(HistoryStatus::NoTransaction);
            if (Levels.size() > 1)
            {
                Levels.pop_back();
                return Status(HistoryStatus::Ok);
            }
            const Level level = Levels.back();
            Levels.pop_back();
            if (level.Before == after || level.Before->Id == after->Id)
                return Status(HistoryStatus::NoChange);
            return Commit(level.Display, level.Before, after);
        }

        Out AbortTransaction()
        {
            if (Levels.empty()) return Status(HistoryStatus::NoTransaction);
            if (FailNext) return Status(HistoryStatus::RestoreFailed, "Abort failed: could not restore the previous state");
            Live = Levels.back().Before;
            Levels.pop_back();
            return Status(HistoryStatus::Ok);
        }

        bool Refine(const std::string& display)
        {
            if (Open) Open->Display = display;
            else if (!Levels.empty()) Levels.front().Display = display;
            else return false;
            return true;
        }

        Out Undo()
        {
            if (Open || !Levels.empty()) return Status(Open ? HistoryStatus::GestureOpen : HistoryStatus::TransactionOpen, "Cannot undo: finish the current edit");
            if (Cursor == 0)
            {
                if (Base.Kind == BaseKind::Barrier)
                    return Status(HistoryStatus::UndoBarrier, "Cannot undo past: " + Base.Display + " - " + Base.Reason);
                return Status(HistoryStatus::NothingToUndo, Base.Kind == BaseKind::Dropped ? "Nothing to undo: " + Base.Reason : "Nothing to undo");
            }
            const Entry& entry = Entries[Cursor - 1];
            if (FailNext) return Status(HistoryStatus::RestoreFailed, "Undo failed: could not restore " + entry.Display);
            Live = entry.Before;
            --Cursor;
            Out out = Status(HistoryStatus::Ok, "Undo: " + entry.Display);
            out.Revision = entry.Revision;
            out.Steps = 1;
            out.Display = entry.Display;
            return out;
        }

        Out Redo()
        {
            if (Open || !Levels.empty()) return Status(Open ? HistoryStatus::GestureOpen : HistoryStatus::TransactionOpen, "Cannot redo: finish the current edit");
            if (Cursor == Entries.size())
                return Status(HistoryStatus::NothingToRedo, "Nothing to redo");
            const Entry& entry = Entries[Cursor];
            if (FailNext) return Status(HistoryStatus::RestoreFailed, "Redo failed: could not restore " + entry.Display);
            Live = entry.After;
            ++Cursor;
            Out out = Status(HistoryStatus::Ok, "Redo: " + entry.Display);
            out.Revision = entry.Revision;
            out.Steps = 1;
            out.Display = entry.Display;
            return out;
        }

        Out JumpTo(u64 revision)
        {
            if (Open || !Levels.empty()) return Status(Open ? HistoryStatus::GestureOpen : HistoryStatus::TransactionOpen, "Cannot jump: finish the current edit");
            std::vector<u64> positions { Base.Revision };
            for (const Entry& entry : Entries)
                positions.push_back(entry.Revision);
            const auto found = std::find(positions.begin(), positions.end(), revision);
            if (found == positions.end())
                return Status(HistoryStatus::UnknownRevision, "Cannot jump: that history row no longer exists");
            const size_t target = static_cast<size_t>(found - positions.begin());
            if (target == Cursor) return Status(HistoryStatus::NoChange);
            const std::string display = target == 0 ? Base.Display : Entries[target - 1].Display;
            if (FailNext) return Status(HistoryStatus::RestoreFailed, "Jump failed: could not restore " + display);
            const bool back = target < Cursor;
            const size_t steps = back ? Cursor - target : target - Cursor;
            Live = back ? Entries[target].Before : Entries[target - 1].After;
            Cursor = target;
            Out out = Status(HistoryStatus::Ok,
                "Jumped to: " + display + " (" + std::to_string(steps) + (steps == 1 ? " step " : " steps ") + (back ? "back)" : "forward)"));
            out.Revision = Head();
            out.Steps = steps;
            out.Display = display;
            return out;
        }

        Out JumpToRow(size_t row)
        {
            if (Open || !Levels.empty()) return Status(Open ? HistoryStatus::GestureOpen : HistoryStatus::TransactionOpen, "Cannot jump: finish the current edit");
            if (row > Entries.size()) return Status(HistoryStatus::UnknownRevision, "Cannot jump: that history row no longer exists");
            return JumpTo(row == 0 ? Base.Revision : Entries[row - 1].Revision);
        }

        Out Barrier(const std::string& display, const std::string& reason)
        {
            if (Open) return Status(HistoryStatus::GestureOpen);
            if (!Levels.empty()) return Status(HistoryStatus::TransactionOpen);
            Entries.clear();
            Cursor = 0;
            Base = { NextRevision++, display, BaseKind::Barrier, reason };
            return Status(HistoryStatus::Ok);
        }

        void Reset(const std::string& display)
        {
            Entries.clear();
            Cursor = 0;
            Open.reset();
            Levels.clear();
            EvictedEntries = 0;
            EvictedBytes = 0;
            EvictedEvents = 0;
            Base = { NextRevision++, display, BaseKind::Opened, {} };
        }

        bool CanMark() const { return !Open && Levels.empty(); }

        Saved Save() const { return { Entries, Cursor, Base, EvictedEntries, EvictedBytes, EvictedEvents }; }

        void Load(const Saved& saved)
        {
            Entries = saved.Entries;
            Cursor = saved.Cursor;
            Base = saved.Base;
            EvictedEntries = saved.EvictedEntries;
            EvictedBytes = saved.EvictedBytes;
            EvictedEvents = saved.EvictedEvents;
        }
    };

    struct Coverage
    {
        std::map<std::string, size_t> Hits;
        void Hit(const std::string& name) { ++Hits[name]; }
    };

    // Runs one generated sequence of operations against the store and the
    // reference model and compares every observable after every operation.
    bool RunModelSequence(Spiral::Tests::ChoiceStream& stream, std::string& message, Coverage& coverage, size_t operations)
    {
        const std::array<u64, 7> budgets { 0, 1, 50, 100, 300, 700, 1000 };
        const std::array<size_t, 6> caps { 1, 2, 3, 5, 8, 512 };
        TestAdapter adapter;
        Model model;
        model.Budget = budgets[stream.NextSize(0, budgets.size() - 1)];
        model.Cap = caps[stream.NextSize(0, caps.size() - 1)];
        u64 nextId = 2;
        const auto newState = [&]() { return Make(nextId++, stream.NextSize(0, 60)); };
        adapter.Live = model.Live = Make(1, stream.NextSize(0, 60));
        Store store(adapter, HistoryConfig { model.Budget, model.Cap });
        std::optional<std::pair<Store::Mark, Model::Saved>> saved;
        bool failPending = false;

        const std::array<EditGestureKey, 6> keys { { { 1, 100, 1 }, { 2, 100, 1 }, { 3, 100, 1 }, { 1, 101, 1 }, { 1, 100, 2 }, { 2, 101, 2 } } };
        const auto pickKey = [&]()
        {
            if (model.Open && stream.NextSize(0, 9) < 8)
                return model.Open->Key;
            return keys[stream.NextSize(0, keys.size() - 1)];
        };
        // An After snapshot: usually the live state, sometimes a distinct
        // object that is equal to the given Before (Equal-path coverage).
        const auto pickAfter = [&](const Ptr& before)
        {
            if (before && stream.NextSize(0, 3) == 0)
                adapter.Live = model.Live = Make(before->Id, stream.NextSize(0, 60));
            return adapter.Live;
        };

        for (size_t op = 0; op < operations; ++op)
        {
            const std::string opName = "op " + std::to_string(op);
            const std::string display = "T" + std::to_string(op);
            const bool failing = failPending;
            adapter.FailRestore = failing;
            model.FailNext = failing;
            failPending = false;

            size_t pick = stream.NextSize(0, 99);
            // Steer toward open edits so update/end/cancel/abort and nesting are
            // exercised; the model still decides every outcome.
            if (failing && model.Open && stream.NextSize(0, 2) == 0)
                pick = 74;
            else if (failing && !model.Levels.empty() && stream.NextSize(0, 2) == 0)
                pick = 87;
            else if (pick >= 63 && pick < 77 && !model.Open && stream.NextSize(0, 4) < 2)
                pick = 59;
            else if (pick >= 81 && pick < 90 && model.Levels.empty() && stream.NextSize(0, 4) < 2)
                pick = 77;
            else if (pick >= 81 && pick < 87 && model.Levels.size() == 1 && stream.NextSize(0, 3) == 0)
                pick = 77;
            std::string name;
            Model::Out expected;
            HistoryResult actual;
            bool compareResult = true;

            if (pick < 22)
            {
                name = "record";
                const Ptr before = adapter.Live;
                const Ptr after = stream.NextSize(0, 3) == 0 ? before : newState();
                adapter.Live = model.Live = after;
                actual = store.Record(MakeHistoryLabel("Edit", display), before, after);
                expected = model.Record("Edit " + display, before, after);
            }
            else if (pick < 34) { name = "undo"; actual = store.Undo(); expected = model.Undo(); }
            else if (pick < 44) { name = "redo"; actual = store.Redo(); expected = model.Redo(); }
            else if (pick < 50)
            {
                name = "jump";
                if (stream.NextSize(0, 2) == 0)
                {
                    const size_t row = stream.NextSize(0, model.Entries.size() + 2);
                    actual = store.JumpToRow(row);
                    expected = model.JumpToRow(row);
                }
                else
                {
                    u64 revision = 0;
                    const size_t kind = stream.NextSize(0, 5);
                    if (kind == 0) revision = 0;
                    else if (kind == 1) revision = model.NextRevision + 3;
                    else if (kind == 2) revision = model.Head();
                    else if (kind == 3) revision = model.Base.Revision;
                    else revision = model.Entries.empty() ? model.Base.Revision : model.Entries[stream.NextSize(0, model.Entries.size() - 1)].Revision;
                    actual = store.JumpTo(revision);
                    expected = model.JumpTo(revision);
                }
            }
            else if (pick < 52)
            {
                name = "barrier";
                actual = store.Barrier(MakeHistoryLabel("Barrier", display, HistorySource::System), "reason " + display);
                expected = model.Barrier("Barrier " + display, "reason " + display);
            }
            else if (pick < 53)
            {
                name = "reset";
                actual = store.Reset(MakeHistoryLabel("Opened", display, HistorySource::System));
                model.Reset("Opened " + display);
                expected = Model::Status(HistoryStatus::Ok);
            }
            else if (pick < 56)
            {
                name = "save-mark";
                Store::Mark mark = store.SaveMark();
                if (mark.Valid != model.CanMark())
                {
                    message = opName + " save-mark validity differs from the model";
                    return false;
                }
                if (mark.Valid)
                    saved = std::make_pair(std::move(mark), model.Save());
                compareResult = false;
            }
            else if (pick < 59)
            {
                name = "load-mark";
                if (saved)
                {
                    const bool loaded = store.LoadMark(saved->first);
                    if (loaded != model.CanMark())
                    {
                        message = opName + " load-mark result differs from the model";
                        return false;
                    }
                    if (loaded)
                    {
                        model.Load(saved->second);
                        coverage.Hit("mark loaded");
                    }
                }
                compareResult = false;
            }
            else if (pick < 63)
            {
                name = "gesture-begin";
                const EditGestureKey key = keys[stream.NextSize(0, keys.size() - 1)];
                actual = store.BeginGesture(key, MakeHistoryLabel("Gesture", display), adapter.Live);
                expected = model.BeginGesture(key, "Gesture " + display, model.Live);
            }
            else if (pick < 67)
            {
                name = "gesture-update";
                const EditGestureKey key = pickKey();
                adapter.Live = model.Live = newState();
                actual = store.UpdateGesture(key);
                expected = model.UpdateGesture(key);
            }
            else if (pick < 74)
            {
                name = "gesture-end";
                const EditGestureKey key = pickKey();
                const Ptr after = pickAfter(model.Open ? model.Open->Before : nullptr);
                actual = store.EndGesture(key, after);
                expected = model.EndGesture(key, after);
            }
            else if (pick < 77) { name = "gesture-cancel"; actual = store.CancelGesture(); expected = model.CancelGesture(); }
            else if (pick < 81)
            {
                name = "txn-begin";
                actual = store.BeginTransaction(MakeHistoryLabel("Txn", display), adapter.Live);
                expected = model.BeginTransaction("Txn " + display, model.Live);
            }
            else if (pick < 87)
            {
                name = "txn-end";
                const Ptr after = pickAfter(model.Levels.empty() ? nullptr : model.Levels.front().Before);
                if (model.Levels.size() > 1) coverage.Hit("nested end");
                actual = store.EndTransaction(after);
                expected = model.EndTransaction(after);
            }
            else if (pick < 90) { name = "txn-abort"; actual = store.AbortTransaction(); expected = model.AbortTransaction(); }
            else if (pick < 92)
            {
                name = "refine";
                const bool refined = store.RefineOpenLabel(MakeHistoryLabel("Refined", display));
                if (refined != model.Refine("Refined " + display))
                {
                    message = opName + " refine result differs from the model";
                    return false;
                }
                compareResult = false;
            }
            else if (pick < 96) { name = "arm-failure"; failPending = true; compareResult = false; }
            else { name = "drift"; adapter.Live = model.Live = newState(); compareResult = false; }

            const std::string where = opName + " (" + name + ")";
            if (compareResult)
            {
                if (actual.Status != expected.Status || actual.Revision != expected.Revision || actual.Steps != expected.Steps
                    || actual.RedoDiscarded != expected.RedoDiscarded || actual.Evicted.ForBudget != expected.ForBudget
                    || actual.Evicted.ForEntryCap != expected.ForCap || actual.Evicted.Bytes != expected.EvictedBytes
                    || actual.Label.Display() != expected.Display || actual.Announcement != expected.Announcement || actual.Head != model.Head())
                {
                    message = where + ": result differs: status " + std::to_string(static_cast<int>(actual.Status)) + "/"
                        + std::to_string(static_cast<int>(expected.Status)) + " revision " + std::to_string(actual.Revision) + "/"
                        + std::to_string(expected.Revision) + " steps " + std::to_string(actual.Steps) + "/" + std::to_string(expected.Steps)
                        + " discarded " + std::to_string(actual.RedoDiscarded) + "/" + std::to_string(expected.RedoDiscarded) + " label '"
                        + actual.Label.Display() + "'/'" + expected.Display + "' announcement '" + actual.Announcement + "'/'"
                        + expected.Announcement + "'";
                    return false;
                }
                coverage.Hit(name + " " + std::to_string(static_cast<int>(actual.Status)));
                if (actual.RedoDiscarded != 0) coverage.Hit("redo discarded");
                if (actual.Evicted.ForBudget != 0) coverage.Hit("evicted for budget");
                if (actual.Evicted.ForEntryCap != 0) coverage.Hit("evicted for cap");
            }

            // Observables.
            if (adapter.Live != model.Live)
            {
                message = where + ": live state differs from the model";
                return false;
            }
            const std::vector<HistoryRow> rows = store.Rows();
            if (rows.size() != model.Entries.size() + 1)
            {
                message = where + ": row count";
                return false;
            }
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const bool base = i == 0;
                const std::string wantDisplay = base ? model.Base.Display + (model.Base.Kind == Model::BaseKind::Barrier ? " (barrier)" : "") : model.Entries[i - 1].Display;
                const u64 wantRevision = base ? model.Base.Revision : model.Entries[i - 1].Revision;
                const u64 wantBytes = base ? 0 : model.Entries[i - 1].Before->Cost + model.Entries[i - 1].After->Cost;
                if (rows[i].Display != wantDisplay || rows[i].Revision != wantRevision || rows[i].Bytes != wantBytes || rows[i].IsBase != base
                    || rows[i].IsBarrier != (base && model.Base.Kind == Model::BaseKind::Barrier) || rows[i].Applied != (i <= model.Cursor)
                    || rows[i].Current != (i == model.Cursor))
                {
                    message = where + ": row " + std::to_string(i) + " differs: '" + rows[i].Display + "' rev " + std::to_string(rows[i].Revision);
                    return false;
                }
            }
            if (store.HeadRevision() != model.Head() || store.Cursor() != model.Cursor || store.UndoDepth() != model.Cursor
                || store.RedoDepth() != model.Entries.size() - model.Cursor || store.EntryCount() != model.Entries.size())
            {
                message = where + ": cursor/head/depth differ";
                return false;
            }
            if (store.UsedBytes() != model.Used())
            {
                message = where + ": used bytes " + std::to_string(store.UsedBytes()) + " expected " + std::to_string(model.Used());
                return false;
            }
            const EvictionStats& stats = store.Evicted();
            if (stats.Entries != model.EvictedEntries || stats.Bytes != model.EvictedBytes || stats.Events != model.EvictedEvents)
            {
                message = where + ": eviction stats differ";
                return false;
            }
            if (store.GestureOpen() != model.Open.has_value() || store.TransactionDepth() != model.Levels.size()
                || store.GestureUpdates() != (model.Open ? model.Open->Updates : 0) || store.BaseIsBarrier() != (model.Base.Kind == Model::BaseKind::Barrier))
            {
                message = where + ": gesture/transaction state differs";
                return false;
            }
            if ((store.TopUndo() != nullptr) != (model.Cursor != 0) || (store.TopRedo() != nullptr) != (model.Cursor != model.Entries.size()))
            {
                message = where + ": top entries presence differs";
                return false;
            }
            if (store.TopUndo() && (store.TopUndo()->Label.Display() != model.Entries[model.Cursor - 1].Display
                    || store.TopUndo()->Revision != model.Entries[model.Cursor - 1].Revision))
            {
                message = where + ": top undo differs";
                return false;
            }
            if (store.TopRedo() && store.TopRedo()->Label.Display() != model.Entries[model.Cursor].Display)
            {
                message = where + ": top redo differs";
                return false;
            }
            const bool busy = model.Open || !model.Levels.empty();
            const HistoryAvailability undo = store.UndoAvailability();
            const bool undoEnabled = !busy && model.Cursor != 0;
            std::string undoLabel = "Undo (finish the current edit)";
            if (!busy)
            {
                if (model.Cursor != 0) undoLabel = "Undo " + model.Entries[model.Cursor - 1].Display;
                else if (model.Base.Kind == Model::BaseKind::Barrier) undoLabel = "Undo (blocked: " + model.Base.Display + ")";
                else undoLabel = "Undo (nothing to undo)";
            }
            const HistoryAvailability redo = store.RedoAvailability();
            const bool redoEnabled = !busy && model.Cursor != model.Entries.size();
            std::string redoLabel = "Redo (finish the current edit)";
            if (!busy)
                redoLabel = redoEnabled ? "Redo " + model.Entries[model.Cursor].Display : "Redo (nothing to redo)";
            if (undo.Enabled != undoEnabled || undo.MenuLabel != undoLabel || redo.Enabled != redoEnabled || redo.MenuLabel != redoLabel
                || undo.Enabled != undo.Detail.empty() || redo.Enabled != redo.Detail.empty())
            {
                message = where + ": availability differs: '" + undo.MenuLabel + "' / '" + redo.MenuLabel + "'";
                return false;
            }

            // Contract invariants independent of the model.
            if (store.UsedBytes() > model.Budget && store.EntryCount() > 1)
            {
                message = where + ": budget exceeded with more than one entry";
                return false;
            }
            if (store.EntryCount() > std::max<size_t>(model.Cap, 1))
            {
                message = where + ": entry cap exceeded";
                return false;
            }
            for (size_t i = 2; i < rows.size(); ++i)
            {
                if (rows[i].Revision <= rows[i - 1].Revision)
                {
                    message = where + ": entry revisions are not strictly increasing";
                    return false;
                }
            }
        }
        return true;
    }
}

namespace SpiralTests
{
    bool TestHistoryStoreMatchesReferenceModel()
    {
        Coverage coverage;
        const bool ok = RunProperty("HistoryStoreModel", [&](Spiral::Tests::ChoiceStream& stream, std::string& message)
        {
            return RunModelSequence(stream, message, coverage, 600);
        }, 48);
        if (!ok)
            return false;
        if (std::getenv("SPIRAL_HISTORY_REPLAY"))
            return true;

        Checker c { "store-model-coverage" };
        if (std::getenv("SPIRAL_HISTORY_COVERAGE")) for (const auto& [k, v] : coverage.Hits) std::cerr << k << "=" << v << "\n";
        const std::vector<std::string> required {
            "record 1", "record 2", "record 3", "record 7", "undo 0", "undo 4", "undo 6", "undo 7", "undo 8", "undo 13", "redo 0", "redo 5",
            "jump 0", "jump 2", "jump 12", "barrier 0", "gesture-begin 0", "gesture-begin 7", "gesture-update 0", "gesture-end 1", "gesture-end 2",
            "gesture-end 11", "gesture-cancel 0", "gesture-cancel 13", "txn-begin 0", "txn-end 0", "txn-end 2", "txn-abort 0", "txn-abort 13",
            "nested end", "redo discarded", "evicted for budget", "evicted for cap", "mark loaded"
        };
        for (const std::string& key : required)
            c.Expect(coverage.Hits[key] > 0, "generator never reached: " + key);
        return c.Ok;
    }

    bool TestHistoryShortcutDecisionTable()
    {
        Checker c { "shortcut" };
        size_t allowed = 0;
        size_t evaluated = 0;
        const std::array<ShortcutKey, 4> keys { ShortcutKey::None, ShortcutKey::Z, ShortcutKey::Y, ShortcutKey::Other };
        for (const ShortcutKey key : keys)
        {
            for (unsigned chordBits = 0; chordBits < 16; ++chordBits)
            {
                for (unsigned contextBits = 0; contextBits < 32; ++contextBits)
                {
                    ShortcutChord chord;
                    chord.Key = key;
                    chord.Ctrl = (chordBits & 1u) != 0;
                    chord.Shift = (chordBits & 2u) != 0;
                    chord.Alt = (chordBits & 4u) != 0;
                    chord.Repeat = (chordBits & 8u) != 0;
                    ShortcutContext context;
                    context.WindowFocused = (contextBits & 1u) != 0;
                    context.BrowserOwnsKeyboard = (contextBits & 2u) != 0;
                    context.TextInputActive = (contextBits & 4u) != 0;
                    context.ModalOpen = (contextBits & 8u) != 0;
                    context.DragActive = (contextBits & 16u) != 0;

                    // Hand-written table: which chords mean what.
                    ShortcutAction wantAction = ShortcutAction::None;
                    if (key == ShortcutKey::Z && chord.Ctrl && !chord.Shift && !chord.Alt) wantAction = ShortcutAction::Undo;
                    if (key == ShortcutKey::Z && chord.Ctrl && chord.Shift && !chord.Alt) wantAction = ShortcutAction::Redo;
                    if (key == ShortcutKey::Y && chord.Ctrl && !chord.Shift && !chord.Alt) wantAction = ShortcutAction::Redo;

                    ShortcutReason wantReason = ShortcutReason::NotAHistoryChord;
                    if (wantAction != ShortcutAction::None)
                    {
                        const std::vector<std::pair<bool, ShortcutReason>> blockers {
                            { !context.WindowFocused, ShortcutReason::WindowNotFocused },
                            { context.BrowserOwnsKeyboard, ShortcutReason::BrowserOwnsKeyboard },
                            { context.TextInputActive, ShortcutReason::TextInputActive },
                            { context.ModalOpen, ShortcutReason::ModalOpen },
                            { context.DragActive, ShortcutReason::DragActive },
                            { chord.Repeat, ShortcutReason::AutoRepeat },
                        };
                        wantReason = ShortcutReason::Allowed;
                        for (const auto& [blocked, reason] : blockers)
                        {
                            if (blocked)
                            {
                                wantReason = reason;
                                break;
                            }
                        }
                        if (wantReason != ShortcutReason::Allowed)
                            wantAction = ShortcutAction::None;
                    }

                    const ShortcutDecision decision = ResolveShortcut(chord, context);
                    ++evaluated;
                    if (decision.Action != wantAction || decision.Reason != wantReason)
                    {
                        c.Expect(false, "key " + std::to_string(static_cast<int>(key)) + " chord bits " + std::to_string(chordBits) + " context bits "
                                + std::to_string(contextBits) + " gave action " + std::to_string(static_cast<int>(decision.Action)) + " reason "
                                + std::to_string(static_cast<int>(decision.Reason)));
                        return false;
                    }
                    if (decision.Reason == ShortcutReason::Allowed)
                        ++allowed;
                    const bool blocked = decision.Reason != ShortcutReason::Allowed && decision.Reason != ShortcutReason::NotAHistoryChord;
                    c.Expect(blocked == (std::string(DescribeShortcutBlock(decision.Reason)) != ""), "block description present exactly for blocked chords");
                }
            }
        }
        c.Expect(evaluated == 4 * 16 * 32, "whole input space evaluated");
        // Three chords (Ctrl+Z, Ctrl+Shift+Z, Ctrl+Y) without repeat in the one fully permissive context.
        c.Expect(allowed == 3, "exactly three chord/context combinations fire: " + std::to_string(allowed));

        // Named scenarios from the contract.
        ShortcutContext open;
        const ShortcutChord undo { ShortcutKey::Z, true, false, false, false };
        c.Expect(ResolveShortcut(undo, open).Action == ShortcutAction::Undo, "Ctrl+Z undoes");
        ShortcutContext typing = open;
        typing.TextInputActive = true;
        c.Expect(ResolveShortcut(undo, typing).Reason == ShortcutReason::TextInputActive, "typing blocks undo");
        ShortcutContext browser = open;
        browser.BrowserOwnsKeyboard = true;
        c.Expect(ResolveShortcut(undo, browser).Reason == ShortcutReason::BrowserOwnsKeyboard, "the web page owning the keyboard blocks undo");
        ShortcutContext both = browser;
        both.TextInputActive = true;
        both.ModalOpen = true;
        c.Expect(ResolveShortcut(undo, both).Reason == ShortcutReason::BrowserOwnsKeyboard, "the browser outranks text input in the reported reason");
        return c.Ok;
    }
}
