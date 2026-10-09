#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Scene/Entity.h"

#include <span>
#include <unordered_set>
#include <vector>

namespace SpiralEditor
{
    using EntityId = Engine::EntityId;
    constexpr EntityId kNoEntity = Engine::kInvalidEntityId;

    // Everything an undo entry needs to restore a selection exactly.
    struct SelectionState
    {
        std::vector<EntityId> Ids; // selection order, oldest first, unique, never kNoEntity
        EntityId Primary = kNoEntity; // always a member of Ids when Ids is not empty
        EntityId Anchor = kNoEntity; // start of the next Shift range; need not be selected

        bool operator==(const SelectionState&) const = default;
    };

    // Ordered multi-selection over opaque entity ids. The caller supplies the
    // visible (filtered) row order to the operations that need one; the model
    // never reads the Scene. Every mutator returns true only when the observable
    // selection changed: a different member set or a different primary. Reordering
    // the same members, or moving only the anchor, is not a change and does not
    // advance Revision().
    class SelectionModel
    {
    public:
        const std::vector<EntityId>& Ids() const { return m_Ids; }
        EntityId Primary() const { return m_Primary; }
        EntityId Anchor() const { return m_Anchor; }
        size_t Count() const { return m_Ids.size(); }
        bool Empty() const { return m_Ids.empty(); }
        bool IsSelected(EntityId id) const { return m_Members.contains(id); }
        Engine::u64 Revision() const { return m_Revision; }
        SelectionState Capture() const;

        // The selected ids in the order of `order` (ids absent from `order` are omitted).
        std::vector<EntityId> InOrder(std::span<const EntityId> order) const;

        // Plain click: the single selection, primary and anchor.
        bool Click(EntityId id);
        // Ctrl-click: toggles one id and moves the anchor to it. Selecting makes it
        // primary; deselecting the primary promotes the most recently selected survivor.
        bool Toggle(EntityId id);
        // Shift-click: the inclusive range between the anchor and `id` in `order`.
        // Replaces the selection, or extends it when `additive`. Falls back to Click
        // when the anchor is not in `order`; does nothing when `id` is not in `order`.
        bool SelectRange(std::span<const EntityId> order, EntityId id, bool additive);
        // Adds every id of `order`; members outside `order` are kept.
        bool SelectAll(std::span<const EntityId> order);
        // Flips the membership of every id of `order`; members outside `order` are kept.
        bool Invert(std::span<const EntityId> order);
        bool Clear();
        // Drops ids absent from `existing` (the complete entity list). A missing anchor
        // stays missing; an anchor whose entity is gone falls back to the primary.
        bool Prune(std::span<const EntityId> existing);
        // Adopts a recorded state, dropping ids absent from `existing` and repairing
        // primary/anchor the same way, so restoring after undo cannot select a missing entity.
        bool Restore(const SelectionState& state, std::span<const EntityId> existing);

    private:
        bool Assign(std::vector<EntityId> ids, EntityId primary, EntityId anchor);

        std::vector<EntityId> m_Ids;
        std::unordered_set<EntityId> m_Members;
        EntityId m_Primary = kNoEntity;
        EntityId m_Anchor = kNoEntity;
        Engine::u64 m_Revision = 0;
    };
}
