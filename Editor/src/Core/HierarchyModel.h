#pragma once

#include "EntityNaming.h"
#include "SelectionModel.h"

#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SpiralEditor
{
    // Component presence bits a row can carry; Transform is universal and has none.
    namespace HierarchyComponent
    {
        constexpr Engine::u32 Camera = 1u << 0;
        constexpr Engine::u32 Light = 1u << 1;
        constexpr Engine::u32 MeshRenderer = 1u << 2;
    }

    constexpr size_t kMaxHierarchyFilterBytes = 256;

    // The Scene-derived facts the outliner needs, supplied by the caller in Scene
    // order. Visible mirrors the document's mesh visibility; it is display and
    // filter input here, never model-owned state.
    struct HierarchyRow
    {
        EntityId Id = kNoEntity;
        std::string Name;
        Engine::u32 Components = 0;
        bool Visible = true;

        bool operator==(const HierarchyRow&) const = default;
    };

    enum class VisibilityFilter
    {
        Any,
        VisibleOnly,
        HiddenOnly
    };

    struct HierarchyFilter
    {
        // ASCII case-insensitive substring of the name; bytes >= 0x80 compare exactly.
        std::string Text;
        // Every bit must be present on the row.
        Engine::u32 RequiredComponents = 0;
        VisibilityFilter Visibility = VisibilityFilter::Any;

        bool operator==(const HierarchyFilter&) const = default;
    };

    bool RowMatchesFilter(const HierarchyRow& row, const HierarchyFilter& filter);

    enum class RenameOutcome
    {
        Renamed,
        Unchanged,
        UnknownEntity,
        InvalidName,
        ProtectedEntity
    };

    struct RenamePlan
    {
        RenameOutcome Outcome = RenameOutcome::UnknownEntity;
        EntityNameStatus Status = EntityNameStatus::Valid; // meaningful for InvalidName
        std::string Name; // the name to apply when Outcome is Renamed
        bool Adjusted = false; // Name differs from the trimmed request because it was in use or reserved
    };

    // Flat, ordered outliner view model. It owns the filter, the filtered row order,
    // and the session-only lock flags; it never touches the Scene, so every result is
    // a plan the Editor applies through the normal history path.
    class HierarchyModel
    {
    public:
        // Replaces the rows. Rows with kNoEntity or an id already seen are dropped.
        // Returns true only when the resulting rows differ from the current ones.
        bool SetRows(std::vector<HierarchyRow> rows);
        // Text is cut to kMaxHierarchyFilterBytes. Returns true only on change.
        bool SetFilter(HierarchyFilter filter);

        const std::vector<HierarchyRow>& Rows() const { return m_Rows; }
        const HierarchyFilter& Filter() const { return m_Filter; }
        // Ids of the rows that match the filter, in row order.
        const std::vector<EntityId>& VisibleIds() const { return m_VisibleIds; }
        std::vector<EntityId> AllIds() const;
        const HierarchyRow* FindRow(EntityId id) const;

        // Locks are session-only Editor state keyed by id, so a lock survives a delete
        // that is undone. Only an id that is currently a row can be newly locked.
        bool SetLocked(EntityId id, bool locked);
        bool IsLocked(EntityId id) const { return m_Locked.contains(id); }
        void ClearLocks() { m_Locked.clear(); }
        std::vector<EntityId> Unlocked(std::span<const EntityId> ids) const;

        // Names currently in use plus the reserved lookup names.
        EntityNamePool MakeNamePool() const;
        // Trims and validates `requested`; resolves collisions with " (N)". An entity
        // that owns a reserved lookup name cannot be renamed away from it.
        RenamePlan PlanRename(EntityId id, std::string_view requested) const;

    private:
        void RebuildVisible();

        std::vector<HierarchyRow> m_Rows;
        std::unordered_map<EntityId, size_t> m_RowIndex;
        HierarchyFilter m_Filter;
        std::vector<EntityId> m_VisibleIds;
        std::unordered_set<EntityId> m_Locked;
    };

    // Moves `moved` (any order) as one block, keeping its relative order from `order`,
    // to just before `insertBefore` (kNoEntity appends). When `insertBefore` is itself
    // moved, the block lands before the first unmoved row at or after it, so dropping
    // a block onto itself is a no-op. Returns nullopt when `moved` is empty or has an
    // unknown or duplicate id, or `insertBefore` is neither kNoEntity nor in `order`,
    // or `order` has duplicate or kNoEntity ids.
    std::optional<std::vector<EntityId>> ReorderEntities(std::span<const EntityId> order,
        std::span<const EntityId> moved, EntityId insertBefore);
}
