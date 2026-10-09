#pragma once

#include "EditGesture.h"
#include "HistoryLabel.h"

#include "Engine/Core/Base.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace EditorHistory
{
    using HistoryRevision = Engine::u64;

    constexpr Engine::u64 kDefaultHistoryBudgetBytes = 256ull * 1024ull * 1024ull;
    constexpr size_t kDefaultHistoryMaximumEntries = 512;

    struct HistoryConfig
    {
        // Estimated bytes of the distinct snapshots the entries reference. The
        // newest entry is always kept, so one entry may exceed it.
        Engine::u64 BudgetBytes = kDefaultHistoryBudgetBytes;
        // Bounds the History panel list.
        size_t MaximumEntries = kDefaultHistoryMaximumEntries;
    };

    enum class HistoryStatus
    {
        // The operation was performed.
        Ok,
        // An entry was recorded; Revision is its revision.
        Recorded,
        // Both states are the same: nothing was recorded (a gesture or
        // transaction that ended unchanged is closed), or a jump to the current
        // position.
        NoChange,
        // The call happened inside an open transaction, which records the one
        // entry when it closes.
        Absorbed,
        NothingToUndo,
        NothingToRedo,
        // Undo is blocked by an undo barrier or by dropped history.
        UndoBarrier,
        GestureOpen,
        TransactionOpen,
        NoGesture,
        NoTransaction,
        // The gesture key does not match the open gesture, which stays open.
        KeyMismatch,
        UnknownRevision,
        // The adapter could not restore; history and live state are unchanged.
        RestoreFailed,
        InvalidArgument
    };

    // Why undo or redo is unavailable right now.
    enum class HistoryBlock
    {
        None,
        NothingToUndo,
        NothingToRedo,
        UndoBarrier,
        GestureOpen,
        TransactionOpen
    };

    struct HistoryAvailability
    {
        bool Enabled = false;
        HistoryBlock Block = HistoryBlock::None;
        // Edit menu text: "Undo Move Cube" or "Undo (blocked: Import Fab Asset)".
        std::string MenuLabel;
        // Status and tooltip text; empty when enabled.
        std::string Detail;
    };

    // The newest entries reference snapshots by pointer; Bytes is the sum of the
    // two snapshots' estimates (shared snapshots are counted once in
    // HistoryStore::UsedBytes, not here).
    struct HistoryEntryInfo
    {
        HistoryRevision Revision = 0;
        HistoryLabel Label;
        Engine::u64 Bytes = 0;
    };

    struct HistoryRow
    {
        HistoryRevision Revision = 0;
        HistoryLabel Label;
        // Label.Display(), plus " (barrier)" for a barrier base row.
        std::string Display;
        Engine::u64 Bytes = 0;
        // The first row: project opened, an undo barrier, or dropped history.
        bool IsBase = false;
        bool IsBarrier = false;
        // The row is applied to the live state (every row up to the current one).
        bool Applied = false;
        // The live state is this row's result.
        bool Current = false;
    };

    struct EvictionStats
    {
        size_t Entries = 0;
        Engine::u64 Bytes = 0;
        size_t Events = 0;
    };

    // What one Record dropped from the old end.
    struct EvictionNotice
    {
        size_t ForBudget = 0;
        size_t ForEntryCap = 0;
        Engine::u64 Bytes = 0;

        size_t Entries() const { return ForBudget + ForEntryCap; }
    };

    struct HistoryResult
    {
        HistoryStatus Status = HistoryStatus::Ok;
        // HeadRevision() after the call.
        HistoryRevision Head = 0;
        // The entry recorded, undone, redone, or jumped to (0 when none).
        HistoryRevision Revision = 0;
        // Entries moved over by undo, redo, or jump.
        size_t Steps = 0;
        // Redo entries a Record or End removed.
        size_t RedoDiscarded = 0;
        EvictionNotice Evicted;
        HistoryLabel Label;
        // One line for the status text and the console.
        std::string Announcement;

        bool Succeeded() const { return Status == HistoryStatus::Ok || Status == HistoryStatus::Recorded; }
    };

    // The only seam to the live project state. EditorLayer's HistoryState
    // (Scene, AssetRegistry, MaterialLibrary, selection, color pipeline) is the
    // State type; the store never looks inside it.
    template <class State>
    class IHistoryStateAdapter
    {
    public:
        using Snapshot = std::shared_ptr<const State>;

        virtual ~IHistoryStateAdapter() = default;

        // Applies the snapshot to live state, including every post-restore
        // invariant (renderer publication, pivot). Returns false and leaves the
        // live state unchanged when it cannot.
        virtual bool Restore(const Snapshot& snapshot) = 0;

        // Estimated heap bytes the snapshot keeps alive. Called once per
        // distinct snapshot while the store references it.
        virtual Engine::u64 EstimateBytes(const State& state) const = 0;

        // Whether two snapshots are the same project state; decides whether a
        // gesture or transaction that ended records an entry. Returning false
        // always records.
        virtual bool Equal(const State& first, const State& second) const = 0;
    };

    // Heap bytes a std::string owns beyond its own object (zero while it fits
    // the small-string buffer), for adapters estimating a snapshot.
    inline Engine::u64 EstimateStringHeapBytes(const std::string& text)
    {
        return text.capacity() > std::string().capacity() ? static_cast<Engine::u64>(text.capacity()) + 1 : 0;
    }

    // Named undo/redo over immutable whole-state snapshots.
    //
    // Model: a base row (project opened, an undo barrier, or dropped history)
    // followed by entries, and a cursor counting the applied entries. Entry i
    // holds the state before and after it. Undo restores the Before of entry
    // cursor-1, Redo the After of entry cursor, and a jump to cursor t restores
    // the Before of entry t when moving back or the After of entry t-1 when
    // moving forward, so one jump is one restore.
    //
    // Every base change and every entry takes the next revision number, which
    // is never reused, so HeadRevision() identifies a position for
    // compare-and-swap even across LoadMark. Not thread-safe; main thread only.
    template <class State>
    class HistoryStore
    {
    private:
        struct Entry
        {
            HistoryEntryInfo Info;
            std::shared_ptr<const State> Before;
            std::shared_ptr<const State> After;
            Engine::u64 BeforeBytes = 0;
            Engine::u64 AfterBytes = 0;
        };

        enum class BaseKind
        {
            Opened,
            Barrier,
            Dropped
        };

        struct Base
        {
            HistoryRevision Revision = 0;
            HistoryLabel Label;
            BaseKind Kind = BaseKind::Opened;
            std::string Reason;
        };

    public:
        using Snapshot = std::shared_ptr<const State>;

        // A pointer copy of the history (not of any state). LoadMark puts the
        // history back exactly; the caller restores live state separately, which
        // is the typed-control rollback path.
        struct Mark
        {
            bool Valid = false;
            std::vector<Entry> Entries;
            size_t Cursor = 0;
            Base BaseRow;
            EvictionStats Evicted;
        };

        HistoryStore(IHistoryStateAdapter<State>& adapter, HistoryConfig config = {})
            : m_Adapter(adapter)
            , m_Config(config)
        {
            if (m_Config.MaximumEntries == 0)
                m_Config.MaximumEntries = 1;
            m_Base = { m_NextRevision++, MakeHistoryLabel("Project opened", "", HistorySource::System), BaseKind::Opened, {} };
        }

        HistoryStore(const HistoryStore&) = delete;
        HistoryStore& operator=(const HistoryStore&) = delete;

        // ---- Recording ----

        // Records one finished edit. The redo tail is discarded. Inside a
        // transaction the call is absorbed; while a gesture is open it is
        // rejected.
        HistoryResult Record(HistoryLabel label, Snapshot before, Snapshot after)
        {
            if (!before || !after)
                return MakeResult(HistoryStatus::InvalidArgument);
            if (m_Gesture)
                return MakeResult(HistoryStatus::GestureOpen);
            if (!m_Transactions.empty())
                return MakeResult(HistoryStatus::Absorbed);
            if (before == after)
                return MakeResult(HistoryStatus::NoChange);
            return Commit(std::move(label), std::move(before), std::move(after));
        }

        // ---- Gestures ----

        // Opens a gesture whose Before was captured before the first edit.
        HistoryResult BeginGesture(const EditGestureKey& key, HistoryLabel label, Snapshot before)
        {
            if (!before)
                return MakeResult(HistoryStatus::InvalidArgument);
            if (m_Gesture)
                return MakeResult(HistoryStatus::GestureOpen);
            if (!m_Transactions.empty())
                return MakeResult(HistoryStatus::TransactionOpen);
            m_Gesture = OpenGesture { key, std::move(label), std::move(before), 0 };
            return MakeResult(HistoryStatus::Ok);
        }

        // Counts one in-place update of the live state.
        HistoryResult UpdateGesture(const EditGestureKey& key)
        {
            if (!m_Gesture)
                return MakeResult(HistoryStatus::NoGesture);
            if (m_Gesture->Key != key)
                return MakeResult(HistoryStatus::KeyMismatch);
            ++m_Gesture->Updates;
            return MakeResult(HistoryStatus::Ok);
        }

        // Closes the gesture and records one entry, or none when After equals
        // Before.
        HistoryResult EndGesture(const EditGestureKey& key, Snapshot after)
        {
            if (!m_Gesture)
                return MakeResult(HistoryStatus::NoGesture);
            if (m_Gesture->Key != key)
                return MakeResult(HistoryStatus::KeyMismatch);
            if (!after)
                return MakeResult(HistoryStatus::InvalidArgument);

            OpenGesture gesture = std::move(*m_Gesture);
            m_Gesture.reset();
            if (StatesEqual(gesture.Before, after))
                return MakeResult(HistoryStatus::NoChange);
            return Commit(std::move(gesture.Label), std::move(gesture.Before), std::move(after));
        }

        // Restores the Before state and closes the gesture without an entry. If
        // the restore fails the gesture stays open.
        HistoryResult CancelGesture()
        {
            if (!m_Gesture)
                return MakeResult(HistoryStatus::NoGesture);
            if (!m_Adapter.Restore(m_Gesture->Before))
                return MakeResult(HistoryStatus::RestoreFailed, "Cancel failed: could not restore the previous state");
            m_Gesture.reset();
            return MakeResult(HistoryStatus::Ok);
        }

        // ---- Transactions ----

        // Opens a (possibly nested) transaction. Only the outermost label is
        // recorded; every level keeps its own Before for AbortTransaction.
        HistoryResult BeginTransaction(HistoryLabel label, Snapshot before)
        {
            if (!before)
                return MakeResult(HistoryStatus::InvalidArgument);
            if (m_Gesture)
                return MakeResult(HistoryStatus::GestureOpen);
            m_Transactions.push_back({ std::move(label), std::move(before) });
            return MakeResult(HistoryStatus::Ok);
        }

        // Closes the innermost transaction. Closing the outermost records one
        // entry from its Before to After, or none when they are equal.
        HistoryResult EndTransaction(Snapshot after)
        {
            if (m_Transactions.empty())
                return MakeResult(HistoryStatus::NoTransaction);
            if (!after)
                return MakeResult(HistoryStatus::InvalidArgument);
            if (m_Transactions.size() > 1)
            {
                m_Transactions.pop_back();
                return MakeResult(HistoryStatus::Ok);
            }

            TransactionLevel level = std::move(m_Transactions.back());
            m_Transactions.pop_back();
            if (StatesEqual(level.Before, after))
                return MakeResult(HistoryStatus::NoChange);
            return Commit(std::move(level.Label), std::move(level.Before), std::move(after));
        }

        // Restores the innermost transaction's Before and closes that level
        // only. If the restore fails the level stays open.
        HistoryResult AbortTransaction()
        {
            if (m_Transactions.empty())
                return MakeResult(HistoryStatus::NoTransaction);
            if (!m_Adapter.Restore(m_Transactions.back().Before))
                return MakeResult(HistoryStatus::RestoreFailed, "Abort failed: could not restore the previous state");
            m_Transactions.pop_back();
            return MakeResult(HistoryStatus::Ok);
        }

        // Sharpens the label of the open gesture, or of the outermost
        // transaction ("Move" becomes "Move Cube" once the target is known).
        bool RefineOpenLabel(HistoryLabel label)
        {
            if (m_Gesture)
                m_Gesture->Label = std::move(label);
            else if (!m_Transactions.empty())
                m_Transactions.front().Label = std::move(label);
            else
                return false;
            return true;
        }

        // ---- Navigation ----

        HistoryResult Undo()
        {
            const HistoryAvailability availability = UndoAvailability();
            if (!availability.Enabled)
                return MakeResult(StatusForBlock(availability.Block), availability.Detail);

            const Entry& entry = m_Entries[m_Cursor - 1];
            if (!m_Adapter.Restore(entry.Before))
                return MakeResult(HistoryStatus::RestoreFailed, "Undo failed: could not restore " + entry.Info.Label.Display());
            --m_Cursor;
            return Moved(entry.Info, 1, "Undo: " + entry.Info.Label.Display());
        }

        HistoryResult Redo()
        {
            const HistoryAvailability availability = RedoAvailability();
            if (!availability.Enabled)
                return MakeResult(StatusForBlock(availability.Block), availability.Detail);

            const Entry& entry = m_Entries[m_Cursor];
            if (!m_Adapter.Restore(entry.After))
                return MakeResult(HistoryStatus::RestoreFailed, "Redo failed: could not restore " + entry.Info.Label.Display());
            ++m_Cursor;
            return Moved(entry.Info, 1, "Redo: " + entry.Info.Label.Display());
        }

        // Moves to the position after the row with this revision (the base row's
        // revision moves to before every entry) with a single restore.
        HistoryResult JumpTo(HistoryRevision revision)
        {
            size_t row = m_Entries.size() + 1;
            if (revision == m_Base.Revision)
            {
                row = 0;
            }
            else
            {
                for (size_t index = 0; index < m_Entries.size(); ++index)
                {
                    if (m_Entries[index].Info.Revision == revision)
                    {
                        row = index + 1;
                        break;
                    }
                }
            }
            return JumpToRow(row);
        }

        // The same by index into Rows(): row 0 is the base row, row i the entry
        // whose result is the live state after i applied entries.
        HistoryResult JumpToRow(size_t row)
        {
            if (m_Gesture)
                return MakeResult(HistoryStatus::GestureOpen, "Cannot jump: finish the current edit");
            if (!m_Transactions.empty())
                return MakeResult(HistoryStatus::TransactionOpen, "Cannot jump: finish the current edit");
            if (row > m_Entries.size())
                return MakeResult(HistoryStatus::UnknownRevision, "Cannot jump: that history row no longer exists");
            if (row == m_Cursor)
                return MakeResult(HistoryStatus::NoChange);

            const size_t target = row;
            const bool backward = target < m_Cursor;
            const Snapshot& snapshot = backward ? m_Entries[target].Before : m_Entries[target - 1].After;
            const HistoryLabel& label = target == 0 ? m_Base.Label : m_Entries[target - 1].Info.Label;
            if (!m_Adapter.Restore(snapshot))
                return MakeResult(HistoryStatus::RestoreFailed, "Jump failed: could not restore " + label.Display());

            const size_t steps = backward ? m_Cursor - target : target - m_Cursor;
            m_Cursor = target;
            HistoryEntryInfo info;
            info.Revision = HeadRevision();
            info.Label = label;
            return Moved(info, steps,
                "Jumped to: " + label.Display() + " (" + std::to_string(steps) + (steps == 1 ? " step " : " steps ")
                    + (backward ? "back)" : "forward)"));
        }

        // ---- Re-basing ----

        // New or loaded project: drops every entry, gesture, and transaction.
        HistoryResult Reset(HistoryLabel baseLabel)
        {
            ClearEntries();
            m_Gesture.reset();
            m_Transactions.clear();
            m_Evicted = {};
            m_Base = { m_NextRevision++, std::move(baseLabel), BaseKind::Opened, {} };
            return MakeResult(HistoryStatus::Ok);
        }

        // Undo barrier: drops both stacks because earlier snapshots can no
        // longer be restored (project changes were committed to disk). Undo is
        // unavailable until a new entry exists, and the reason is shown.
        HistoryResult Barrier(HistoryLabel label, std::string reason)
        {
            if (m_Gesture)
                return MakeResult(HistoryStatus::GestureOpen);
            if (!m_Transactions.empty())
                return MakeResult(HistoryStatus::TransactionOpen);
            ClearEntries();
            m_Base = { m_NextRevision++, std::move(label), BaseKind::Barrier, std::move(reason) };
            return MakeResult(HistoryStatus::Ok);
        }

        // ---- Rollback ----

        // Invalid while a gesture or transaction is open.
        Mark SaveMark() const
        {
            Mark mark;
            if (m_Gesture || !m_Transactions.empty())
                return mark;
            mark.Valid = true;
            mark.Entries = m_Entries;
            mark.Cursor = m_Cursor;
            mark.BaseRow = m_Base;
            mark.Evicted = m_Evicted;
            return mark;
        }

        bool LoadMark(const Mark& mark)
        {
            if (!mark.Valid || m_Gesture || !m_Transactions.empty())
                return false;
            ClearEntries();
            for (const Entry& entry : mark.Entries)
            {
                Reacquire(entry.Before, entry.BeforeBytes);
                Reacquire(entry.After, entry.AfterBytes);
            }
            m_Entries = mark.Entries;
            m_Cursor = mark.Cursor;
            m_Base = mark.BaseRow;
            m_Evicted = mark.Evicted;
            return true;
        }

        // ---- Observation ----

        HistoryRevision HeadRevision() const { return m_Cursor == 0 ? m_Base.Revision : m_Entries[m_Cursor - 1].Info.Revision; }
        size_t Cursor() const { return m_Cursor; }
        size_t UndoDepth() const { return m_Cursor; }
        size_t RedoDepth() const { return m_Entries.size() - m_Cursor; }
        size_t EntryCount() const { return m_Entries.size(); }
        const HistoryConfig& Config() const { return m_Config; }
        Engine::u64 UsedBytes() const { return m_UsedBytes; }
        const EvictionStats& Evicted() const { return m_Evicted; }

        const HistoryEntryInfo* TopUndo() const { return m_Cursor == 0 ? nullptr : &m_Entries[m_Cursor - 1].Info; }
        const HistoryEntryInfo* TopRedo() const { return m_Cursor == m_Entries.size() ? nullptr : &m_Entries[m_Cursor].Info; }

        bool GestureOpen() const { return m_Gesture.has_value(); }
        const EditGestureKey* OpenGestureKey() const { return m_Gesture ? &m_Gesture->Key : nullptr; }
        size_t GestureUpdates() const { return m_Gesture ? m_Gesture->Updates : 0; }
        size_t TransactionDepth() const { return m_Transactions.size(); }

        bool BaseIsBarrier() const { return m_Base.Kind == BaseKind::Barrier; }

        HistoryAvailability UndoAvailability() const
        {
            HistoryAvailability result;
            if (m_Gesture || !m_Transactions.empty())
            {
                result.Block = m_Gesture ? HistoryBlock::GestureOpen : HistoryBlock::TransactionOpen;
                result.MenuLabel = "Undo (finish the current edit)";
                result.Detail = "Cannot undo: finish the current edit";
            }
            else if (m_Cursor == 0 && m_Base.Kind == BaseKind::Barrier)
            {
                result.Block = HistoryBlock::UndoBarrier;
                result.MenuLabel = "Undo (blocked: " + m_Base.Label.Display() + ")";
                result.Detail = "Cannot undo past: " + m_Base.Label.Display() + " - " + m_Base.Reason;
            }
            else if (m_Cursor == 0)
            {
                result.Block = HistoryBlock::NothingToUndo;
                result.MenuLabel = "Undo (nothing to undo)";
                result.Detail = m_Base.Kind == BaseKind::Dropped ? "Nothing to undo: " + m_Base.Reason : "Nothing to undo";
            }
            else
            {
                result.Enabled = true;
                result.MenuLabel = "Undo " + m_Entries[m_Cursor - 1].Info.Label.Display();
            }
            return result;
        }

        HistoryAvailability RedoAvailability() const
        {
            HistoryAvailability result;
            if (m_Gesture || !m_Transactions.empty())
            {
                result.Block = m_Gesture ? HistoryBlock::GestureOpen : HistoryBlock::TransactionOpen;
                result.MenuLabel = "Redo (finish the current edit)";
                result.Detail = "Cannot redo: finish the current edit";
            }
            else if (m_Cursor == m_Entries.size())
            {
                result.Block = HistoryBlock::NothingToRedo;
                result.MenuLabel = "Redo (nothing to redo)";
                result.Detail = "Nothing to redo";
            }
            else
            {
                result.Enabled = true;
                result.MenuLabel = "Redo " + m_Entries[m_Cursor].Info.Label.Display();
            }
            return result;
        }

        // Base row first, then every entry oldest to newest.
        std::vector<HistoryRow> Rows() const
        {
            std::vector<HistoryRow> rows;
            rows.reserve(m_Entries.size() + 1);
            HistoryRow base;
            base.Revision = m_Base.Revision;
            base.Label = m_Base.Label;
            base.Display = m_Base.Label.Display() + (m_Base.Kind == BaseKind::Barrier ? " (barrier)" : "");
            base.IsBase = true;
            base.IsBarrier = m_Base.Kind == BaseKind::Barrier;
            base.Applied = true;
            base.Current = m_Cursor == 0;
            rows.push_back(std::move(base));
            for (size_t index = 0; index < m_Entries.size(); ++index)
            {
                HistoryRow row;
                row.Revision = m_Entries[index].Info.Revision;
                row.Label = m_Entries[index].Info.Label;
                row.Display = row.Label.Display();
                row.Bytes = m_Entries[index].Info.Bytes;
                row.Applied = index < m_Cursor;
                row.Current = index + 1 == m_Cursor;
                rows.push_back(std::move(row));
            }
            return rows;
        }

    private:
        struct OpenGesture
        {
            EditGestureKey Key;
            HistoryLabel Label;
            Snapshot Before;
            size_t Updates = 0;
        };

        struct TransactionLevel
        {
            HistoryLabel Label;
            Snapshot Before;
        };

        struct Slot
        {
            Engine::u64 Bytes = 0;
            size_t References = 0;
        };

        static HistoryStatus StatusForBlock(HistoryBlock block)
        {
            switch (block)
            {
            case HistoryBlock::NothingToUndo: return HistoryStatus::NothingToUndo;
            case HistoryBlock::NothingToRedo: return HistoryStatus::NothingToRedo;
            case HistoryBlock::UndoBarrier: return HistoryStatus::UndoBarrier;
            case HistoryBlock::GestureOpen: return HistoryStatus::GestureOpen;
            case HistoryBlock::TransactionOpen: return HistoryStatus::TransactionOpen;
            case HistoryBlock::None: break;
            }
            return HistoryStatus::Ok;
        }

        HistoryResult MakeResult(HistoryStatus status, std::string announcement = {}) const
        {
            HistoryResult result;
            result.Status = status;
            result.Head = HeadRevision();
            result.Announcement = std::move(announcement);
            return result;
        }

        HistoryResult Moved(const HistoryEntryInfo& info, size_t steps, std::string announcement) const
        {
            HistoryResult result = MakeResult(HistoryStatus::Ok, std::move(announcement));
            result.Revision = info.Revision;
            result.Label = info.Label;
            result.Steps = steps;
            return result;
        }

        bool StatesEqual(const Snapshot& first, const Snapshot& second) const
        {
            return first == second || m_Adapter.Equal(*first, *second);
        }

        Engine::u64 Acquire(const Snapshot& snapshot)
        {
            Slot& slot = m_Slots[snapshot.get()];
            if (slot.References++ == 0)
            {
                slot.Bytes = m_Adapter.EstimateBytes(*snapshot);
                m_UsedBytes += slot.Bytes;
            }
            return slot.Bytes;
        }

        void Reacquire(const Snapshot& snapshot, Engine::u64 bytes)
        {
            Slot& slot = m_Slots[snapshot.get()];
            if (slot.References++ == 0)
            {
                slot.Bytes = bytes;
                m_UsedBytes += bytes;
            }
        }

        void Release(const Snapshot& snapshot)
        {
            const auto found = m_Slots.find(snapshot.get());
            if (--found->second.References == 0)
            {
                m_UsedBytes -= found->second.Bytes;
                m_Slots.erase(found);
            }
        }

        void ReleaseEntry(const Entry& entry)
        {
            Release(entry.Before);
            Release(entry.After);
        }

        void ClearEntries()
        {
            for (const Entry& entry : m_Entries)
                ReleaseEntry(entry);
            m_Entries.clear();
            m_Cursor = 0;
        }

        HistoryResult Commit(HistoryLabel label, Snapshot before, Snapshot after)
        {
            const size_t discarded = m_Entries.size() - m_Cursor;
            for (size_t index = m_Cursor; index < m_Entries.size(); ++index)
                ReleaseEntry(m_Entries[index]);
            m_Entries.resize(m_Cursor);

            Entry entry;
            entry.Info.Label = std::move(label);
            entry.BeforeBytes = Acquire(before);
            entry.AfterBytes = Acquire(after);
            entry.Info.Bytes = entry.BeforeBytes + entry.AfterBytes;
            entry.Before = std::move(before);
            entry.After = std::move(after);
            m_Entries.push_back(std::move(entry));
            m_Cursor = m_Entries.size();

            EvictionNotice notice;
            while (m_Entries.size() > 1)
            {
                const bool overBudget = m_UsedBytes > m_Config.BudgetBytes;
                const bool overCap = m_Entries.size() > m_Config.MaximumEntries;
                if (!overBudget && !overCap)
                    break;
                const Engine::u64 usedBefore = m_UsedBytes;
                ReleaseEntry(m_Entries.front());
                m_Entries.erase(m_Entries.begin());
                --m_Cursor;
                notice.Bytes += usedBefore - m_UsedBytes;
                if (overBudget)
                    ++notice.ForBudget;
                else
                    ++notice.ForEntryCap;
            }
            if (notice.Entries() != 0)
            {
                m_Evicted.Entries += notice.Entries();
                m_Evicted.Bytes += notice.Bytes;
                ++m_Evicted.Events;
                m_Base = { m_NextRevision++, MakeHistoryLabel("Earlier history dropped", "", HistorySource::System), BaseKind::Dropped,
                    "earlier history was dropped to stay within the history limit" };
            }
            m_Entries.back().Info.Revision = m_NextRevision++;

            HistoryResult result = MakeResult(HistoryStatus::Recorded);
            result.Revision = m_Entries.back().Info.Revision;
            result.Label = m_Entries.back().Info.Label;
            result.RedoDiscarded = discarded;
            result.Evicted = notice;
            if (discarded != 0)
                result.Announcement = FormatRedoDiscardedNotice(discarded);
            if (notice.Entries() != 0)
            {
                if (!result.Announcement.empty())
                    result.Announcement += "; ";
                result.Announcement += FormatEvictionNotice(notice.ForBudget, notice.ForEntryCap, m_Config.BudgetBytes, m_Config.MaximumEntries);
            }
            return result;
        }

        IHistoryStateAdapter<State>& m_Adapter;
        HistoryConfig m_Config;
        std::vector<Entry> m_Entries;
        size_t m_Cursor = 0;
        Base m_Base;
        HistoryRevision m_NextRevision = 1;
        std::unordered_map<const State*, Slot> m_Slots;
        Engine::u64 m_UsedBytes = 0;
        EvictionStats m_Evicted;
        std::optional<OpenGesture> m_Gesture;
        std::vector<TransactionLevel> m_Transactions;
    };
}
