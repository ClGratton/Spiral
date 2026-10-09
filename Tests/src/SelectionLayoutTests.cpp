#include "SelectionLayoutTests.h"

#include "EntityClipboard.h"
#include "EntityNaming.h"
#include "HierarchyModel.h"
#include "LayoutFile.h"
#include "LayoutMigration.h"
#include "SelectionModel.h"
#include "TestSupport/GeneratedTest.h"

#include "Engine/Core/Sha256.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#if defined(__linux__)
    #include <csignal>
    #include <sys/resource.h>
    #include <unistd.h>
#endif

namespace
{
    using namespace SpiralEditor;
    using Engine::u32;
    using Engine::u64;
    using Engine::u8;
    using Spiral::Tests::ChoiceStream;

    // Failure hypotheses, oracles, and non-claims for the whole file:
    // - Selection is checked against a stamp-based reference (each selected id carries
    //   the sequence number of when it was last added; order and "most recent member"
    //   are derived from the stamps), a different data structure from the production
    //   vector plus hash set, over generated operation sequences with duplicate,
    //   missing, and zero ids in the supplied lists.
    // - Naming is checked against a std::regex stem splitter plus a naive "try N = 2, 3,
    //   ..." scan over std::set, and against an RFC 3629 byte-range table for UTF-8.
    // - Filtering, reordering and clipboard use naive per-row / sort-key / hand-built
    //   byte models. Layout grammar is checked against hand-written golden bytes whose
    //   SHA-256 was produced by an independent tool (Python hashlib), a std::regex name
    //   oracle, and the property "an accepted mutation re-serializes to itself".
    // - Properties are deterministic and replayable through SPIRAL_SELECTION_LAYOUT_SEED
    //   and SPIRAL_SELECTION_LAYOUT_REPLAY; a failure prints the seed, original and
    //   minimised choice traces and writes a counterexample JSON under the system
    //   temp directory.
    // - Tier: Fast for in-memory models; Integration for the filesystem test, which
    //   uses unique temp fixtures and never touches output/editor.
    // - Not claimed: any ImGui or GLFW behaviour, real OS clipboard transport,
    //   Windows or macOS execution, durability across power loss, or the behaviour of
    //   the Editor panels that will consume these models.

    struct Checker
    {
        const char* Suite;
        bool Ok = true;

        void Expect(bool condition, const std::string& message)
        {
            if (!condition)
            {
                std::cerr << "Selection/layout test failed [" << Suite << "]: " << message << '\n';
                Ok = false;
            }
        }
    };

    std::atomic<u64> g_FixtureCounter { 0 };

    class TempDir
    {
    public:
        explicit TempDir(std::string_view name)
        {
            const u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
            m_Path = std::filesystem::temp_directory_path()
                / ("spiral-selection-layout-" + std::string(name) + "-" + std::to_string(stamp) + "-"
                    + std::to_string(g_FixtureCounter.fetch_add(1)));
            std::error_code error;
            std::filesystem::create_directories(m_Path, error);
        }

