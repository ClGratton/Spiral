#include "SelectionModel.h"

#include <algorithm>

namespace SpiralEditor
{
    namespace
    {
        // The primary is the most recently selected member, so an emptied-then-refilled
        // or partially deselected set always keeps the Inspector pointed at a member.
        EntityId RepairPrimary(const std::vector<EntityId>& ids, EntityId primary)
        {
            if (ids.empty())
                return kNoEntity;
            if (std::find(ids.begin(), ids.end(), primary) != ids.end())
                return primary;
            return ids.back();
        }

        size_t IndexOf(std::span<const EntityId> order, EntityId id)
        {
            const auto found = std::find(order.begin(), order.end(), id);
            return found == order.end() ? order.size() : static_cast<size_t>(found - order.begin());
        }
    }

    SelectionState SelectionModel::Capture() const
    {
        return { m_Ids, m_Primary, m_Anchor };
    }

    std::vector<EntityId> SelectionModel::InOrder(std::span<const EntityId> order) const
    {
        std::vector<EntityId> result;
        std::unordered_set<EntityId> emitted;
        for (const EntityId id : order)
        {
            if (m_Members.contains(id) && emitted.insert(id).second)
                result.push_back(id);
        }
        return result;
    }

    bool SelectionModel::Assign(std::vector<EntityId> ids, EntityId primary, EntityId anchor)
    {
        bool changed = primary != m_Primary || ids.size() != m_Ids.size();
        if (!changed)
        {
            for (const EntityId id : ids)
            {
                if (!m_Members.contains(id))
                {
                    changed = true;
                    break;
                }
            }
        }

        m_Ids = std::move(ids);
        m_Members.clear();
        m_Members.insert(m_Ids.begin(), m_Ids.end());
        m_Primary = primary;
        m_Anchor = anchor;
        if (changed)
            ++m_Revision;
        return changed;
    }

    bool SelectionModel::Click(EntityId id)
    {
        if (id == kNoEntity)
            return false;
        return Assign({ id }, id, id);
    }

    bool SelectionModel::Toggle(EntityId id)
    {
        if (id == kNoEntity)
            return false;

        std::vector<EntityId> ids = m_Ids;
        EntityId primary = m_Primary;
        const auto found = std::find(ids.begin(), ids.end(), id);
        if (found == ids.end())
        {
            ids.push_back(id);
            primary = id;
        }
        else
        {
            ids.erase(found);
            primary = RepairPrimary(ids, primary);
        }
        return Assign(std::move(ids), primary, id);
    }

    bool SelectionModel::SelectRange(std::span<const EntityId> order, EntityId id, bool additive)
    {
        const size_t target = IndexOf(order, id);
        if (id == kNoEntity || target == order.size())
            return false;

        const size_t anchor = IndexOf(order, m_Anchor);
        if (m_Anchor == kNoEntity || anchor == order.size())
            return Click(id);

        std::vector<EntityId> ids;
        std::unordered_set<EntityId> present;
        if (additive)
        {
            ids = m_Ids;
            present = m_Members;
        }
        const size_t first = std::min(anchor, target);
        const size_t last = std::max(anchor, target);
        for (size_t index = first; index <= last; ++index)
        {
            if (order[index] != kNoEntity && present.insert(order[index]).second)
                ids.push_back(order[index]);
        }
        return Assign(std::move(ids), id, m_Anchor);
    }

    bool SelectionModel::SelectAll(std::span<const EntityId> order)
    {
        std::vector<EntityId> ids = m_Ids;
        std::unordered_set<EntityId> present = m_Members;
        for (const EntityId id : order)
        {
            if (id != kNoEntity && present.insert(id).second)
                ids.push_back(id);
        }
        const EntityId primary = RepairPrimary(ids, m_Primary);
        return Assign(std::move(ids), primary, m_Anchor);
    }

    bool SelectionModel::Invert(std::span<const EntityId> order)
    {
        std::unordered_set<EntityId> flipped;
        for (const EntityId id : order)
        {
            if (id != kNoEntity)
                flipped.insert(id);
        }

        std::vector<EntityId> ids;
        ids.reserve(m_Ids.size());
        for (const EntityId id : m_Ids)
        {
            if (!flipped.contains(id))
                ids.push_back(id);
        }
        std::unordered_set<EntityId> appended;
        for (const EntityId id : order)
        {
            if (id != kNoEntity && !m_Members.contains(id) && appended.insert(id).second)
                ids.push_back(id);
        }
        const EntityId primary = RepairPrimary(ids, m_Primary);
        return Assign(std::move(ids), primary, m_Anchor);
    }

    bool SelectionModel::Clear()
    {
        return Assign({}, kNoEntity, kNoEntity);
    }

    bool SelectionModel::Prune(std::span<const EntityId> existing)
    {
        const std::unordered_set<EntityId> alive(existing.begin(), existing.end());
        std::vector<EntityId> ids;
        ids.reserve(m_Ids.size());
        for (const EntityId id : m_Ids)
        {
            if (alive.contains(id))
                ids.push_back(id);
        }
        const EntityId primary = RepairPrimary(ids, m_Primary);
        const EntityId anchor = m_Anchor == kNoEntity || alive.contains(m_Anchor) ? m_Anchor : primary;
        return Assign(std::move(ids), primary, anchor);
    }

    bool SelectionModel::Restore(const SelectionState& state, std::span<const EntityId> existing)
    {
        const std::unordered_set<EntityId> alive(existing.begin(), existing.end());
        std::vector<EntityId> ids;
        std::unordered_set<EntityId> present;
        for (const EntityId id : state.Ids)
        {
            if (id != kNoEntity && alive.contains(id) && present.insert(id).second)
                ids.push_back(id);
        }
        const EntityId primary = RepairPrimary(ids, state.Primary);
        const EntityId anchor = state.Anchor == kNoEntity || alive.contains(state.Anchor) ? state.Anchor : primary;
        return Assign(std::move(ids), primary, anchor);
    }
}