        ~TempDir()
        {
            std::error_code error;
            for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(
                     m_Path, std::filesystem::directory_options::skip_permission_denied, error))
            {
                if (entry.is_directory(error))
                    std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_all,
                        std::filesystem::perm_options::add, error);
            }
            std::filesystem::remove_all(m_Path, error);
        }

        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;

        const std::filesystem::path& Path() const { return m_Path; }

    private:
        std::filesystem::path m_Path;
    };

    bool RunProperty(std::string_view name, const Spiral::Tests::Property& property, size_t iterations)
    {
        Spiral::Tests::CampaignOptions options;
        options.Iterations = iterations;
        if (const char* seed = std::getenv("SPIRAL_SELECTION_LAYOUT_SEED"))
            options.Seed = std::strtoull(seed, nullptr, 10);
        Spiral::Tests::ChoiceTrace replay;
        if (const char* trace = std::getenv("SPIRAL_SELECTION_LAYOUT_REPLAY");
            trace && Spiral::Tests::ParseTrace(trace, replay))
        {
            options.Replay = replay;
        }

        Spiral::Tests::Counterexample failure;
        if (Spiral::Tests::RunCampaign(options, property, failure))
            return true;

        const std::string minimized = Spiral::Tests::SerializeTrace(failure.MinimizedTrace);
        const std::string rerun = "SPIRAL_SELECTION_LAYOUT_SEED=" + std::to_string(failure.Seed)
            + " SPIRAL_SELECTION_LAYOUT_REPLAY=\"" + minimized + "\" EngineTests --test <registered name of "
            + std::string(name) + ">";
        const std::filesystem::path artifact = std::filesystem::temp_directory_path()
            / ("spiral-selection-layout-counterexample-" + std::string(name) + ".json");
        std::string artifactError;
        const bool written = Spiral::Tests::WriteCounterexample(artifact, name, failure, rerun, artifactError);
        std::cerr << "Selection/layout property failed [" << name << "]: " << failure.Message
            << " seed=" << failure.Seed << " iteration=" << failure.Iteration
            << " originalTrace=" << Spiral::Tests::SerializeTrace(failure.OriginalTrace)
            << " minimizedTrace=" << minimized << "\n  rerun: " << rerun << '\n';
        if (written)
            std::cerr << "  counterexample: " << artifact.string() << '\n';
        else
            std::cerr << "  counterexample write failed: " << artifactError << '\n';
        return false;
    }

    template <typename T>
    const T& Pick(ChoiceStream& stream, const std::vector<T>& values)
    {
        return values[stream.NextSize(0, values.size() - 1)];
    }

    std::string Describe(const std::vector<EntityId>& ids)
    {
        std::string text = "[";
        for (size_t index = 0; index < ids.size(); ++index)
            text += (index ? "," : "") + std::to_string(ids[index]);
        return text + "]";
    }

    std::string ReadAll(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    }

    std::vector<std::string> ListNames(const std::filesystem::path& directory)
    {
        std::vector<std::string> names;
        std::error_code error;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, error))
            names.push_back(entry.path().filename().string());
        std::sort(names.begin(), names.end());
        return names;
    }

    // ===== selection: reference model ==============================================

    struct ReferenceSelection
    {
        std::map<EntityId, u64> Stamp;
        u64 Next = 1;
        EntityId Primary = 0;
        EntityId Anchor = 0;

        std::vector<EntityId> Ordered() const
        {
            std::vector<std::pair<u64, EntityId>> pairs;
            for (const auto& [id, stamp] : Stamp)
                pairs.emplace_back(stamp, id);
            std::sort(pairs.begin(), pairs.end());
            std::vector<EntityId> ids;
            for (const auto& pair : pairs)
                ids.push_back(pair.second);
            return ids;
        }

        EntityId Newest() const
        {
            EntityId best = 0;
            u64 bestStamp = 0;
            for (const auto& [id, stamp] : Stamp)
            {
                if (stamp > bestStamp)
                {
                    best = id;
                    bestStamp = stamp;
                }
            }
            return best;
        }

        void Repair()
        {
            if (Stamp.empty())
                Primary = 0;
            else if (!Stamp.count(Primary))
                Primary = Newest();
        }

        void Add(EntityId id)
        {
            if (id != 0 && !Stamp.count(id))
                Stamp[id] = Next++;
        }

        void Click(EntityId id)
        {
            if (id == 0)
                return;
            Stamp.clear();
            Add(id);
            Primary = Anchor = id;
        }

        void Toggle(EntityId id)
        {
            if (id == 0)
                return;
            if (Stamp.count(id))
            {
                Stamp.erase(id);
                Repair();
            }
            else
            {
                Add(id);
                Primary = id;
            }
            Anchor = id;
        }

        static int FirstIndex(const std::vector<EntityId>& order, EntityId id)
        {
            for (size_t index = 0; index < order.size(); ++index)
            {
                if (order[index] == id)
                    return static_cast<int>(index);
            }
            return -1;
        }

        void Range(const std::vector<EntityId>& order, EntityId id, bool additive)
        {
            const int target = FirstIndex(order, id);
            if (id == 0 || target < 0)
                return;
            const int anchor = Anchor == 0 ? -1 : FirstIndex(order, Anchor);
            if (anchor < 0)
            {
                Click(id);
                return;
            }
            if (!additive)
                Stamp.clear();
            for (int index = std::min(anchor, target); index <= std::max(anchor, target); ++index)
                Add(order[static_cast<size_t>(index)]);
            Primary = id;
        }

        void All(const std::vector<EntityId>& order)
        {
            for (const EntityId id : order)
                Add(id);
            Repair();
        }

        void Invert(const std::vector<EntityId>& order)
        {
            const std::set<EntityId> before = [&]
            {
                std::set<EntityId> members;
                for (const auto& entry : Stamp)
                    members.insert(entry.first);
                return members;
            }();
            std::set<EntityId> handled;
            for (const EntityId id : order)
            {
                if (id == 0 || !handled.insert(id).second)
                    continue;
                if (before.count(id))
                    Stamp.erase(id);
                else
                    Add(id);
            }
            Repair();
        }

        void Clear()
        {
            Stamp.clear();
            Primary = Anchor = 0;
        }

        void Prune(const std::vector<EntityId>& existing)
        {
            const std::set<EntityId> alive(existing.begin(), existing.end());
            for (auto it = Stamp.begin(); it != Stamp.end();)
                it = alive.count(it->first) ? std::next(it) : Stamp.erase(it);
            Repair();
            Anchor = (Anchor == 0 || alive.count(Anchor)) ? Anchor : Primary;
        }

        void Restore(const SelectionState& state, const std::vector<EntityId>& existing)
        {
            const std::set<EntityId> alive(existing.begin(), existing.end());
            Stamp.clear();
            for (const EntityId id : state.Ids)
            {
                if (alive.count(id))
                    Add(id);
            }
            Primary = Stamp.count(state.Primary) ? state.Primary : Newest();
            Repair();
            Anchor = (state.Anchor == 0 || alive.count(state.Anchor)) ? state.Anchor : Primary;
        }

        std::set<EntityId> Members() const
        {
            std::set<EntityId> members;
            for (const auto& entry : Stamp)
                members.insert(entry.first);
            return members;
        }
    };

    std::vector<EntityId> GenerateUniverse(ChoiceStream& stream)
    {
        const std::vector<EntityId> candidates = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 255, 256, 65535, 65536,
            0x7FFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFFFu };
        std::vector<EntityId> pool = candidates;
        std::vector<EntityId> universe;
        const size_t count = stream.NextSize(0, 12);
        for (size_t index = 0; index < count && !pool.empty(); ++index)
        {
            const size_t pick = stream.NextSize(0, pool.size() - 1);
            universe.push_back(pool[pick]);
            pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(pick));
        }
        return universe;
    }

    // A caller-supplied row order: usually a shuffled subset, sometimes with duplicates or zero ids.
    std::vector<EntityId> GenerateOrder(ChoiceStream& stream, const std::vector<EntityId>& universe)
    {
        std::vector<EntityId> order;
        for (const EntityId id : universe)
        {
            if (stream.NextSize(0, 5) != 0)
                order.push_back(id);
        }
        for (size_t index = order.size(); index > 1; --index)
            std::swap(order[index - 1], order[stream.NextSize(0, index - 1)]);
        if (stream.NextSize(0, 7) == 0 && !order.empty())
            order.insert(order.begin() + static_cast<std::ptrdiff_t>(stream.NextSize(0, order.size())), order[stream.NextSize(0, order.size() - 1)]);
        if (stream.NextSize(0, 11) == 0)
            order.insert(order.begin() + static_cast<std::ptrdiff_t>(stream.NextSize(0, order.size())), 0);
        return order;
    }

    EntityId PickId(ChoiceStream& stream, const std::vector<EntityId>& universe, const std::vector<EntityId>& order)
    {
        const size_t selector = stream.NextSize(0, 9);
        if (selector == 0)
            return 0;
        if (selector == 1)
            return 0xFFFFFF00u + static_cast<EntityId>(stream.NextSize(0, 3)); // usually absent from every list
        if (selector <= 6 && !order.empty())
            return order[stream.NextSize(0, order.size() - 1)];
        if (!universe.empty())
            return universe[stream.NextSize(0, universe.size() - 1)];
        return 1;
    }

    bool SelectionPropertyOnce(ChoiceStream& stream, std::string& message)
    {
        const std::vector<EntityId> universe = GenerateUniverse(stream);
        SelectionModel model;
        ReferenceSelection reference;
        const size_t operations = stream.NextSize(1, 60);
        std::vector<SelectionState> captured;
        for (size_t step = 0; step < operations; ++step)
        {
            const std::vector<EntityId> order = GenerateOrder(stream, universe);
            const EntityId id = PickId(stream, universe, order);
            const size_t operation = stream.NextSize(0, 10);
            const std::set<EntityId> membersBefore = reference.Members();
            const EntityId primaryBefore = reference.Primary;
            const u64 revisionBefore = model.Revision();
            std::string name;
            bool changed = false;
            switch (operation)
            {
                case 0: name = "Click"; changed = model.Click(id); reference.Click(id); break;
                case 1: name = "Toggle"; changed = model.Toggle(id); reference.Toggle(id); break;
                case 2: name = "SelectRange"; changed = model.SelectRange(order, id, false); reference.Range(order, id, false); break;
                case 3: name = "SelectRange+"; changed = model.SelectRange(order, id, true); reference.Range(order, id, true); break;
                case 4: name = "SelectAll"; changed = model.SelectAll(order); reference.All(order); break;
                case 5: name = "Invert"; changed = model.Invert(order); reference.Invert(order); break;
                case 6: name = "Clear"; changed = model.Clear(); reference.Clear(); break;
                case 7:
                {
                    name = "Prune";
                    std::vector<EntityId> existing;
                    for (const EntityId candidate : universe)
                    {
                        if (stream.NextSize(0, 3) != 0)
                            existing.push_back(candidate);
                    }
                    changed = model.Prune(existing);
                    reference.Prune(existing);
                    break;
                }
                case 8:
                {
                    name = "Restore";
                    SelectionState state;
                    if (!captured.empty() && stream.NextBool())
                        state = captured[stream.NextSize(0, captured.size() - 1)];
                    else
                    {
                        for (const EntityId candidate : order)
                            state.Ids.push_back(candidate);
                        state.Primary = id;
                        state.Anchor = PickId(stream, universe, order);
                    }
                    std::vector<EntityId> existing;
                    for (const EntityId candidate : universe)
                    {
                        if (stream.NextSize(0, 4) != 0)
                            existing.push_back(candidate);
                    }
                    changed = model.Restore(state, existing);
                    reference.Restore(state, existing);
                    break;
                }
                case 9:
                {
                    name = "Capture";
                    captured.push_back(model.Capture());
                    continue;
                }
                default:
                {
                    name = "InOrder";
                    const std::vector<EntityId> got = model.InOrder(order);
                    std::vector<EntityId> expected;
                    std::set<EntityId> emitted;
                    for (const EntityId candidate : order)
                    {
                        if (reference.Stamp.count(candidate) && emitted.insert(candidate).second)
                            expected.push_back(candidate);
                    }
                    if (got != expected)
                    {
                        message = "step " + std::to_string(step) + " InOrder " + Describe(got) + " expected " + Describe(expected);
                        return false;
                    }
                    continue;
                }
            }

            const std::string where = "step " + std::to_string(step) + " " + name + "(id=" + std::to_string(id)
                + ", order=" + Describe(order) + "): ";
            if (model.Ids() != reference.Ordered())
            {
                message = where + "ids " + Describe(model.Ids()) + " expected " + Describe(reference.Ordered());
                return false;
            }
            if (model.Primary() != reference.Primary || model.Anchor() != reference.Anchor)
            {
                message = where + "primary/anchor " + std::to_string(model.Primary()) + "/" + std::to_string(model.Anchor())
                    + " expected " + std::to_string(reference.Primary) + "/" + std::to_string(reference.Anchor);
                return false;
            }
            const bool expectedChange = reference.Members() != membersBefore || reference.Primary != primaryBefore;
            if (changed != expectedChange)
            {
                message = where + "change notification " + std::to_string(changed) + " expected " + std::to_string(expectedChange);
                return false;
            }
            if (model.Revision() != revisionBefore + (expectedChange ? 1 : 0))
            {
                message = where + "revision did not advance exactly on a change";
                return false;
            }
            if (!model.Empty() && !model.IsSelected(model.Primary()))
            {
                message = where + "primary is not a member";
                return false;
            }
            if (model.Count() != reference.Stamp.size())
            {
                message = where + "count mismatch";
                return false;
            }
            for (const EntityId candidate : universe)
            {
                if (model.IsSelected(candidate) != (reference.Stamp.count(candidate) != 0))
                {
                    message = where + "membership mismatch for " + std::to_string(candidate);
                    return false;
                }
            }
        }

        // Undo-style round trip: capture, destroy some entities, restore with everything alive again.
        const SelectionState snapshot = model.Capture();
        std::vector<EntityId> survivors;
        for (const EntityId candidate : universe)
        {
            if (stream.NextBool())
                survivors.push_back(candidate);
        }
        model.Prune(survivors);
        for (const EntityId candidate : model.Ids())
        {
            if (std::find(survivors.begin(), survivors.end(), candidate) == survivors.end())
            {
                message = "prune left a missing entity selected";
                return false;
            }
        }
        const std::set<EntityId> alive(universe.begin(), universe.end());
        const bool allExist = std::all_of(snapshot.Ids.begin(), snapshot.Ids.end(),
            [&](EntityId candidate) { return alive.count(candidate) != 0; })
            && (snapshot.Anchor == 0 || alive.count(snapshot.Anchor) != 0);
        model.Restore(snapshot, universe);
        if (allExist && !(model.Capture() == snapshot))
        {
            const SelectionState got = model.Capture();
            message = "restore with every entity alive did not reproduce the captured state: recorded " + Describe(snapshot.Ids)
                + " primary " + std::to_string(snapshot.Primary) + " anchor " + std::to_string(snapshot.Anchor) + ", got " + Describe(got.Ids)
                + " primary " + std::to_string(got.Primary) + " anchor " + std::to_string(got.Anchor) + ", universe " + Describe(universe);
            return false;
        }
        ReferenceSelection restored;
        restored.Restore(snapshot, universe);
        if (model.Ids() != restored.Ordered() || model.Primary() != restored.Primary || model.Anchor() != restored.Anchor)
        {
            message = "restore after prune disagrees with the reference restore";
            return false;
        }
        return true;
    }

    bool CheckSelectionScripts()
    {
        Checker check { "selection-scripts" };
        const std::vector<EntityId> order = { 1, 2, 3, 4, 5, 6 };
        SelectionModel model;
        const auto Is = [&](std::vector<EntityId> ids, EntityId primary, EntityId anchor, const char* step)
        {
            check.Expect(model.Ids() == ids && model.Primary() == primary && model.Anchor() == anchor,
                std::string(step) + ": ids " + Describe(model.Ids()) + " primary " + std::to_string(model.Primary())
                    + " anchor " + std::to_string(model.Anchor()));
        };

        check.Expect(model.Empty() && model.Count() == 0 && model.Revision() == 0, "a new model is empty at revision 0");
        check.Expect(model.Click(2), "first click reports a change");
        Is({ 2 }, 2, 2, "click 2");
        check.Expect(!model.Click(2) && model.Revision() == 1, "re-clicking the sole selection is not a change");
        check.Expect(model.SelectRange(order, 5, false), "shift range reports a change");
        Is({ 2, 3, 4, 5 }, 5, 2, "range 2..5");
        check.Expect(model.Toggle(3), "ctrl toggle off reports a change");
        Is({ 2, 4, 5 }, 5, 3, "toggle 3 off");
        check.Expect(model.SelectRange(order, 1, true), "additive range reports a change");
        Is({ 2, 4, 5, 1, 3 }, 1, 3, "additive range 3..1");
        check.Expect(model.Toggle(1), "deselecting the primary reports a change");
        Is({ 2, 4, 5, 3 }, 3, 1, "toggle primary off promotes the newest survivor");
        check.Expect(model.Invert(order), "invert reports a change");
        Is({ 1, 6 }, 6, 1, "invert");
        check.Expect(model.InOrder(order) == std::vector<EntityId> { 1, 6 }, "InOrder follows the row order");
        check.Expect(model.InOrder(std::vector<EntityId> { 6, 1, 6, 9 }) == std::vector<EntityId> { 6, 1 },
            "InOrder ignores unselected and repeated rows");
        check.Expect(model.Clear() && model.Empty(), "clear empties");
        Is({}, kNoEntity, kNoEntity, "clear");
        check.Expect(!model.Clear(), "clearing an empty selection is not a change");

        // Rows hidden by a filter: a range is computed in the visible order; select-all and
        // invert leave hidden members alone; a plain click or a plain range replaces everything.
        const std::vector<EntityId> filtered = { 4, 2, 8 };
        model = SelectionModel();
        model.Click(4);
        model.Toggle(9);
        check.Expect(model.SelectRange(filtered, 8, true), "range from a hidden anchor reports a change");
        Is({ 8 }, 8, 8, "a hidden anchor makes the range a plain click");
        model.Click(9);
        check.Expect(model.SelectAll(filtered), "select-all over a filtered list reports a change");
        Is({ 9, 4, 2, 8 }, 9, 9, "select-all keeps the hidden member and the primary");
        check.Expect(model.Invert(filtered), "invert over a filtered list reports a change");
        Is({ 9 }, 9, 9, "invert removes only visible members");
        check.Expect(model.Invert(filtered), "second invert reports a change");
        Is({ 9, 4, 2, 8 }, 9, 9, "invert twice restores the members");
        model.Click(9);
        model.Toggle(4);
        check.Expect(model.SelectRange(filtered, 8, false), "plain range reports a change");
        Is({ 4, 2, 8 }, 8, 4, "a plain range replaces hidden members");
        model.Click(8);
        check.Expect(model.SelectRange(filtered, 4, false), "upward range reports a change");
        Is({ 4, 2, 8 }, 4, 8, "range toward the start of the list");

        // Rejected input changes nothing.
        const SelectionState before = model.Capture();
        const u64 revision = model.Revision();
        check.Expect(!model.Click(kNoEntity) && !model.Toggle(kNoEntity), "the null entity cannot be selected");
        check.Expect(!model.SelectRange(filtered, 77, false), "a range target outside the visible rows is ignored");
        check.Expect(!model.SelectAll({}) && !model.Invert({}), "empty lists change nothing");
        check.Expect(model.Capture() == before && model.Revision() == revision, "rejected operations leave state and revision alone");

        // Undo-style restore: the recorded selection returns only for entities that exist again.
        model = SelectionModel();
        model.Click(1);
        model.Toggle(3);
        model.Toggle(5);
        const SelectionState recorded = model.Capture();
        Is({ 1, 3, 5 }, 5, 5, "recorded selection");
        check.Expect(model.Prune(std::vector<EntityId> { 2, 3, 4, 6 }), "pruning deleted entities reports a change");
        Is({ 3 }, 3, 3, "prune keeps the survivor and repairs primary and anchor");
        check.Expect(!model.Prune(std::vector<EntityId> { 2, 3, 4, 6 }), "pruning again is not a change");
        check.Expect(model.Restore(recorded, std::vector<EntityId> { 1, 2, 3, 4, 5, 6 }), "restoring after the undo reports a change");
        check.Expect(model.Capture() == recorded, "restore reproduces the recorded state exactly");
        check.Expect(model.Restore(recorded, std::vector<EntityId> { 2, 3, 4, 6 }), "restore without the deleted entities changes the selection");
        Is({ 3 }, 3, 3, "restore drops missing entities");
        SelectionState hostile;
        hostile.Ids = { 3, 3, 0, 2 };
        hostile.Primary = 7;
        hostile.Anchor = 99;
        model.Restore(hostile, std::vector<EntityId> { 2, 3 });
        Is({ 3, 2 }, 2, 2, "restore repairs duplicates, null ids, a missing primary and a missing anchor");
        return check.Ok;
    }

    // ===== naming ==================================================================

    // RFC 3629 Table 3-7 well-formed byte sequences, written as ranges.
    bool ReferenceWellFormedUtf8(std::string_view text)
    {
        const auto at = [&](size_t index) { return static_cast<unsigned char>(text[index]); };
        const auto between = [&](size_t index, unsigned char low, unsigned char high)
        {
            return index < text.size() && at(index) >= low && at(index) <= high;
        };
        size_t index = 0;
        while (index < text.size())
        {
            const unsigned char lead = at(index);
            if (lead <= 0x7F)
                index += 1;
            else if (lead >= 0xC2 && lead <= 0xDF && between(index + 1, 0x80, 0xBF))
                index += 2;
            else if (lead == 0xE0 && between(index + 1, 0xA0, 0xBF) && between(index + 2, 0x80, 0xBF))
                index += 3;
            else if (((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF)
                && between(index + 1, 0x80, 0xBF) && between(index + 2, 0x80, 0xBF))
                index += 3;
            else if (lead == 0xED && between(index + 1, 0x80, 0x9F) && between(index + 2, 0x80, 0xBF))
                index += 3;
            else if (lead == 0xF0 && between(index + 1, 0x90, 0xBF) && between(index + 2, 0x80, 0xBF) && between(index + 3, 0x80, 0xBF))
                index += 4;
            else if (lead >= 0xF1 && lead <= 0xF3 && between(index + 1, 0x80, 0xBF) && between(index + 2, 0x80, 0xBF)
                && between(index + 3, 0x80, 0xBF))
                index += 4;
            else if (lead == 0xF4 && between(index + 1, 0x80, 0x8F) && between(index + 2, 0x80, 0xBF) && between(index + 3, 0x80, 0xBF))
                index += 4;
            else
                return false;
        }
        return true;
    }

    bool ReferenceHasControl(std::string_view text)
    {
        for (size_t index = 0; index < text.size(); ++index)
        {
            const unsigned char value = static_cast<unsigned char>(text[index]);
            if (value < 0x20 || value == 0x7F)
                return true;
            // C1 controls are U+0080..U+009F, encoded C2 80..C2 9F.
            if (value == 0xC2 && index + 1 < text.size() && static_cast<unsigned char>(text[index + 1]) >= 0x80
                && static_cast<unsigned char>(text[index + 1]) <= 0x9F)
                return true;
        }
        return false;
    }

    EntityNameStatus ReferenceValidate(std::string_view name)
    {
        if (name.empty())
            return EntityNameStatus::Empty;
        if (name.size() > 127)
            return EntityNameStatus::TooLong;
        if (!ReferenceWellFormedUtf8(name))
            return EntityNameStatus::InvalidUtf8;
        if (ReferenceHasControl(name))
            return EntityNameStatus::ControlCharacter;
        if (name.front() == ' ' || name.back() == ' ')
            return EntityNameStatus::EdgeSpace;
        return EntityNameStatus::Valid;
    }

    std::string ReferenceResolve(const std::set<std::string>& taken, const std::string& requested)
    {
        std::set<std::string> blocked = taken;
        for (const std::string_view reserved : LookupReservedEntityNames())
            blocked.emplace(reserved);
        if (!blocked.count(requested))
            return requested;
        std::string stem = requested;
        static const std::regex suffix(R"(^(.+) \(([1-9][0-9]{0,8})\)$)");
        std::smatch match;
        if (std::regex_match(requested, match, suffix))
            stem = match[1].str();
        for (unsigned copy = 2;; ++copy)
        {
            const std::string candidate = stem + " (" + std::to_string(copy) + ")";
            if (!blocked.count(candidate))
                return candidate;
        }
    }

    std::string RandomBytes(ChoiceStream& stream, size_t maximumLength)
    {
        static const std::vector<unsigned char> specials = { 0x00, 0x09, 0x0A, 0x1F, 0x20, 0x7F, 0x80, 0xBF, 0xC0, 0xC1, 0xC2,
            0xDF, 0xE0, 0xED, 0xEF, 0xF0, 0xF4, 0xF5, 0xFF, 'a', 'Z', '(', ')', '2' };
        std::string bytes;
        const size_t length = stream.NextSize(0, maximumLength);
        for (size_t index = 0; index < length; ++index)
        {
            bytes.push_back(stream.NextSize(0, 2) == 0 ? static_cast<char>(Pick(stream, specials))
                                                       : static_cast<char>(stream.NextSize(0, 255)));
        }
        return bytes;
    }

    bool NamingPropertyOnce(ChoiceStream& stream, std::string& message)
    {
        // Sanitizer: total, valid, idempotent, identity on valid names, validator agrees with the reference.
        for (int round = 0; round < 8; ++round)
        {
            const std::string raw = stream.NextBool() ? RandomBytes(stream, 200) : std::string(stream.NextSize(120, 140), 'k') + RandomBytes(stream, 8);
            const std::string repaired = SanitizeEntityName(raw);
            if (ValidateEntityName(repaired) != EntityNameStatus::Valid)
            {
                message = "sanitized name is not valid";
                return false;
            }
            if (SanitizeEntityName(repaired) != repaired)
            {
                message = "sanitize is not idempotent";
                return false;
            }
            if (ValidateEntityName(raw) != ReferenceValidate(raw))
            {
                message = "validator status " + std::to_string(static_cast<int>(ValidateEntityName(raw))) + " disagrees with the reference "
                + std::to_string(static_cast<int>(ReferenceValidate(raw))) + " for a generated name of " + std::to_string(raw.size()) + " bytes";
                return false;
            }
            if (ValidateEntityName(raw) == EntityNameStatus::Valid && repaired != raw)
            {
                message = "sanitize changed a valid name";
                return false;
            }
        }

        // Pool: resolve and claim agree with the naive scan; results are unique, reserved-free, and idempotent.
        const std::vector<std::string> stems = { "Cube", "Sphere", "Entity", "Prototype Mesh", "Directional Light",
            "Player Start", "Name (0)", "Name (02)", "Name (2)", "a", "x (1)", "Deep (3) (4)" };
        std::map<std::string, size_t> occupied;
        EntityNamePool pool;
        const auto Occupy = [&](const std::string& name)
        {
            ++occupied[name];
            pool.Add(name);
        };
        const auto Taken = [&]()
        {
            std::set<std::string> names;
            for (const auto& [name, count] : occupied)
            {
                if (count != 0)
                    names.insert(name);
            }
            return names;
        };
        const size_t population = stream.NextSize(0, 14);
        for (size_t index = 0; index < population; ++index)
        {
            const std::string& stem = Pick(stream, stems);
            Occupy(stream.NextBool() ? stem : stem + " (" + std::to_string(stream.NextSize(2, 7)) + ")");
        }

        const size_t claims = stream.NextSize(1, 24);
        for (size_t index = 0; index < claims; ++index)
        {
            const std::string& stem = Pick(stream, stems);
            const std::string requested = stream.NextBool() ? stem : stem + " (" + std::to_string(stream.NextSize(1, 9)) + ")";
            const std::string expected = ReferenceResolve(Taken(), requested);
            const std::string resolved = pool.Resolve(requested);
            if (resolved != expected)
            {
                message = "Resolve(\"" + requested + "\") = \"" + resolved + "\" expected \"" + expected + "\"";
                return false;
            }
            if (pool.Resolve(resolved) != resolved)
            {
                message = "Resolve is not idempotent for \"" + resolved + "\"";
                return false;
            }
            if (pool.IsTaken(resolved) || ValidateEntityName(resolved) != EntityNameStatus::Valid)
            {
                message = "resolved name is taken or invalid: \"" + resolved + "\"";
                return false;
            }
            const std::string claimed = pool.Claim(requested);
            if (claimed != expected)
            {
                message = "Claim disagrees with Resolve";
                return false;
            }
            ++occupied[claimed];
            for (const std::string_view reserved : LookupReservedEntityNames())
            {
                if (claimed == reserved)
                {
                    message = "claimed a reserved lookup name";
                    return false;
                }
            }

            // Releasing a name between resolutions must make the lowest free number available again.
            if (stream.NextSize(0, 2) == 0)
            {
                const std::set<std::string> names = Taken();
                if (!names.empty())
                {
                    auto victim = names.begin();
                    std::advance(victim, static_cast<std::ptrdiff_t>(stream.NextSize(0, names.size() - 1)));
                    if (!pool.Remove(*victim))
                    {
                        message = "Remove refused an occupied name";
                        return false;
                    }
                    --occupied[*victim];
                }
                if (pool.Remove("Never Occupied Name"))
                {
                    message = "Remove released a name that was never occupied";
                    return false;
                }
            }
        }

        // Remove releases exactly one occurrence.
        const std::string& probe = Pick(stream, stems);
        pool.Add(probe);
        pool.Add(probe);
        pool.Remove(probe);
        if (!pool.IsTaken(probe))
        {
            message = "Remove released more than one occurrence";
            return false;
        }
        return true;
    }

    bool CheckNamingTables()
    {
        Checker check { "naming-tables" };

        const std::string longName(127, 'a');
        const struct { std::string Name; EntityNameStatus Expected; } validation[] = {
            { "", EntityNameStatus::Empty },
            { " a", EntityNameStatus::EdgeSpace },
            { "a ", EntityNameStatus::EdgeSpace },
            { "a b", EntityNameStatus::Valid },
            { longName, EntityNameStatus::Valid },
            { longName + "a", EntityNameStatus::TooLong },
            { std::string("a\nb"), EntityNameStatus::ControlCharacter },
            { std::string("a\tb"), EntityNameStatus::ControlCharacter },
            { std::string("a\0b", 3), EntityNameStatus::ControlCharacter },
            { "a\x7F", EntityNameStatus::ControlCharacter },
            { "a\xC2\x85", EntityNameStatus::ControlCharacter },
            { "\xC3\xA9t\xC3\xA9", EntityNameStatus::Valid },
            { "\xF0\x9F\x98\x80", EntityNameStatus::Valid },
            { "\xC0\xAF", EntityNameStatus::InvalidUtf8 },
            { "\xED\xA0\x80", EntityNameStatus::InvalidUtf8 },
            { "\xF4\x90\x80\x80", EntityNameStatus::InvalidUtf8 },
            { "\xE2\x82", EntityNameStatus::InvalidUtf8 },
            { "\xFF", EntityNameStatus::InvalidUtf8 },
        };
        for (const auto& row : validation)
            check.Expect(ValidateEntityName(row.Name) == row.Expected && ReferenceValidate(row.Name) == row.Expected,
                "validation of a table name: " + std::to_string(static_cast<int>(row.Expected)));

        const struct { std::string Raw; std::string Expected; } sanitized[] = {
            { "  Cube  ", "Cube" },
            { "", "Entity" },
            { "   ", "Entity" },
            { "a\nb", "a b" },
            { "a\xFF" "b", "a?b" },
            { "\xC0\xAF", "??" },
            { "\xF0\x9F\x98\x80", "\xF0\x9F\x98\x80" },
            { std::string(200, 'z'), std::string(127, 'z') },
            { std::string(126, 'z') + "\xF0\x9F\x98\x80", std::string(126, 'z') },
            { std::string(126, 'z') + " " + std::string(10, 'y'), std::string(126, 'z') },
            { std::string(125, 'z') + " " + std::string(10, 'y'), std::string(125, 'z') + " y" },
        };
        for (const auto& row : sanitized)
            check.Expect(SanitizeEntityName(row.Raw) == row.Expected, "sanitize of \"" + row.Raw.substr(0, 20) + "\"");
        check.Expect(TrimEntityName("  x y  ") == "x y" && TrimEntityName("   ").empty(), "trim removes only edge spaces");

        const auto Resolve = [](std::vector<std::string> taken, std::string_view requested)
        {
            EntityNamePool pool;
            for (const std::string& name : taken)
                pool.Add(name);
            return pool.Resolve(requested);
        };
        check.Expect(Resolve({}, "Cube") == "Cube", "a free name is kept");
        check.Expect(Resolve({ "Cube" }, "Cube") == "Cube (2)", "first collision gets (2)");
        check.Expect(Resolve({ "Cube", "Cube (2)" }, "Cube") == "Cube (3)", "second collision gets (3)");
        check.Expect(Resolve({ "Cube", "Cube (3)" }, "Cube") == "Cube (2)", "the lowest free number is used");
        check.Expect(Resolve({ "Cube", "Cube (2)" }, "Cube (2)") == "Cube (3)", "duplicating a copy does not stack suffixes");
        check.Expect(Resolve({ "Cube" }, "Cube (2)") == "Cube (2)", "a free explicit suffix is kept");
        check.Expect(Resolve({ "Cube (2)" }, "Cube (2)") == "Cube (3)", "a taken copy name resolves from its stem even when the stem is free");
        check.Expect(Resolve({ "Name (0)" }, "Name (0)") == "Name (0) (2)", "(0) is not a copy suffix");
        check.Expect(Resolve({ "Name (02)" }, "Name (02)") == "Name (02) (2)", "(02) is not a copy suffix");
        check.Expect(Resolve({ "Name (2) (3)" }, "Name (2) (3)") == "Name (2) (2)", "only the last suffix is a copy suffix");
        check.Expect(Resolve({}, "(2)") == "(2)", "a bare (2) has no stem and is kept");
        check.Expect(Resolve({}, "Prototype Mesh") == "Prototype Mesh (2)", "a reserved lookup name is never produced, even when absent");
        check.Expect(Resolve({}, "Directional Light") == "Directional Light (2)", "reserved Directional Light");
        check.Expect(Resolve({}, "Player Start") == "Player Start (2)", "reserved Player Start");
        check.Expect(Resolve({}, "Editor Control Shared Peer") == "Editor Control Shared Peer (2)", "reserved Editor Control Shared Peer");
        check.Expect(Resolve({}, "Authored Entity") == "Authored Entity (2)", "reserved Authored Entity");
        check.Expect(Resolve({ "Entity" }, "") == "Entity (2)", "an empty request becomes the default name, then resolves");
        check.Expect(Resolve({}, "  Cube  ") == "Cube", "requests are sanitized");

        EntityNamePool released;
        released.Add("Cube");
        check.Expect(released.Claim("Cube") == "Cube (2)" && released.Claim("Cube") == "Cube (3)", "claims take consecutive numbers");
        check.Expect(released.Remove("Cube (2)") && released.Resolve("Cube") == "Cube (2)", "a released number is the lowest free number again");
        check.Expect(released.Remove("Cube") && released.Resolve("Cube") == "Cube", "releasing the stem frees the stem itself");
        check.Expect(!released.Remove("Cube") && !released.Remove("Cube (9)"), "removing an unoccupied name reports false");

        // Length bound: a 127-byte name leaves room for " (N)" by shortening the stem on a code point boundary.
        EntityNamePool pool;
        pool.Add(longName);
        const std::string shortened = pool.Resolve(longName);
        check.Expect(shortened == std::string(123, 'a') + " (2)" && shortened.size() == 127, "a full-length name is shortened to fit the suffix");
        std::string emoji;
        for (int index = 0; index < 31; ++index)
            emoji += "\xF0\x9F\x98\x80"; // 124 bytes
        emoji += "abc";                  // 127 bytes
        EntityNamePool emojiPool;
        emojiPool.Add(emoji);
        std::set<std::string> distinct;
        for (int index = 0; index < 150; ++index)
        {
            const std::string claimed = emojiPool.Claim(emoji);
            check.Expect(ValidateEntityName(claimed) == EntityNameStatus::Valid && claimed.size() <= kMaxEntityNameBytes,
                "claimed multi-byte name stays valid and bounded");
            distinct.insert(claimed);
        }
        check.Expect(distinct.size() == 150, "150 claims of a multi-byte full-length name are all distinct");

        // Bulk claims stay unique (paste of the maximum clipboard entity count).
        EntityNamePool bulk;
        bulk.Add("Entity");
        std::unordered_set<std::string> bulkNames;
        for (u32 index = 0; index < kMaxClipboardEntities; ++index)
            bulkNames.insert(bulk.Claim("Entity"));
        check.Expect(bulkNames.size() == kMaxClipboardEntities && bulkNames.count("Entity (2)") == 1
                && bulkNames.count("Entity (" + std::to_string(kMaxClipboardEntities + 1) + ")") == 1,
            "4096 pasted copies get 4096 distinct consecutive names");
        return check.Ok;
    }

    // ===== hierarchy ===============================================================

    std::string LowerAscii(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value)
        {
            return static_cast<char>(value >= 'A' && value <= 'Z' ? value - 'A' + 'a' : value);
        });
        return text;
    }

    std::vector<EntityId> ReferenceVisible(const std::vector<HierarchyRow>& rows, const HierarchyFilter& filter)
    {
        std::vector<EntityId> ids;
        for (const HierarchyRow& row : rows)
        {
            bool match = LowerAscii(row.Name).find(LowerAscii(filter.Text)) != std::string::npos;
            for (u32 bit = 0; bit < 32; ++bit)
            {
                if ((filter.RequiredComponents >> bit & 1u) != 0 && (row.Components >> bit & 1u) == 0)
                    match = false;
            }
            if (filter.Visibility == VisibilityFilter::VisibleOnly && !row.Visible)
                match = false;
            if (filter.Visibility == VisibilityFilter::HiddenOnly && row.Visible)
                match = false;
            if (match)
                ids.push_back(row.Id);
        }
        return ids;
    }

    std::vector<HierarchyRow> GenerateRows(ChoiceStream& stream, bool allowBadIds)
    {
        static const std::vector<std::string> names = { "Cube", "cube (2)", "CUBE", "Sphere", "Directional Light", "Main Camera",
            "\xC3\x89" "cole", "\xC3\xA9" "cole", "x", "Prototype Mesh", "a b" };
        std::vector<HierarchyRow> rows;
        const size_t count = stream.NextSize(0, 14);
        for (size_t index = 0; index < count; ++index)
        {
            HierarchyRow row;
            row.Id = static_cast<EntityId>(stream.NextSize(1, 40));
            if (!allowBadIds)
            {
                while (std::any_of(rows.begin(), rows.end(), [&](const HierarchyRow& other) { return other.Id == row.Id; }))
                    row.Id = static_cast<EntityId>(stream.NextSize(1, 400));
            }
            else if (stream.NextSize(0, 9) == 0)
            {
                row.Id = 0;
            }
            row.Name = Pick(stream, names);
            row.Components = static_cast<u32>(stream.NextSize(0, 7));
            row.Visible = stream.NextBool();
            rows.push_back(std::move(row));
        }
        return rows;
    }

    HierarchyFilter GenerateFilter(ChoiceStream& stream)
    {
        static const std::vector<std::string> needles = { "", "c", "CU", "cube", "Cube (", "(2)", "e", "\xC3\x89", "\xC3\xA9", "zzz", " " };
        HierarchyFilter filter;
        filter.Text = Pick(stream, needles);
        filter.RequiredComponents = static_cast<u32>(stream.NextSize(0, 7));
        filter.Visibility = static_cast<VisibilityFilter>(stream.NextSize(0, 2));
        return filter;
    }

    bool HierarchyPropertyOnce(ChoiceStream& stream, std::string& message)
    {
        HierarchyModel model;
        std::vector<HierarchyRow> current;
        HierarchyFilter filter;
        for (int step = 0; step < 12; ++step)
        {
            const std::vector<HierarchyRow> proposed = GenerateRows(stream, true);
            std::vector<HierarchyRow> sanitized;
            std::set<EntityId> seen;
            for (const HierarchyRow& row : proposed)
            {
                if (row.Id != 0 && seen.insert(row.Id).second)
                    sanitized.push_back(row);
            }
            const bool rowsChanged = model.SetRows(proposed);
            if (rowsChanged != (sanitized != current))
            {
                message = "SetRows change notification is not equality based";
                return false;
            }
            if (model.SetRows(proposed))
            {
                message = "SetRows with identical rows reported a change";
                return false;
            }
            current = sanitized;
            if (model.Rows() != current)
            {
                message = "rows are not the sanitized input";
                return false;
            }

            const HierarchyFilter next = GenerateFilter(stream);
            const bool filterChanged = model.SetFilter(next);
            if (filterChanged != !(next == filter) || model.SetFilter(next))
            {
                message = "SetFilter change notification is not equality based";
                return false;
            }
            filter = next;

            if (model.VisibleIds() != ReferenceVisible(current, filter))
            {
                message = "visible ids " + Describe(model.VisibleIds()) + " expected " + Describe(ReferenceVisible(current, filter));
                return false;
            }
            for (const HierarchyRow& row : current)
            {
                const HierarchyRow* found = model.FindRow(row.Id);
                if (!found || !(*found == row))
                {
                    message = "FindRow disagrees with the rows";
                    return false;
                }
            }
        }
        return true;
    }

    // Independent reorder oracle: stable sort by a position key. Unmoved rows keep their own
    // index; the block takes the key just below the first unmoved row at or after the drop row.
    std::vector<EntityId> ReferenceReorder(const std::vector<EntityId>& order, const std::vector<EntityId>& moved, EntityId insertBefore)
    {
        const std::set<EntityId> movedSet(moved.begin(), moved.end());
        long dropIndex = static_cast<long>(order.size());
        if (insertBefore != 0)
            dropIndex = std::find(order.begin(), order.end(), insertBefore) - order.begin();
        long anchorIndex = static_cast<long>(order.size());
        for (long index = dropIndex; index < static_cast<long>(order.size()); ++index)
        {
            if (!movedSet.count(order[static_cast<size_t>(index)]))
            {
                anchorIndex = index;
                break;
            }
        }
        std::vector<std::pair<std::pair<long, long>, EntityId>> keyed;
        for (long index = 0; index < static_cast<long>(order.size()); ++index)
        {
            const EntityId id = order[static_cast<size_t>(index)];
            keyed.push_back(movedSet.count(id) ? std::make_pair(std::make_pair(anchorIndex, -1L), id)
                                               : std::make_pair(std::make_pair(index, 0L), id));
        }
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& left, const auto& right)
        {
            if (left.first.first != right.first.first)
                return left.first.first < right.first.first;
            return left.first.second < right.first.second;
        });
        std::vector<EntityId> result;
        for (const auto& entry : keyed)
            result.push_back(entry.second);
        return result;
    }

    bool ReorderPropertyOnce(ChoiceStream& stream, std::string& message)
    {
        std::vector<EntityId> order;
        const size_t count = stream.NextSize(0, 12);
        for (size_t index = 0; index < count; ++index)
            order.push_back(static_cast<EntityId>(index * 3 + 1));
        for (size_t index = order.size(); index > 1; --index)
            std::swap(order[index - 1], order[stream.NextSize(0, index - 1)]);

        std::vector<EntityId> moved;
        for (const EntityId id : order)
        {
            if (stream.NextSize(0, 3) == 0)
                moved.push_back(id);
        }
        for (size_t index = moved.size(); index > 1; --index)
            std::swap(moved[index - 1], moved[stream.NextSize(0, index - 1)]);
        const EntityId insertBefore = order.empty() || stream.NextSize(0, 5) == 0 ? 0 : order[stream.NextSize(0, order.size() - 1)];

        const std::optional<std::vector<EntityId>> result = ReorderEntities(order, moved, insertBefore);
        if (moved.empty())
        {
            if (result.has_value())
            {
                message = "an empty move was accepted";
                return false;
            }
            return true;
        }
        if (!result)
        {
            message = "a valid move was rejected";
            return false;
        }
        const std::vector<EntityId> expected = ReferenceReorder(order, moved, insertBefore);
        if (*result != expected)
        {
            message = "reorder " + Describe(order) + " move " + Describe(moved) + " before " + std::to_string(insertBefore)
                + " gave " + Describe(*result) + " expected " + Describe(expected);
            return false;
        }

        // Structural invariants stated independently of the oracle's key construction.
        std::vector<EntityId> sortedResult = *result;
        std::vector<EntityId> sortedOrder = order;
        std::sort(sortedResult.begin(), sortedResult.end());
        std::sort(sortedOrder.begin(), sortedOrder.end());
        if (sortedResult != sortedOrder)
        {
            message = "reorder is not a permutation";
            return false;
        }
        const std::set<EntityId> movedSet(moved.begin(), moved.end());
        std::vector<EntityId> unmovedBefore, unmovedAfter, blockBefore, blockAfter;
        for (const EntityId id : order)
            (movedSet.count(id) ? blockBefore : unmovedBefore).push_back(id);
        for (const EntityId id : *result)
            (movedSet.count(id) ? blockAfter : unmovedAfter).push_back(id);
        if (unmovedBefore != unmovedAfter || blockBefore != blockAfter)
        {
            message = "reorder changed the relative order inside the block or among the unmoved rows";
            return false;
        }
        size_t first = result->size();
        size_t last = 0;
        for (size_t index = 0; index < result->size(); ++index)
        {
            if (movedSet.count((*result)[index]))
            {
                first = std::min(first, index);
                last = index;
            }
        }
        if (last - first + 1 != movedSet.size())
        {
            message = "the moved block is not contiguous";
            return false;
        }
        return true;
    }

    bool CheckHierarchyTables()
    {
        Checker check { "hierarchy-tables" };

        // Locks are session state keyed by id.
        HierarchyModel model;
        model.SetRows({ { 1, "A", 0, true }, { 2, "B", 0, true }, { 3, "C", 0, true } });
        check.Expect(!model.SetLocked(99, true), "an unknown id cannot be newly locked");
        check.Expect(model.SetLocked(2, true) && !model.SetLocked(2, true), "locking reports change once");
        check.Expect(model.IsLocked(2) && !model.IsLocked(1), "lock flag is per entity");
        check.Expect(model.Unlocked(std::vector<EntityId> { 1, 2, 3 }) == std::vector<EntityId> { 1, 3 }, "Unlocked drops locked ids");
        model.SetRows({ { 1, "A", 0, true }, { 3, "C", 0, true } });
        check.Expect(model.IsLocked(2), "a lock survives deleting the entity so an undo restores it");
        model.SetRows({ { 1, "A", 0, true }, { 2, "B", 0, true }, { 3, "C", 0, true } });
        check.Expect(model.IsLocked(2), "the lock is back after the entity returns");
        check.Expect(model.SetLocked(2, false) && !model.SetLocked(2, false) && !model.IsLocked(2), "unlocking reports change once");
        model.SetLocked(1, true);
        model.ClearLocks();
        check.Expect(!model.IsLocked(1), "ClearLocks forgets every lock");

        // Duplicate and null ids are dropped, first wins.
        check.Expect(model.SetRows({ { 5, "first", 0, true }, { 0, "null", 0, true }, { 5, "dupe", 0, true }, { 6, "ok", 0, true } }),
            "rows with null and duplicate ids are accepted after dropping them");
        check.Expect(model.Rows().size() == 2 && model.Rows()[0].Name == "first" && model.Rows()[1].Id == 6,
            "the first row for an id wins and the null id is dropped");

        // Rename plans.
        model.SetRows({ { 1, "Cube", 0, true }, { 2, "Cube (2)", 0, true }, { 3, "Prototype Mesh", 0, true }, { 4, "Sphere", 0, true } });
        const auto Plan = [&](EntityId id, std::string_view requested) { return model.PlanRename(id, requested); };
        RenamePlan plan = Plan(4, "Cube");
        check.Expect(plan.Outcome == RenameOutcome::Renamed && plan.Name == "Cube (3)" && plan.Adjusted, "colliding rename is suffixed past existing copies");
        plan = Plan(4, "  Torus  ");
        check.Expect(plan.Outcome == RenameOutcome::Renamed && plan.Name == "Torus" && !plan.Adjusted, "rename trims and accepts a free name");
        plan = Plan(2, "Cube (2)  ");
        check.Expect(plan.Outcome == RenameOutcome::Unchanged && plan.Name == "Cube (2)", "renaming to the same trimmed name is unchanged");
        plan = Plan(2, "Cube");
        check.Expect(plan.Outcome == RenameOutcome::Unchanged && plan.Adjusted, "renaming a copy back to a taken stem resolves to its own name");
        plan = Plan(3, "Foo");
        check.Expect(plan.Outcome == RenameOutcome::ProtectedEntity, "the entity holding a reserved lookup name cannot be renamed");
        plan = Plan(3, "Prototype Mesh");
        check.Expect(plan.Outcome == RenameOutcome::Unchanged, "a protected entity keeping its name is unchanged, not an error");
        plan = Plan(4, "Prototype Mesh");
        check.Expect(plan.Outcome == RenameOutcome::Renamed && plan.Name == "Prototype Mesh (2)" && plan.Adjusted,
            "no other entity may take a reserved lookup name");
        plan = Plan(4, "");
        check.Expect(plan.Outcome == RenameOutcome::InvalidName && plan.Status == EntityNameStatus::Empty, "empty rename is invalid");
        plan = Plan(4, "   ");
        check.Expect(plan.Outcome == RenameOutcome::InvalidName && plan.Status == EntityNameStatus::Empty, "blank rename is invalid");
        plan = Plan(4, "a\nb");
        check.Expect(plan.Outcome == RenameOutcome::InvalidName && plan.Status == EntityNameStatus::ControlCharacter, "control characters are invalid");
        plan = Plan(4, std::string(128, 'q'));
        check.Expect(plan.Outcome == RenameOutcome::InvalidName && plan.Status == EntityNameStatus::TooLong, "over-long rename is invalid");
        plan = Plan(4, "\xFF");
        check.Expect(plan.Outcome == RenameOutcome::InvalidName && plan.Status == EntityNameStatus::InvalidUtf8, "invalid UTF-8 rename is invalid");
        plan = Plan(99, "x");
        check.Expect(plan.Outcome == RenameOutcome::UnknownEntity, "unknown entity");
        model.SetRows({ { 1, "Cube", 0, true }, { 4, "Sphere", 0, true } });
        plan = Plan(4, "Prototype Mesh");
        check.Expect(plan.Name == "Prototype Mesh (2)", "a reserved name stays reserved while its owner is deleted (undo would restore it)");

        // Reorder invalid input and the drop-onto-itself case.
        const std::vector<EntityId> order = { 1, 2, 3, 4 };
        check.Expect(!ReorderEntities(order, {}, 0), "empty move");
        check.Expect(!ReorderEntities(order, std::vector<EntityId> { 9 }, 0), "unknown moved id");
        check.Expect(!ReorderEntities(order, std::vector<EntityId> { 1, 1 }, 0), "duplicate moved id");
        check.Expect(!ReorderEntities(order, std::vector<EntityId> { 1 }, 9), "unknown drop target");
        check.Expect(!ReorderEntities(std::vector<EntityId> { 1, 1 }, std::vector<EntityId> { 1 }, 0), "duplicate rows");
        check.Expect(!ReorderEntities(std::vector<EntityId> { 0, 1 }, std::vector<EntityId> { 1 }, 0), "null row");
        check.Expect(ReorderEntities(order, std::vector<EntityId> { 2, 3 }, 3) == order, "dropping a block onto itself is a no-op");
        check.Expect(ReorderEntities(order, std::vector<EntityId> { 4 }, 2) == std::vector<EntityId> { 1, 4, 2, 3 }, "move before a row");
        check.Expect(ReorderEntities(order, std::vector<EntityId> { 1, 3 }, 0) == std::vector<EntityId> { 2, 4, 1, 3 }, "move to the end");
        check.Expect(ReorderEntities(order, std::vector<EntityId> { 3, 1 }, 4) == std::vector<EntityId> { 2, 1, 3, 4 }, "block keeps list order");
        return check.Ok;
    }

    // ===== clipboard ===============================================================

    // Hand-built header; the digest comes from the caller (an independent tool for the golden).
    std::string BuildClipboardText(std::string_view version, std::string_view count, std::string_view length,
        std::string_view digest, std::string_view body)
    {
        return "SpiralEntityClipboard " + std::string(version) + ' ' + std::string(count) + ' ' + std::string(length) + ' '
            + std::string(digest) + '\n' + std::string(body);
    }

    std::string RandomBody(ChoiceStream& stream, size_t maximumLength)
    {
        std::string body;
        const size_t length = stream.NextSize(1, maximumLength);
        for (size_t index = 0; index < length; ++index)
        {
            char value = static_cast<char>(stream.NextSize(1, 255));
            body.push_back(value);
        }
        return body;
    }

    bool ClipboardPropertyOnce(ChoiceStream& stream, std::string& message)
    {
        EntityClipboardPayload payload;
        payload.EntityCount = static_cast<u32>(stream.NextSize(1, kMaxClipboardEntities));
        payload.Body = RandomBody(stream, 600);
        std::string text;
        if (EncodeEntityClipboard(payload, text) != ClipboardStatus::Ok)
        {
            message = "a valid payload was not encoded";
            return false;
        }
        EntityClipboardPayload decoded;
        decoded.EntityCount = 777;
        decoded.Body = "sentinel";
        if (DecodeEntityClipboard(text, decoded) != ClipboardStatus::Ok || !(decoded == payload))
        {
            message = "round trip did not reproduce the payload";
            return false;
        }

        const EntityClipboardPayload sentinel { 777, "sentinel" };
        const auto MustReject = [&](const std::string& corrupted, const char* what) -> bool
        {
            EntityClipboardPayload out = sentinel;
            if (DecodeEntityClipboard(corrupted, out) == ClipboardStatus::Ok)
            {
                message = std::string(what) + " was accepted";
                return false;
            }
            if (!(out == sentinel))
            {
                message = std::string(what) + " modified the output on failure";
                return false;
            }
            return true;
        };

        const size_t position = stream.NextSize(0, text.size() - 1);
        std::string mutated = text;
        mutated[position] = static_cast<char>(mutated[position] ^ static_cast<char>(1u << stream.NextSize(0, 7)));
        if (!MustReject(mutated, "a single-bit mutation"))
            return false;
        if (!MustReject(text.substr(0, stream.NextSize(0, text.size() - 1)), "a truncation"))
            return false;
        return MustReject(text + std::string(1, static_cast<char>(stream.NextSize(0, 255))), "a trailing byte");
    }

    bool CheckClipboardTables()
    {
        Checker check { "clipboard-tables" };

        // Golden: the digest was produced by Python hashlib, not by Engine::Sha256Builder.
        const std::string goldenBody = "entity A\nentity B\n";
        const std::string golden = BuildClipboardText("1", "2", "18", "2ff8ce2c0e010551c09cadf8540cb75163c15d011b4c9dd114e6f04ac7b6c47a", goldenBody);
        std::string encoded;
        check.Expect(EncodeEntityClipboard({ 2, goldenBody }, encoded) == ClipboardStatus::Ok && encoded == golden,
            "encoding matches the hand-built golden bytes");
        EntityClipboardPayload decoded;
        check.Expect(DecodeEntityClipboard(golden, decoded) == ClipboardStatus::Ok && decoded == EntityClipboardPayload { 2, goldenBody },
            "the golden bytes decode");

        const std::string digest = "2ff8ce2c0e010551c09cadf8540cb75163c15d011b4c9dd114e6f04ac7b6c47a";
        const EntityClipboardPayload sentinel { 9, "sentinel" };
        const auto Status = [&](const std::string& text)
        {
            EntityClipboardPayload out = sentinel;
            const ClipboardStatus status = DecodeEntityClipboard(text, out);
            check.Expect((status == ClipboardStatus::Ok) != (out == sentinel), "output is modified exactly when decoding succeeds");
            return status;
        };

        check.Expect(Status("") == ClipboardStatus::NotAClipboard, "empty text");
        check.Expect(Status("hello") == ClipboardStatus::NotAClipboard, "foreign text");
        check.Expect(Status("SpiralEntityClipboard") == ClipboardStatus::NotAClipboard, "magic alone");
        check.Expect(Status("SpiralEntityClipboard1 2 18 x\n") == ClipboardStatus::NotAClipboard, "magic not followed by a space");
        check.Expect(Status("SpiralEntityClipboard 1 2 18") == ClipboardStatus::Truncated, "header without a newline");
        check.Expect(Status(BuildClipboardText("0", "2", "18", digest, goldenBody)) == ClipboardStatus::UnsupportedVersion, "version 0");
        check.Expect(Status(BuildClipboardText("2", "2", "18", digest, goldenBody)) == ClipboardStatus::UnsupportedVersion, "version 2");
        check.Expect(Status(BuildClipboardText("4294967295", "2", "18", digest, goldenBody)) == ClipboardStatus::UnsupportedVersion, "version u32 max");
        check.Expect(Status(BuildClipboardText("01", "2", "18", digest, goldenBody)) == ClipboardStatus::Malformed, "leading zero version");
        check.Expect(Status(BuildClipboardText("+1", "2", "18", digest, goldenBody)) == ClipboardStatus::Malformed, "signed version");
        check.Expect(Status(BuildClipboardText("1", "2", "018", digest, goldenBody)) == ClipboardStatus::Malformed, "leading zero length");
        check.Expect(Status(BuildClipboardText("1", "2", "99999999999", digest, goldenBody)) == ClipboardStatus::Malformed, "length wider than u32");
        check.Expect(Status(BuildClipboardText("1", "0", "18", digest, goldenBody)) == ClipboardStatus::Malformed, "zero entities");
        check.Expect(Status(BuildClipboardText("1", "4097", "18", digest, goldenBody)) == ClipboardStatus::TooLarge, "too many entities");
        check.Expect(Status(BuildClipboardText("1", "2", "8388609", digest, goldenBody)) == ClipboardStatus::TooLarge, "declared body over the limit");
        check.Expect(Status(BuildClipboardText("1", "2", "0", digest, "")) == ClipboardStatus::Malformed, "empty body");
        check.Expect(Status(BuildClipboardText("1", "2", "19", digest, goldenBody)) == ClipboardStatus::Truncated, "body shorter than declared");
        check.Expect(Status(BuildClipboardText("1", "2", "17", digest, goldenBody)) == ClipboardStatus::Malformed, "body longer than declared");
        check.Expect(Status(BuildClipboardText("1", "2", "18", std::string(64, '0'), goldenBody)) == ClipboardStatus::ChecksumMismatch, "wrong digest");
        check.Expect(Status(BuildClipboardText("1", "3", "18", digest, goldenBody)) == ClipboardStatus::ChecksumMismatch,
            "the entity count is covered by the digest");
        std::string upper = digest;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
        check.Expect(Status(BuildClipboardText("1", "2", "18", upper, goldenBody)) == ClipboardStatus::Malformed, "uppercase digest");
        check.Expect(Status(BuildClipboardText("1", "2", "18", digest.substr(1), goldenBody)) == ClipboardStatus::Malformed, "short digest");
        check.Expect(Status(BuildClipboardText("1", "2", "18", digest, goldenBody) + " ") == ClipboardStatus::Malformed, "trailing byte");
        check.Expect(Status("SpiralEntityClipboard  1 2 18 " + digest + "\n" + goldenBody) == ClipboardStatus::Malformed, "doubled separator");
        check.Expect(Status("SpiralEntityClipboard 1 2 18 " + digest + " \n" + goldenBody) == ClipboardStatus::Malformed, "trailing separator");
        check.Expect(Status("SpiralEntityClipboard 1 2 " + digest + "\n" + goldenBody) == ClipboardStatus::Malformed, "missing field");
        check.Expect(Status("SpiralEntityClipboard 1 2 18 " + digest + " x\n" + goldenBody) == ClipboardStatus::Malformed, "extra field");
        check.Expect(Status(std::string("SpiralEntityClipboard 1 2 18 ") + std::string(400, 'a')) == ClipboardStatus::Malformed, "oversized header without a newline");

        std::string embeddedNul = "ab";
        embeddedNul.push_back('\0');
        embeddedNul += "c";
        encoded = golden;
        check.Expect(EncodeEntityClipboard({ 1, embeddedNul }, encoded) == ClipboardStatus::Malformed, "encoding refuses NUL");
        check.Expect(Status(BuildClipboardText("1", "1", "4", Engine::Sha256Builder::HashString(embeddedNul), embeddedNul)) == ClipboardStatus::Malformed,
            "decoding refuses NUL even with a matching digest");
        check.Expect(EncodeEntityClipboard({ 0, "x" }, encoded) == ClipboardStatus::Malformed, "encoding refuses zero entities");
        check.Expect(EncodeEntityClipboard({ 1, "" }, encoded) == ClipboardStatus::Malformed, "encoding refuses an empty body");
        check.Expect(EncodeEntityClipboard({ kMaxClipboardEntities + 1, "x" }, encoded) == ClipboardStatus::TooLarge, "encoding refuses too many entities");
        check.Expect(encoded == golden, "failed encodes leave the output untouched");

        // Boundary: exactly the maximum body is accepted, one more byte is not.
        const std::string maximum(kMaxClipboardBodyBytes, 'm');
        check.Expect(EncodeEntityClipboard({ 1, maximum }, encoded) == ClipboardStatus::Ok, "the maximum body encodes");
        check.Expect(DecodeEntityClipboard(encoded, decoded) == ClipboardStatus::Ok && decoded.Body == maximum, "the maximum body decodes");
        check.Expect(EncodeEntityClipboard({ 1, maximum + "m" }, encoded) == ClipboardStatus::TooLarge, "one byte over the maximum is refused");
        check.Expect(EncodeEntityClipboard({ kMaxClipboardEntities, "x" }, encoded) == ClipboardStatus::Ok, "the maximum entity count encodes");

        // Exhaustive single-byte mutation and every truncation of the golden text.
        for (size_t position = 0; position < golden.size(); ++position)
        {
            for (const unsigned char replacement : { 0x00, 0x20, 0x30, 0x0A, 0x7F, 0xFF, 0x41 })
            {
                if (static_cast<unsigned char>(golden[position]) == replacement)
                    continue;
                std::string mutated = golden;
                mutated[position] = static_cast<char>(replacement);
                check.Expect(Status(mutated) != ClipboardStatus::Ok, "mutation at " + std::to_string(position) + " is rejected");
            }
            check.Expect(Status(golden.substr(0, position)) != ClipboardStatus::Ok, "truncation to " + std::to_string(position) + " is rejected");
        }
        return check.Ok;
    }

    // ===== layout codec ============================================================

    const std::vector<std::string_view>& PanelCatalog()
    {
        static const std::vector<std::string_view> ids = { "scene.hierarchy", "inspector", "viewport", "content.browser", "console",
            "profiler", "history", "fab.browser", "fab.import", "shortcuts", "status-bar", "panel_x" };
        return ids;
    }

    // Independent oracles: std::regex for the grammars, a naive loop for the ini alphabet.
    bool ReferenceWorkspaceName(const std::string& name)
    {
        static const std::regex shape(R"(^[A-Za-z0-9]([A-Za-z0-9_.() -]*[A-Za-z0-9)])?$)");
        static const std::regex device(R"(^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\..*)?$)", std::regex::icase);
        return name.size() <= 48 && std::regex_match(name, shape) && name.find("  ") == std::string::npos
            && !std::regex_match(name, device);
    }

    bool ReferencePanelId(const std::string& id)
    {
        static const std::regex shape(R"(^[a-z][a-z0-9._-]{0,47}$)");
        return std::regex_match(id, shape);
    }

    bool ReferenceToken(const std::string& value, size_t maximum)
    {
        static const std::regex shape(R"(^[A-Za-z0-9_.:#+-]*$)");
        return value.size() <= maximum && std::regex_match(value, shape);
    }

    std::string GenerateWorkspaceName(ChoiceStream& stream)
    {
        static const std::vector<std::string> words = { "Default", "Modeling", "Lighting", "Diagnostics", "Review", "Fab", "Layout-2", "v1.2" };
        std::string name = Pick(stream, words);
        if (stream.NextBool())
            name += " " + Pick(stream, words);
        if (stream.NextSize(0, 3) == 0)
            name += " (" + std::to_string(stream.NextSize(1, 99)) + ")";
        return name;
    }

    std::string GenerateIni(ChoiceStream& stream)
    {
        std::string ini;
        const size_t lines = stream.NextSize(0, 60);
        for (size_t index = 0; index < lines; ++index)
        {
            ini += "[Window][Panel " + std::to_string(stream.NextSize(0, 99)) + "]\n";
            ini += "Pos=" + std::to_string(stream.NextSize(0, 3000)) + "," + std::to_string(stream.NextSize(0, 2000)) + "\n";
            if (stream.NextSize(0, 5) == 0)
                ini += "\tDockId=0x" + std::to_string(stream.NextSize(1, 9999999)) + ",0\n";
            if (stream.NextSize(0, 9) == 0)
                ini += "caf\xC3\xA9 \xF0\x9F\x98\x80\n";
        }
        return ini;
    }

    Workspace GenerateWorkspace(ChoiceStream& stream, std::string name)
    {
        Workspace workspace;
        workspace.Name = std::move(name);
        workspace.RestoreDetached = stream.NextBool();
        std::vector<std::string_view> catalog = PanelCatalog();
        for (size_t index = catalog.size(); index > 1; --index)
            std::swap(catalog[index - 1], catalog[stream.NextSize(0, index - 1)]);
        const size_t panelCount = stream.NextSize(0, catalog.size());
        static const std::vector<std::string> docks = { "", "", "0x00000005", "node:3", "a#b+c.d_e-f" };
        for (size_t index = 0; index < panelCount; ++index)
            workspace.Panels.push_back({ std::string(catalog[index]), stream.NextBool(), Pick(stream, docks) });
        for (size_t index = 0; index < workspace.Panels.size(); ++index)
        {
            if (stream.NextSize(0, 3) != 0)
                continue;
            DetachedViewportRecord record;
            record.PanelId = workspace.Panels[index].Id;
            record.X = static_cast<std::int32_t>(stream.NextI64(-kMaxDetachedCoordinate, kMaxDetachedCoordinate,
                { -kMaxDetachedCoordinate, 0, kMaxDetachedCoordinate }));
            record.Y = static_cast<std::int32_t>(stream.NextI64(-kMaxDetachedCoordinate, kMaxDetachedCoordinate,
                { -kMaxDetachedCoordinate, 0, kMaxDetachedCoordinate }));
            record.Width = static_cast<u32>(stream.NextI64(1, kMaxDetachedExtent, { 1, kMaxDetachedExtent }));
            record.Height = static_cast<u32>(stream.NextI64(1, kMaxDetachedExtent, { 1, kMaxDetachedExtent }));
            static const std::vector<std::string> monitors = { "", "DP-3", "HDMI-A-1", "eDP-1" };
            record.Monitor = Pick(stream, monitors);
            workspace.Detached.push_back(std::move(record));
        }
        workspace.ImGuiIni = GenerateIni(stream);
        return workspace;
    }

    WorkspaceStore GenerateStore(ChoiceStream& stream)
    {
        WorkspaceStore store;
        const size_t count = stream.NextSize(1, 5);
        while (store.Workspaces.size() < count)
        {
            std::string name = GenerateWorkspaceName(stream);
            const bool duplicate = std::any_of(store.Workspaces.begin(), store.Workspaces.end(), [&](const Workspace& other)
            {
                return LowerAscii(other.Name) == LowerAscii(name);
            });
            if (!duplicate)
                store.Workspaces.push_back(GenerateWorkspace(stream, std::move(name)));
        }
        store.Active = store.Workspaces[stream.NextSize(0, store.Workspaces.size() - 1)].Name;
        return store;
    }

    std::string Resign(const std::string& body)
    {
        return body + "checksum " + Engine::Sha256Builder::HashString(body) + "\n";
    }

    std::string BodyOf(const std::string& signedText)
    {
        return signedText.substr(0, signedText.size() - (9 + 64 + 1));
    }

    const std::string& GoldenLayoutText()
    {
        // Hand-written; the trailing digest was produced by Python hashlib.
        static const std::string text =
            "SpiralLayout 1\n"
            "active \"Lighting (HDR)\"\n"
            "workspaces 2\n"
            "workspace \"Default\" restore_detached=0 panels=2 detached=0 ini=0\n"
            "panel scene.hierarchy visible=1 dock=~\n"
            "panel inspector visible=0 dock=0x00000005\n"
            "\n"
            "workspace \"Lighting (HDR)\" restore_detached=1 panels=2 detached=1 ini=24\n"
            "panel scene.hierarchy visible=1 dock=~\n"
            "panel viewport visible=1 dock=node:2\n"
            "detached scene.hierarchy x=-20 y=40 w=640 h=480 monitor=DP-3\n"
            "[Window][Debug]\n"
            "Pos=1,2\n"
            "\n"
            "checksum e9e1bd54e43d92332286c7e6c32267f359e1cf6a204a87fa7022073b3b79c784\n";
        return text;
    }

    WorkspaceStore GoldenLayoutStore()
    {
        WorkspaceStore store;
        store.Active = "Lighting (HDR)";
        Workspace first;
        first.Name = "Default";
        first.Panels = { { "scene.hierarchy", true, "" }, { "inspector", false, "0x00000005" } };
        Workspace second;
        second.Name = "Lighting (HDR)";
        second.RestoreDetached = true;
        second.Panels = { { "scene.hierarchy", true, "" }, { "viewport", true, "node:2" } };
        second.Detached = { { "scene.hierarchy", -20, 40, 640, 480, "DP-3" } };
        second.ImGuiIni = "[Window][Debug]\nPos=1,2\n";
        store.Workspaces = { first, second };
        return store;
    }

    WorkspaceStore SentinelStore()
    {
        WorkspaceStore store;
        store.Active = "Sentinel";
        store.Workspaces.push_back({ "Sentinel", true, { { "console", false, "keep" } }, {}, "[keep]\n" });
        return store;
    }

    bool LayoutRoundTripPropertyOnce(ChoiceStream& stream, std::string& message)
    {
        const WorkspaceStore store = GenerateStore(stream);
        std::string text;
        std::string error;
        if (!SerializeWorkspaceStore(store, text, error))
        {
            message = "a generated valid store did not serialize: " + error;
            return false;
        }
        WorkspaceStore parsed = SentinelStore();
        if (!ParseWorkspaceStore(text, parsed, error) || !(parsed == store))
        {
            message = "round trip did not reproduce the store: " + error;
            return false;
        }
        std::string again;
        if (!SerializeWorkspaceStore(parsed, again, error) || again != text)
        {
            message = "serialization is not canonical";
            return false;
        }

        // Structure-aware mutation of the signed body: either rejected with the output untouched,
        // or accepted as a valid store whose canonical serialization is exactly the mutated text.
        for (int round = 0; round < 6; ++round)
        {
            std::string body = BodyOf(text);
            std::vector<size_t> lineStarts = { 0 };
            for (size_t index = 0; index < body.size(); ++index)
            {
                if (body[index] == '\n')
                    lineStarts.push_back(index + 1);
            }
            const size_t operation = stream.NextSize(0, 7);
            const size_t position = stream.NextSize(0, body.size() - 1);
            static const std::string alphabet = " \n0123456789=~\"abcXYZ-(.)\t\r\x01";
            switch (operation)
            {
                case 0: body[position] = alphabet[stream.NextSize(0, alphabet.size() - 1)]; break;
                case 1: body.erase(position, 1); break;
                case 2: body.insert(position, 1, alphabet[stream.NextSize(0, alphabet.size() - 1)]); break;
                case 3:
                {
                    const size_t line = stream.NextSize(0, lineStarts.size() - 1);
                    const size_t end = line + 1 < lineStarts.size() ? lineStarts[line + 1] : body.size();
                    body.insert(lineStarts[line], body.substr(lineStarts[line], end - lineStarts[line]));
                    break;
                }
                case 4:
                {
                    const size_t line = stream.NextSize(0, lineStarts.size() - 1);
                    const size_t end = line + 1 < lineStarts.size() ? lineStarts[line + 1] : body.size();
                    body.erase(lineStarts[line], end - lineStarts[line]);
                    break;
                }
                case 5: body.resize(position); break;
                case 6: body.insert(position, body.substr(stream.NextSize(0, body.size() - 1), stream.NextSize(1, 20))); break;
                default: body += alphabet.substr(stream.NextSize(0, alphabet.size() - 1), 1); break;
            }
            const std::string mutated = Resign(body);
            WorkspaceStore out = SentinelStore();
            std::string mutationError;
            if (ParseWorkspaceStore(mutated, out, mutationError))
            {
                std::string canonical;
                if (!SerializeWorkspaceStore(out, canonical, mutationError) || canonical != mutated)
                {
                    message = "an accepted mutation (operation " + std::to_string(operation) + ") is not in canonical form";
                    return false;
                }
            }
            else if (!(out == SentinelStore()) || mutationError.empty())
            {
                message = "a rejected mutation modified the output or gave no reason";
                return false;
            }
        }

        // Without re-signing, any single-byte change or truncation is caught by the digest.
        std::string flipped = text;
        flipped[stream.NextSize(0, flipped.size() - 1)] ^= static_cast<char>(1u << stream.NextSize(0, 6));
        WorkspaceStore out = SentinelStore();
        if (flipped != text && ParseWorkspaceStore(flipped, out, error))
        {
            message = "a flipped bit was accepted";
            return false;
        }
        if (ParseWorkspaceStore(text.substr(0, stream.NextSize(0, text.size() - 1)), out, error) || !(out == SentinelStore()))
        {
            message = "a truncation was accepted or modified the output";
            return false;
        }
        return true;
    }

    bool LayoutNameGrammarPropertyOnce(ChoiceStream& stream, std::string& message)
    {
        static const std::vector<std::string> fragments = { "a", "Z", "0", " ", "_", "-", ".", "(", ")", "/", "\\", ":", "*", "?", "\"",
            "<", ">", "|", "\t", "con", "CON", "Nul", "com1", "lpt9", "COM0", "..", "x.txt", "\xC3\xA9", "ab", "Layout", "  " };
        std::string name;
        const size_t pieces = stream.NextSize(0, 6);
        for (size_t index = 0; index < pieces; ++index)
            name += Pick(stream, fragments);
        if (stream.NextSize(0, 9) == 0)
            name += std::string(stream.NextSize(40, 60), 'q');
        if (IsValidWorkspaceName(name) != ReferenceWorkspaceName(name))
        {
            message = "workspace name \"" + name + "\" disagrees with the regex oracle";
            return false;
        }

        std::string panel;
        const size_t panelPieces = stream.NextSize(0, 4);
        static const std::vector<std::string> panelFragments = { "a", "b9", ".", "_", "-", "A", "9", " ", "/", "panel", "x" };
        for (size_t index = 0; index < panelPieces; ++index)
            panel += Pick(stream, panelFragments);
        if (stream.NextSize(0, 9) == 0)
            panel += std::string(stream.NextSize(44, 52), 'p');
        if (IsValidPanelId(panel) != ReferencePanelId(panel))
        {
            message = "panel id \"" + panel + "\" disagrees with the regex oracle";
            return false;
        }

        static const std::vector<std::string> tokenFragments = { "A", "z", "0", "_", ".", ":", "#", "+", "-", "~", " ", "=", "\"", "x" };
        std::string token;
        const size_t tokenPieces = stream.NextSize(0, 10);
        for (size_t index = 0; index < tokenPieces; ++index)
            token += Pick(stream, tokenFragments);
        if (stream.NextSize(0, 9) == 0)
            token += std::string(stream.NextSize(28, 70), 'm');
        if (IsValidDockRef(token) != ReferenceToken(token, 32) || IsValidMonitorHint(token) != ReferenceToken(token, 64))
        {
            message = "token \"" + token + "\" disagrees with the regex oracle";
            return false;
        }
        const std::string sanitized = SanitizeMonitorHint(token + "\\\\.\\DISPLAY1 \xC3\xA9");
        if (!IsValidMonitorHint(sanitized))
        {
            message = "a sanitized monitor hint is not valid";
            return false;
        }

        const std::string ini = RandomBytes(stream, 40);
        const bool naive = std::none_of(ini.begin(), ini.end(), [](char value)
        {
            const unsigned char byte = static_cast<unsigned char>(value);
            return (byte < 0x20 && byte != '\n' && byte != '\t') || byte == 0x7F;
        });
        if (IsValidImGuiIni(ini) != naive)
        {
            message = "ini alphabet disagrees with the naive oracle";
            return false;
        }
        return true;
    }

    std::string ReplaceOnce(std::string text, std::string_view from, std::string_view to, Checker& check)
    {
        const size_t at = text.find(from);
        check.Expect(at != std::string::npos, "corpus edit target exists: " + std::string(from));
        if (at != std::string::npos)
            text.replace(at, from.size(), to);
        return text;
    }

    bool CheckLayoutGoldenAndLimits()
    {
        Checker check { "layout-golden-and-limits" };
        const std::string& golden = GoldenLayoutText();
        const WorkspaceStore expected = GoldenLayoutStore();

        std::string error;
        WorkspaceStore parsed;
        check.Expect(ParseWorkspaceStore(golden, parsed, error) && parsed == expected, "the golden bytes parse to the expected store: " + error);
        std::string text;
        check.Expect(SerializeWorkspaceStore(expected, text, error) && text == golden, "the expected store serializes to the golden bytes");
        check.Expect(Engine::Sha256Builder::HashString(BodyOf(golden)) == golden.substr(golden.size() - 65, 64),
            "the golden digest matches Engine SHA-256 (digest produced independently)");
        check.Expect(DetachedViewportsToRestore(parsed.Workspaces[0]).empty() && DetachedViewportsToRestore(parsed.Workspaces[1]).size() == 1,
            "restore rule on the golden store");

        // Limits at the boundary are accepted and survive a round trip.
        WorkspaceStore full;
        for (size_t index = 0; index < kMaxWorkspaces; ++index)
        {
            Workspace workspace;
            workspace.Name = "Workspace " + std::to_string(index);
            for (size_t panel = 0; panel < kMaxPanelsPerWorkspace; ++panel)
                workspace.Panels.push_back({ "p" + std::to_string(panel), true, std::string(kMaxDockRefBytes, 'd') });
            for (size_t panel = 0; panel < kMaxDetachedPerWorkspace; ++panel)
                workspace.Detached.push_back({ "p" + std::to_string(panel), -kMaxDetachedCoordinate, kMaxDetachedCoordinate,
                    kMaxDetachedExtent, 1, std::string(kMaxMonitorHintBytes, 'm') });
            workspace.ImGuiIni.assign(kMaxImGuiIniBytes, 'i');
            workspace.ImGuiIni.back() = '\n';
            full.Workspaces.push_back(std::move(workspace));
        }
        full.Active = full.Workspaces.back().Name;
        check.Expect(SerializeWorkspaceStore(full, text, error) && text.size() <= kMaxLayoutFileBytes, "a store at every limit serializes within the file cap: " + error);
        parsed = SentinelStore();
        check.Expect(ParseWorkspaceStore(text, parsed, error) && parsed == full, "a store at every limit round trips");

        const auto Rejects = [&](WorkspaceStore store, const char* what)
        {
            std::string kept = "kept";
            const bool serialized = SerializeWorkspaceStore(store, kept, error);
            check.Expect(!serialized && kept == "kept" && !error.empty(), std::string(what) + " is refused and leaves the output untouched");
            check.Expect(!ValidateWorkspaceStore(store, error), std::string(what) + " fails validation");
        };
        WorkspaceStore mutated = full;
        mutated.Workspaces.push_back({ "Extra", false, {}, {}, "" });
        Rejects(mutated, "17 workspaces");
        mutated = full;
        mutated.Workspaces[0].Panels.push_back({ "one-too-many", true, "" });
        Rejects(mutated, "65 panels");
        mutated = full;
        mutated.Workspaces[0].Detached.push_back({ "p20", 0, 0, 10, 10, "" });
        Rejects(mutated, "17 detached windows");
        mutated = full;
        mutated.Workspaces[0].ImGuiIni.push_back('i');
        Rejects(mutated, "a dock layout over the limit");
        mutated = full;
        mutated.Workspaces[0].Name = std::string(49, 'n');
        Rejects(mutated, "a 49-byte workspace name");
        mutated = full;
        mutated.Workspaces[0].Panels[0].DockRef.push_back('d');
        Rejects(mutated, "a 33-byte dock reference");
        mutated = full;
        mutated.Workspaces[0].Detached[0].Monitor.push_back('m');
        Rejects(mutated, "a 65-byte monitor hint");
        mutated = full;
        mutated.Workspaces[0].Panels[0].Id = std::string(49, 'p');
        Rejects(mutated, "a 49-byte panel id");
        mutated = full;
        mutated.Workspaces[0].Detached[0].Width = kMaxDetachedExtent + 1;
        Rejects(mutated, "a detached window wider than the limit");
        mutated = full;
        mutated.Active = "Missing";
        Rejects(mutated, "an unknown active workspace");
        Rejects(WorkspaceStore {}, "an empty store");
        mutated = full;
        mutated.Workspaces[1].Name = "WORKSPACE 0";
        Rejects(mutated, "workspace names differing only in case");
        return check.Ok;
    }

    bool CheckLayoutHostileInputs()
    {
        Checker check { "layout-hostile" };
        const std::string& golden = GoldenLayoutText();
        const std::string body = BodyOf(golden);

        const WorkspaceStore sentinel = SentinelStore();
        const auto Rejected = [&](const std::string& text, const std::string& what)
        {
            WorkspaceStore out = sentinel;
            std::string error;
            const bool accepted = ParseWorkspaceStore(text, out, error);
            check.Expect(!accepted, what + " is rejected");
            check.Expect(out == sentinel, what + " leaves the output untouched");
            check.Expect(accepted || !error.empty(), what + " reports a reason");
        };
        const auto RejectedBody = [&](const std::string& mutatedBody, const std::string& what) { Rejected(Resign(mutatedBody), what); };

        // Version table.
        for (const char* header : { "SpiralLayout 0", "SpiralLayout 2", "SpiralLayout 999", "SpiralLayout 01", "SpiralLayout +1",
                 "SpiralLayout 1 ", "SpiralLayout  1", "spirallayout 1", "SpiralLayout", "SpiralLayout x", "SpiralLayout 4294967297" })
            RejectedBody(ReplaceOnce(body, "SpiralLayout 1", header, check), std::string("header ") + header);

        // Top-level lines.
        RejectedBody(ReplaceOnce(body, "active \"Lighting (HDR)\"", "active \"Nope\"", check), "an unknown active workspace");
        RejectedBody(ReplaceOnce(body, "active \"Lighting (HDR)\"", "active \"Lighting (HDR)\" ", check), "a trailing space after active");
        RejectedBody(ReplaceOnce(body, "active \"Lighting (HDR)\"", "active Lighting (HDR)", check), "an unquoted active name");
        RejectedBody(ReplaceOnce(body, "active \"Lighting (HDR)\"", "active \"lighting (hdr)\"", check), "an active name that differs in case");
        for (const char* count : { "3", "1", "0", "17", "02", "+2", "-1", "2 ", " 2", "99999999999" })
            RejectedBody(ReplaceOnce(body, "workspaces 2", std::string("workspaces ") + count, check), std::string("workspace count ") + count);

        // Workspace names.
        for (const std::string& name : { std::string("lighting (hdr)"), std::string("../x"), std::string("a/b"), std::string("a\\b"), std::string("CON"),
                 std::string("con.txt"), std::string("Default "), std::string(""), std::string(49, 'n'), std::string("-bad"),
                 std::string("bad."), std::string("two  spaces"), std::string("tab\t"), std::string("Caf\xC3\xA9"), std::string("a:b"),
                 std::string(".hidden"), std::string("a*b") })
            RejectedBody(ReplaceOnce(body, "workspace \"Default\"", "workspace \"" + name + "\"", check), "workspace name \"" + name + "\"");
        RejectedBody(ReplaceOnce(body, "workspace \"Default\"", "workspace \"a\"b\"", check), "an embedded quote");
        RejectedBody(ReplaceOnce(body, "workspace \"Lighting (HDR)\" ", "workspace \"Lighting (HDR)\"", check), "no separator after the name");
        RejectedBody(ReplaceOnce(body, "workspace \"Lighting (HDR)\"", "workspace \"Lighting (HDR)", check), "an unterminated name");

        // Workspace line fields.
        const std::string line = "workspace \"Default\" restore_detached=0 panels=2 detached=0 ini=0";
        for (const char* edit : {
                 "workspace \"Default\" panels=2 restore_detached=0 detached=0 ini=0",
                 "workspace \"Default\" restore_detached=0 restore_detached=0 detached=0 ini=0",
                 "workspace \"Default\" restore_detached=0 panels=2 detached=0 ini=0 extra=1",
                 "workspace \"Default\" restore_detached=0 panels=2 detached=0",
                 "workspace \"Default\" restore_detached=2 panels=2 detached=0 ini=0",
                 "workspace \"Default\" restore_detached=true panels=2 detached=0 ini=0",
                 "workspace \"Default\" restore_detached=0 panels=02 detached=0 ini=0",
                 "workspace \"Default\" restore_detached=0 panels=65 detached=0 ini=0",
                 "workspace \"Default\" restore_detached=0 panels=2 detached=17 ini=0",
                 "workspace \"Default\" restore_detached=0 panels=2 detached=0 ini=196609",
                 "workspace \"Default\" restore_detached=0 panels=2 detached=0 ini=4294967296",
                 "workspace \"Default\" restore_detached=0 panels=-2 detached=0 ini=0",
                 "workspace \"Default\"  restore_detached=0 panels=2 detached=0 ini=0",
                 "workspace \"Default\" restore_detached=0 panels=2 detached=0 ini=0 ",
                 "workspace \"Default\" restore_detached= panels=2 detached=0 ini=0",
                 "workspace \"Default\" Restore_detached=0 panels=2 detached=0 ini=0",
                 "workspace \"Default\"restore_detached=0 panels=2 detached=0 ini=0",
                 "workspace \"Default\" restore_detached=0 panels=3 detached=0 ini=0",
                 "workspace \"Default\" restore_detached=0 panels=1 detached=0 ini=0",
                 "workspace \"Default\" restore_detached=0 panels=2 detached=1 ini=0",
                 "workspace \"Default\" restore_detached=0 panels=2 detached=0 ini=1",
             })
            RejectedBody(ReplaceOnce(body, line, edit, check), std::string("workspace line: ") + edit);

        // Panel lines.
        const std::string panel = "panel inspector visible=0 dock=0x00000005";
        for (const char* edit : { "panel scene.hierarchy visible=0 dock=0x00000005", "panel Inspector visible=0 dock=0x00000005",
                 "panel 1inspector visible=0 dock=0x00000005", "panel inspector visible=2 dock=0x00000005",
                 "panel inspector visible=0 dock=a~b", "panel inspector visible=0 dock=", "panel inspector visible=0",
                 "panel inspector visible=0 dock=0x00000005 extra", "panel inspector  visible=0 dock=0x00000005",
                 "panel inspector dock=0x00000005 visible=0", "panel inspector visible=0 dock=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                 "panel inspector visible=0 dock=a b", "pane inspector visible=0 dock=0x00000005", "panel inspector visible=0 dock=0x00000005\r",
                 "panel inspector visible=00 dock=0x00000005", "panel inspector visible= dock=0x00000005" })
            RejectedBody(ReplaceOnce(body, panel, edit, check), std::string("panel line: ") + edit);
        RejectedBody(ReplaceOnce(body, "panel viewport visible=1 dock=node:2", "panel scene.hierarchy visible=1 dock=node:2", check), "a duplicate panel id in one workspace");

        // Detached lines.
        const std::string detached = "detached scene.hierarchy x=-20 y=40 w=640 h=480 monitor=DP-3";
        for (const char* edit : { "detached nothere x=-20 y=40 w=640 h=480 monitor=DP-3", "detached scene.hierarchy x=-20 y=40 w=0 h=480 monitor=DP-3",
                 "detached scene.hierarchy x=-20 y=40 w=640 h=0 monitor=DP-3", "detached scene.hierarchy x=-20 y=40 w=32769 h=480 monitor=DP-3",
                 "detached scene.hierarchy x=1000001 y=40 w=640 h=480 monitor=DP-3", "detached scene.hierarchy x=-1000001 y=40 w=640 h=480 monitor=DP-3",
                 "detached scene.hierarchy x=-0 y=40 w=640 h=480 monitor=DP-3", "detached scene.hierarchy x=01 y=40 w=640 h=480 monitor=DP-3",
                 "detached scene.hierarchy x=+1 y=40 w=640 h=480 monitor=DP-3", "detached scene.hierarchy x=99999999999 y=40 w=640 h=480 monitor=DP-3",
                 "detached scene.hierarchy x=-20 y=40 w=-640 h=480 monitor=DP-3", "detached scene.hierarchy x=-20 y=40 w=640 h=480 monitor=",
                 "detached scene.hierarchy x=-20 y=40 w=640 h=480", "detached scene.hierarchy y=40 x=-20 w=640 h=480 monitor=DP-3",
                 "detached scene.hierarchy x=-20 y=40 w=640 h=480 monitor=DP 3", "detached scene.hierarchy x=-20 y=40 w=640 h=480 monitor=a~",
                 "detached scene.hierarchy x=-20 y=40 w=640 h=480 monitor=DP-3 w=1", "detach scene.hierarchy x=-20 y=40 w=640 h=480 monitor=DP-3",
                 "detached scene.hierarchy x=-20 y=40 w=6.5 h=480 monitor=DP-3" })
            RejectedBody(ReplaceOnce(body, detached, edit, check), std::string("detached line: ") + edit);
        RejectedBody(ReplaceOnce(ReplaceOnce(body, " detached=1 ", " detached=2 ", check), detached, detached + "\n" + detached, check),
            "two detached windows for one panel");

        // Dock layout block.
        RejectedBody(ReplaceOnce(body, "ini=24", "ini=25", check), "an ini length one too long");
        RejectedBody(ReplaceOnce(body, "ini=24", "ini=23", check), "an ini length one too short");
        RejectedBody(ReplaceOnce(body, "Pos=1,2", "Pos=1\x01" "2", check), "a control character in the dock layout");
        RejectedBody(ReplaceOnce(body, "Pos=1,2", std::string("Pos=1") + '\0' + "2", check), "a NUL in the dock layout");
        RejectedBody(ReplaceOnce(body, "Pos=1,2", "Pos=1\r2", check), "a carriage return in the dock layout");
        RejectedBody(ReplaceOnce(body, "Pos=1,2", "Pos=1\x7F" "2", check), "DEL in the dock layout");
        RejectedBody(body.substr(0, body.size() - 1), "a missing final newline before the digest");
        RejectedBody(body + "extra\n", "data after the last workspace");
        RejectedBody(body + "\n", "an extra blank line after the last workspace");
        RejectedBody(body.substr(0, body.rfind("detached ")), "a body cut before the detached line");
        std::string crlf;
        for (const char value : body)
        {
            if (value == '\n')
                crlf += '\r';
            crlf += value;
        }
        RejectedBody(crlf, "CRLF line endings");
        RejectedBody("\xEF\xBB\xBF" + body, "a byte order mark");

        // Digest and framing, not re-signed.
        std::string wrongDigest = golden;
        wrongDigest[wrongDigest.size() - 3] = wrongDigest[wrongDigest.size() - 3] == '0' ? '1' : '0';
        Rejected(wrongDigest, "a wrong digest");
        std::string upperDigest = golden;
        std::transform(upperDigest.end() - 65, upperDigest.end() - 1, upperDigest.end() - 65, [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
        Rejected(upperDigest, "an uppercase digest");
        Rejected(body, "a missing digest");
        Rejected(golden + golden.substr(golden.size() - 74), "a repeated digest line");
        Rejected(golden.substr(0, golden.size() - 1), "a digest line without its newline");
        Rejected(golden + "x", "a byte after the digest");
        Rejected("", "empty input");
        Rejected("\n", "a single newline");
        Rejected("SpiralLayout 1\n", "a header alone");
        Rejected(std::string(100, 'a'), "foreign text");
        Rejected(std::string(1 << 20, 'a'), "one megabyte of filler");
        Rejected("SpiralLayout 1\n" + std::string(kMaxLayoutFileBytes, 'a'), "input over the file size cap");
        Rejected(std::string(kMaxLayoutFileBytes + 1, '\0'), "a NUL-filled input over the cap");
        std::string deepName = "workspace \"" + std::string(5000, 'n') + "\"";
        RejectedBody(ReplaceOnce(body, "workspace \"Default\"", deepName, check), "a name longer than a line");

        // Every prefix of the golden file is rejected.
        for (size_t length = 0; length < golden.size(); ++length)
        {
            WorkspaceStore out = sentinel;
            std::string error;
            if (ParseWorkspaceStore(std::string_view(golden).substr(0, length), out, error) || !(out == sentinel))
            {
                check.Expect(false, "the prefix of length " + std::to_string(length) + " was accepted or modified the output");
                break;
            }
        }

        // Re-signed prefixes at line boundaries exercise the grammar rather than the digest.
        for (size_t length = 0; length < body.size(); ++length)
        {
            if (length == 0 || body[length - 1] == '\n')
            {
                WorkspaceStore out = sentinel;
                std::string error;
                if (ParseWorkspaceStore(Resign(body.substr(0, length)), out, error) || !(out == sentinel))
                {
                    check.Expect(false, "the re-signed prefix of length " + std::to_string(length) + " was accepted or modified the output");
                    break;
                }
            }
        }

        // Many workspace headers claimed, few present.
        std::string many = "SpiralLayout 1\nactive \"Default\"\nworkspaces 16\n";
        for (int index = 0; index < 3; ++index)
            many += "workspace \"W" + std::to_string(index) + "\" restore_detached=0 panels=0 detached=0 ini=0\n\n";
        RejectedBody(many, "fewer workspaces than declared");
        return check.Ok;
    }

    // ===== layout operations and migration =========================================

    bool LayoutOperationsPropertyOnce(ChoiceStream& stream, std::string& message)
    {
        WorkspaceStore store = GenerateStore(stream);
        std::string error;
        for (int step = 0; step < 30; ++step)
        {
            const WorkspaceStore before = store;
            const size_t operation = stream.NextSize(0, 5);
            switch (operation)
            {
                case 0:
                {
                    Workspace workspace = GenerateWorkspace(stream, GenerateWorkspaceName(stream));
                    const bool exists = FindWorkspace(store, workspace.Name) != nullptr;
                    const bool full = store.Workspaces.size() >= kMaxWorkspaces;
                    const bool upserted = UpsertWorkspace(store, workspace, error);
                    if (upserted != (exists || !full))
                    {
                        message = "UpsertWorkspace accepted or refused a valid workspace wrongly";
                        return false;
                    }
                    if (upserted && FindWorkspace(store, workspace.Name) == nullptr)
                    {
                        message = "an upserted workspace cannot be found";
                        return false;
                    }
                    break;
                }
                case 1:
                {
                    const std::string name = stream.NextBool() ? Pick(stream, store.Workspaces).Name : GenerateWorkspaceName(stream);
                    const bool known = FindWorkspace(store, name) != nullptr;
                    const bool isDefault = LowerAscii(name) == "default";
                    const bool removed = RemoveWorkspace(store, name);
                    if (removed != (known && !isDefault && before.Workspaces.size() > 1))
                    {
                        message = "RemoveWorkspace policy violated for \"" + name + "\"";
                        return false;
                    }
                    break;
                }
                case 2:
                {
                    const std::string name = stream.NextBool() ? Pick(stream, store.Workspaces).Name : GenerateWorkspaceName(stream);
                    const bool known = FindWorkspace(store, name) != nullptr;
                    if (SetActiveWorkspace(store, name) != known)
                    {
                        message = "SetActiveWorkspace accepted an unknown name or refused a known one";
                        return false;
                    }
                    break;
                }
                case 3:
                {
                    std::vector<std::string_view> ids;
                    for (const std::string_view id : PanelCatalog())
                    {
                        if (stream.NextSize(0, 3) != 0)
                            ids.push_back(id);
                    }
                    ids.push_back("not a panel");
                    Workspace& workspace = store.Workspaces[stream.NextSize(0, store.Workspaces.size() - 1)];
                    ReconcilePanels(workspace, ids);
                    for (const PanelRecord& panel : workspace.Panels)
                    {
                        if (std::find(ids.begin(), ids.end(), std::string_view(panel.Id)) == ids.end())
                        {
                            message = "Reconcile kept a panel the Editor no longer has";
                            return false;
                        }
                    }
                    for (const std::string_view id : ids)
                    {
                        if (IsValidPanelId(id) && FindPanel(workspace, id) == nullptr)
                        {
                            message = "Reconcile did not add a missing panel";
                            return false;
                        }
                    }
                    if (ReconcilePanels(workspace, ids))
                    {
                        message = "Reconcile is not idempotent";
                        return false;
                    }
                    break;
                }
                case 4:
                {
                    Workspace& workspace = store.Workspaces[stream.NextSize(0, store.Workspaces.size() - 1)];
                    ResetWorkspaceToDefault(workspace, PanelCatalog());
                    if (workspace.RestoreDetached || !workspace.Detached.empty() || !workspace.ImGuiIni.empty()
                        || workspace.Panels.size() != PanelCatalog().size()
                        || !std::all_of(workspace.Panels.begin(), workspace.Panels.end(), [](const PanelRecord& panel) { return panel.Visible && panel.DockRef.empty(); }))
                    {
                        message = "reset did not return the default content";
                        return false;
                    }
                    break;
                }
                default:
                {
                    Workspace& workspace = store.Workspaces[stream.NextSize(0, store.Workspaces.size() - 1)];
                    workspace.RestoreDetached = stream.NextBool();
                    const std::span<const DetachedViewportRecord> restored = DetachedViewportsToRestore(workspace);
                    if (workspace.RestoreDetached ? restored.size() != workspace.Detached.size() : !restored.empty())
                    {
                        message = "the restore rule is not 'only when the workspace opted in'";
                        return false;
                    }
                    break;
                }
            }
            if (!ValidateWorkspaceStore(store, error))
            {
                message = "operation " + std::to_string(operation) + " left an invalid store: " + error;
                return false;
            }
            std::string text;
            WorkspaceStore parsed;
            if (!SerializeWorkspaceStore(store, text, error) || !ParseWorkspaceStore(text, parsed, error) || !(parsed == store))
            {
                message = "operation " + std::to_string(operation) + " left a store that does not round trip";
                return false;
            }
        }
        return true;
    }

    bool CheckLayoutOperationsAndMigration()
    {
        Checker check { "layout-operations-and-migration" };
        std::string error;

        // Default content: invalid, repeated, and surplus ids are skipped; the active default workspace is "Default".
        const std::vector<std::string_view> ids = { "viewport", "Bad Id", "viewport", "inspector", "" };
        const Workspace defaults = MakeDefaultWorkspace(ids);
        check.Expect(defaults.Name == "Default" && defaults.Panels.size() == 2 && defaults.Panels[0].Id == "viewport"
                && defaults.Panels[1].Id == "inspector" && !defaults.RestoreDetached && defaults.ImGuiIni.empty(),
            "the default workspace lists valid distinct ids once, visible, with no saved dock layout");
        std::vector<std::string> many;
        for (size_t index = 0; index < kMaxPanelsPerWorkspace + 10; ++index)
            many.push_back("panel" + std::to_string(index));
        std::vector<std::string_view> manyViews(many.begin(), many.end());
        check.Expect(MakeDefaultWorkspace(manyViews).Panels.size() == kMaxPanelsPerWorkspace, "surplus panels are capped");
        const WorkspaceStore defaultStore = MakeDefaultStore(PanelCatalog());
        check.Expect(ValidateWorkspaceStore(defaultStore, error) && defaultStore.Active == "Default" && defaultStore.Workspaces.size() == 1,
            "the default store is valid and active");

        // Reset keeps the name only.
        Workspace custom = GoldenLayoutStore().Workspaces[1];
        ResetWorkspaceToDefault(custom, PanelCatalog());
        check.Expect(custom.Name == "Lighting (HDR)" && custom.ImGuiIni.empty() && custom.Detached.empty() && !custom.RestoreDetached
                && custom.Panels.size() == PanelCatalog().size(),
            "reset keeps the name and replaces everything else");

        // Upsert, remove, activate.
        WorkspaceStore store = GoldenLayoutStore();
        Workspace renamed = store.Workspaces[1];
        renamed.Name = "lighting (hdr)";
        renamed.RestoreDetached = false;
        check.Expect(UpsertWorkspace(store, renamed, error) && store.Workspaces.size() == 2 && store.Workspaces[1].Name == "lighting (hdr)"
                && store.Active == "lighting (hdr)",
            "upsert replaces in place ignoring case and follows the active workspace");
        const WorkspaceStore beforeFailure = store;
        Workspace invalid = renamed;
        invalid.Name = "a/b";
        check.Expect(!UpsertWorkspace(store, invalid, error) && store == beforeFailure && !error.empty(), "an invalid upsert changes nothing");
        invalid = renamed;
        invalid.Detached.push_back({ "nope", 0, 0, 1, 1, "" });
        check.Expect(!UpsertWorkspace(store, invalid, error) && store == beforeFailure, "an upsert with a dangling detached record changes nothing");
        check.Expect(!RemoveWorkspace(store, "Default") && !RemoveWorkspace(store, "default") && !RemoveWorkspace(store, "Nope"),
            "the default and unknown workspaces cannot be removed");
        check.Expect(RemoveWorkspace(store, "LIGHTING (HDR)") && store.Active == "Default" && store.Workspaces.size() == 1,
            "removing the active workspace activates the default one");
        check.Expect(!RemoveWorkspace(store, "Default"), "the last workspace cannot be removed");
        Workspace extra2 = MakeDefaultWorkspace(ids);
        for (size_t index = 0; index < kMaxWorkspaces + 2; ++index)
        {
            extra2.Name = "Extra " + std::to_string(index);
            UpsertWorkspace(store, extra2, error);
        }
        check.Expect(store.Workspaces.size() == kMaxWorkspaces && ValidateWorkspaceStore(store, error), "upserts stop at the workspace cap");
        const WorkspaceStore full = store;
        extra2.Name = "Overflow";
        check.Expect(!UpsertWorkspace(store, extra2, error) && store == full, "a full store refuses a new workspace and is unchanged");
        check.Expect(SetActiveWorkspace(store, "extra 1") && store.Active == "Extra 1" && !SetActiveWorkspace(store, "missing") && store.Active == "Extra 1",
            "activation resolves case and refuses unknown names");

        // Reconcile.
        Workspace workspace = GoldenLayoutStore().Workspaces[1];
        const std::vector<std::string_view> current = { "viewport", "console", "new.panel" };
        check.Expect(ReconcilePanels(workspace, current), "reconcile reports a change");
        check.Expect(workspace.Panels.size() == 3 && workspace.Panels[0].Id == "viewport" && workspace.Panels[0].DockRef == "node:2"
                && workspace.Panels[1].Id == "console" && workspace.Panels[2].Id == "new.panel" && workspace.Detached.empty(),
            "reconcile keeps surviving panels' state, drops unknown panels and their detached windows, appends new panels visible");
        check.Expect(!ReconcilePanels(workspace, current), "reconcile is idempotent");

        // Monitor hints.
        check.Expect(SanitizeMonitorHint("DP-3") == "DP-3" && SanitizeMonitorHint("\\\\.\\DISPLAY1") == "__._DISPLAY1"
                && SanitizeMonitorHint("a b") == "a_b" && SanitizeMonitorHint(std::string(100, 'x')).size() == kMaxMonitorHintBytes,
            "monitor hint sanitizing table");

        // Legacy migration.
        const std::string legacySettings = "SpiralEditorSettings 1\nViewportNavigationPreset Unreal\n";
        const std::string legacyIni = "[Window][Scene Hierarchy]\nPos=0,19\nSize=300,400\nCollapsed=0\nDockId=0x00000001,0\n";
        LayoutMigrationResult migrated = MigrateLegacyEditorState(legacySettings, legacyIni, PanelCatalog());
        check.Expect(migrated.RecognisedSettings && migrated.NavigationPreset == "Unreal" && migrated.AdoptedImGuiIni
                && migrated.Store.Workspaces.size() == 1 && migrated.Store.Workspaces[0].ImGuiIni == legacyIni
                && ValidateWorkspaceStore(migrated.Store, error),
            "both legacy files are adopted into a valid default store");
        migrated = MigrateLegacyEditorState("", "", PanelCatalog());
        check.Expect(!migrated.RecognisedSettings && !migrated.AdoptedImGuiIni && migrated.NavigationPreset.empty()
                && migrated.Store == MakeDefaultStore(PanelCatalog()),
            "no legacy files give exactly the default store");
        for (const char* text : { "SpiralEditorSettings 2\nViewportNavigationPreset Unreal\n", "SpiralEditorSettings 1\nViewportNavigationPreset\n",
                 "SpiralEditorSettings 1\nViewportNavigationPreset Unreal\nextra\n", "Other 1\nViewportNavigationPreset Unreal\n",
                 "SpiralEditorSettings 1\nSomething Unreal\n", "SpiralEditorSettings 1\nViewportNavigationPreset Un-real\n",
                 "SpiralEditorSettings 01\nViewportNavigationPreset Unreal\n" })
        {
            migrated = MigrateLegacyEditorState(text, "", PanelCatalog());
            check.Expect(!migrated.RecognisedSettings && migrated.NavigationPreset.empty(), std::string("settings text is not recognised: ") + text);
        }
        check.Expect(MigrateLegacyEditorState("SpiralEditorSettings\t1\r\nViewportNavigationPreset  Fusion\r\n\r\n", "", PanelCatalog()).NavigationPreset == "Fusion",
            "legacy settings are read as whitespace separated tokens, exactly like the Editor reader");
        migrated = MigrateLegacyEditorState(legacySettings, std::string("[Window]\n") + '\0' + "x", PanelCatalog());
        check.Expect(!migrated.AdoptedImGuiIni && migrated.Store.Workspaces[0].ImGuiIni.empty() && migrated.RecognisedSettings,
            "a damaged dock layout is ignored without blocking the settings");
        migrated = MigrateLegacyEditorState(legacySettings, std::string(kMaxImGuiIniBytes + 1, 'x'), PanelCatalog());
        check.Expect(!migrated.AdoptedImGuiIni, "an oversized dock layout is ignored");
        migrated = MigrateLegacyEditorState(legacySettings, std::string(kMaxImGuiIniBytes, 'x'), PanelCatalog());
        check.Expect(migrated.AdoptedImGuiIni && ValidateWorkspaceStore(migrated.Store, error), "a dock layout at the limit is adopted");
        migrated = MigrateLegacyEditorState(std::string(100000, 'q'), "", PanelCatalog());
        check.Expect(!migrated.RecognisedSettings, "oversized settings text is not recognised");
        return check.Ok;
    }

    // ===== persistence =============================================================

#if defined(__linux__)
    // Real mid-write failure: the kernel refuses to grow a file past the soft limit
    // (EFBIG) after a partial write. SIGXFSZ is ignored for the duration.
    class ScopedFileSizeLimit
    {
    public:
        explicit ScopedFileSizeLimit(rlim_t bytes)
        {
            m_Ready = getrlimit(RLIMIT_FSIZE, &m_Previous) == 0;
            if (!m_Ready)
                return;
            m_PreviousHandler = std::signal(SIGXFSZ, SIG_IGN);
            rlimit limit = m_Previous;
            limit.rlim_cur = bytes;
            m_Applied = setrlimit(RLIMIT_FSIZE, &limit) == 0;
        }

        ~ScopedFileSizeLimit()
        {
            if (m_Ready)
            {
                setrlimit(RLIMIT_FSIZE, &m_Previous);
                std::signal(SIGXFSZ, m_PreviousHandler);
            }
        }

        ScopedFileSizeLimit(const ScopedFileSizeLimit&) = delete;
        ScopedFileSizeLimit& operator=(const ScopedFileSizeLimit&) = delete;

        bool IsApplied() const { return m_Applied; }

    private:
        rlimit m_Previous {};
        void (*m_PreviousHandler)(int) = SIG_DFL;
        bool m_Ready = false;
        bool m_Applied = false;
    };
#endif

    void WriteBytes(const std::filesystem::path& path, std::string_view bytes)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    bool CheckLayoutPersistence()
    {
        Checker check { "layout-persistence" };
        TempDir root("persistence");
        const std::filesystem::path directory = root.Path() / "layouts";
        const std::filesystem::path file = directory / "workspaces.spirallayout";
        std::string error;

        // First run: nothing on disk.
        WorkspaceStore loaded = SentinelStore();
        check.Expect(LoadWorkspaceStore(file, loaded, error) == LayoutLoadStatus::Missing && loaded == SentinelStore(), "a missing file is reported as missing");
        check.Expect(LoadWorkspaceStore(root.Path() / "plain" / "nested.spirallayout", loaded, error) == LayoutLoadStatus::Missing, "a missing directory is reported as missing");
        WriteBytes(root.Path() / "a-file", "x");
        check.Expect(LoadWorkspaceStore(root.Path() / "a-file" / "child.spirallayout", loaded, error) == LayoutLoadStatus::Missing,
            "a path below a regular file is reported as missing");

        // Save creates the directory; load returns exactly what was saved; the file is the canonical text.
        const WorkspaceStore first = GoldenLayoutStore();
        check.Expect(SaveWorkspaceStore(file, first, error), "save creates the layouts directory and the file: " + error);
        check.Expect(ReadAll(file) == GoldenLayoutText(), "the saved file is the canonical text");
        check.Expect(LoadWorkspaceStore(file, loaded, error) == LayoutLoadStatus::Loaded && loaded == first, "load returns the saved store");
        check.Expect(ListNames(directory) == std::vector<std::string> { "workspaces.spirallayout" }, "no temporary file remains after a save");

        WorkspaceStore second = first;
        second.Workspaces[0].ImGuiIni = "[Window][Updated]\n";
        second.Active = "Default";
        check.Expect(SaveWorkspaceStore(file, second, error), "a second save replaces the file");
        check.Expect(LoadWorkspaceStore(file, loaded, error) == LayoutLoadStatus::Loaded && loaded == second, "the replacement loads");

        // Saving an invalid store refuses before touching the file.
        const std::string beforeInvalid = ReadAll(file);
        WorkspaceStore invalid = second;
        invalid.Active = "Nope";
        check.Expect(!SaveWorkspaceStore(file, invalid, error) && ReadAll(file) == beforeInvalid && !error.empty(), "an invalid store is not written");
        check.Expect(!SaveWorkspaceStore(directory / "", second, error), "a destination without a file name is refused");
        check.Expect(ListNames(directory) == std::vector<std::string> { "workspaces.spirallayout" }, "refused saves leave nothing behind");

        // Unusable files on disk are rejected and never modify the output or the file.
        const std::filesystem::path bad = root.Path() / "bad" / "bad.spirallayout";
        const std::string good = GoldenLayoutText();
        const std::vector<std::pair<std::string, std::string>> corpus = {
            { "empty", "" },
            { "garbage", "not a layout" },
            { "truncated", good.substr(0, good.size() / 2) },
            { "one bit flipped", [&] { std::string text = good; text[40] ^= 1; return text; }() },
            { "trailing data", good + "\n" },
            { "oversized", std::string(kMaxLayoutFileBytes + 1, 'z') },
            { "future version", Resign(ReplaceOnce(BodyOf(good), "SpiralLayout 1", "SpiralLayout 2", check)) },
        };
        for (const auto& [name, bytes] : corpus)
        {
            WriteBytes(bad, bytes);
            loaded = SentinelStore();
            check.Expect(LoadWorkspaceStore(bad, loaded, error) == LayoutLoadStatus::Rejected && loaded == SentinelStore() && !error.empty(),
                name + " file is rejected with the output untouched");
            check.Expect(ReadAll(bad) == bytes, name + " file is not modified by a failed load");
        }
        std::filesystem::create_directories(root.Path() / "a-directory");
        check.Expect(LoadWorkspaceStore(root.Path() / "a-directory", loaded, error) == LayoutLoadStatus::Rejected, "a directory is not a layout file");

#if defined(__linux__)
        std::error_code linkError;
        std::filesystem::create_symlink(file, root.Path() / "link.spirallayout", linkError);
        std::filesystem::create_symlink(root.Path() / "nowhere", root.Path() / "dangling.spirallayout", linkError);
        if (!linkError)
        {
            check.Expect(LoadWorkspaceStore(root.Path() / "link.spirallayout", loaded, error) == LayoutLoadStatus::Loaded && loaded == second,
                "a symlink to a layout file loads");
            check.Expect(LoadWorkspaceStore(root.Path() / "dangling.spirallayout", loaded, error) == LayoutLoadStatus::Missing, "a dangling symlink is missing");
        }

        // Failure injection 1: the target name is occupied by a non-empty directory.
        {
            const std::filesystem::path blocked = root.Path() / "blocked";
            const std::filesystem::path target = blocked / "workspaces.spirallayout";
            std::filesystem::create_directories(target);
            WriteBytes(target / "payload", "keep");
            check.Expect(!SaveWorkspaceStore(target, second, error) && !error.empty(), "save fails when the target cannot be replaced");
            check.Expect(ReadAll(target / "payload") == "keep" && ListNames(blocked) == std::vector<std::string> { "workspaces.spirallayout" },
                "the blocking directory is intact and no temporary file remains");
        }

        // Failure injection 2: an unwritable directory keeps the previous file byte for byte.
        {
            const std::filesystem::path locked = root.Path() / "locked";
            const std::filesystem::path target = locked / "workspaces.spirallayout";
            std::filesystem::create_directories(locked);
            check.Expect(SaveWorkspaceStore(target, first, error), "initial save into the soon-to-be-locked directory");
            const std::string before = ReadAll(target);
            std::error_code permissionError;
            std::filesystem::permissions(locked, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                std::filesystem::perm_options::replace, permissionError);
            if (access(locked.c_str(), W_OK) == 0)
            {
                std::cerr << "Selection/layout test note: the unwritable-directory case is skipped because this process can write regardless of permissions\n";
            }
            else
            {
                check.Expect(!SaveWorkspaceStore(target, second, error) && !error.empty(), "save fails in an unwritable directory");
                check.Expect(ReadAll(target) == before, "the previous layout survives an unwritable directory byte for byte");
                loaded = SentinelStore();
                check.Expect(LoadWorkspaceStore(target, loaded, error) == LayoutLoadStatus::Loaded && loaded == first, "and it still loads");
            }
            std::filesystem::permissions(locked, std::filesystem::perms::owner_all, std::filesystem::perm_options::add, permissionError);
            check.Expect(ListNames(locked) == std::vector<std::string> { "workspaces.spirallayout" }, "no temporary file remains beside the previous layout");
        }

        // Failure injection 3: a real partial write (EFBIG) after bytes reached the temporary file.
        {
            const std::filesystem::path partial = root.Path() / "partial";
            const std::filesystem::path target = partial / "workspaces.spirallayout";
            check.Expect(SaveWorkspaceStore(target, first, error), "baseline save before the partial-write injection");
            const std::string before = ReadAll(target);
            WorkspaceStore large = first;
            large.Workspaces[0].ImGuiIni.assign(100000, 'i');
            ScopedFileSizeLimit limit(4096);
            if (!limit.IsApplied())
            {
                std::cerr << "Selection/layout test note: the partial-write case is skipped because RLIMIT_FSIZE could not be applied\n";
            }
            else
            {
                const bool saved = SaveWorkspaceStore(target, large, error);
                check.Expect(!saved && !error.empty(), "save fails when the disk refuses the tail of the file");
                check.Expect(ReadAll(target) == before, "the previous layout survives a partial write byte for byte");
                check.Expect(ListNames(partial) == std::vector<std::string> { "workspaces.spirallayout" }, "the partial temporary file is removed");
            }
        }
#endif
        return check.Ok;
    }
}

namespace SpiralTests
{
    bool TestSelectionModelAgainstReferenceModel()
    {
        return RunProperty("selection-model", SelectionPropertyOnce, 400);
    }

    bool TestSelectionModelScriptedSequences()
    {
        return CheckSelectionScripts();
    }

    bool TestEntityNamingPolicy()
    {
        bool passed = CheckNamingTables();
        passed = RunProperty("entity-naming", NamingPropertyOnce, 500) && passed;
        return passed;
    }

    bool TestHierarchyModelFilterLocksReorderAndRename()
    {
        bool passed = CheckHierarchyTables();
        passed = RunProperty("hierarchy-filter", HierarchyPropertyOnce, 300) && passed;
        passed = RunProperty("hierarchy-reorder", ReorderPropertyOnce, 600) && passed;
        return passed;
    }

    bool TestEntityClipboardRoundTripAndCorruption()
    {
        bool passed = CheckClipboardTables();
        passed = RunProperty("entity-clipboard", ClipboardPropertyOnce, 300) && passed;
        return passed;
    }

    bool TestLayoutCodecRoundTripAndGrammar()
    {
        bool passed = CheckLayoutGoldenAndLimits();
        passed = RunProperty("layout-roundtrip", LayoutRoundTripPropertyOnce, 120) && passed;
        passed = RunProperty("layout-grammar", LayoutNameGrammarPropertyOnce, 800) && passed;
        return passed;
    }

    bool TestLayoutCodecRejectsHostileInputAtomically()
    {
        return CheckLayoutHostileInputs();
    }

    bool TestLayoutStoreOperationsAndLegacyMigration()
    {
        bool passed = CheckLayoutOperationsAndMigration();
        passed = RunProperty("layout-operations", LayoutOperationsPropertyOnce, 150) && passed;
        return passed;
    }

    bool TestLayoutPersistenceIsAtomicAndFailClosed()
    {
        return CheckLayoutPersistence();
    }
}
