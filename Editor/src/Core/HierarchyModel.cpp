#include "HierarchyModel.h"

#include <algorithm>

namespace SpiralEditor
{
    namespace
    {
        char FoldAscii(char value)
        {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        }

        bool ContainsFolded(std::string_view haystack, std::string_view needle)
        {
            if (needle.empty())
                return true;
            if (needle.size() > haystack.size())
                return false;
            for (size_t start = 0; start + needle.size() <= haystack.size(); ++start)
            {
                size_t offset = 0;
                while (offset < needle.size() && FoldAscii(haystack[start + offset]) == FoldAscii(needle[offset]))
                    ++offset;
                if (offset == needle.size())
                    return true;
            }
            return false;
        }
    }

    bool RowMatchesFilter(const HierarchyRow& row, const HierarchyFilter& filter)
    {
        if ((row.Components & filter.RequiredComponents) != filter.RequiredComponents)
            return false;
        if (filter.Visibility == VisibilityFilter::VisibleOnly && !row.Visible)
            return false;
        if (filter.Visibility == VisibilityFilter::HiddenOnly && row.Visible)
            return false;
        return ContainsFolded(row.Name, filter.Text);
    }

    bool HierarchyModel::SetRows(std::vector<HierarchyRow> rows)
    {
        std::unordered_set<EntityId> seen;
        std::vector<HierarchyRow> accepted;
        accepted.reserve(rows.size());
        for (HierarchyRow& row : rows)
        {
            if (row.Id != kNoEntity && seen.insert(row.Id).second)
                accepted.push_back(std::move(row));
        }
        if (accepted == m_Rows)
            return false;

        m_Rows = std::move(accepted);
        m_RowIndex.clear();
        for (size_t index = 0; index < m_Rows.size(); ++index)
            m_RowIndex.emplace(m_Rows[index].Id, index);
        RebuildVisible();
        return true;
    }

    bool HierarchyModel::SetFilter(HierarchyFilter filter)
    {
        if (filter.Text.size() > kMaxHierarchyFilterBytes)
            filter.Text.resize(kMaxHierarchyFilterBytes);
        if (filter == m_Filter)
            return false;
        m_Filter = std::move(filter);
        RebuildVisible();
        return true;
    }

    void HierarchyModel::RebuildVisible()
    {
        m_VisibleIds.clear();
        for (const HierarchyRow& row : m_Rows)
        {
            if (RowMatchesFilter(row, m_Filter))
                m_VisibleIds.push_back(row.Id);
        }
    }

    std::vector<EntityId> HierarchyModel::AllIds() const
    {
        std::vector<EntityId> ids;
        ids.reserve(m_Rows.size());
        for (const HierarchyRow& row : m_Rows)
            ids.push_back(row.Id);
        return ids;
    }

    const HierarchyRow* HierarchyModel::FindRow(EntityId id) const
    {
        const auto found = m_RowIndex.find(id);
        return found == m_RowIndex.end() ? nullptr : &m_Rows[found->second];
    }

    bool HierarchyModel::SetLocked(EntityId id, bool locked)
    {
        if (!locked)
            return m_Locked.erase(id) != 0;
        if (!m_RowIndex.contains(id))
            return false;
        return m_Locked.insert(id).second;
    }

    std::vector<EntityId> HierarchyModel::Unlocked(std::span<const EntityId> ids) const
    {
        std::vector<EntityId> result;
        for (const EntityId id : ids)
        {
            if (!m_Locked.contains(id))
                result.push_back(id);
        }
        return result;
    }

    EntityNamePool HierarchyModel::MakeNamePool() const
    {
        EntityNamePool pool;
        for (const HierarchyRow& row : m_Rows)
            pool.Add(row.Name);
        return pool;
    }

    RenamePlan HierarchyModel::PlanRename(EntityId id, std::string_view requested) const
    {
        RenamePlan plan;
        const HierarchyRow* row = FindRow(id);
        if (!row)
            return plan;

        const std::string_view trimmed = TrimEntityName(requested);
        if (trimmed == row->Name)
        {
            plan.Outcome = RenameOutcome::Unchanged;
            plan.Name = row->Name;
            return plan;
        }

        const std::span<const std::string_view> reserved = LookupReservedEntityNames();
        if (std::find(reserved.begin(), reserved.end(), std::string_view(row->Name)) != reserved.end())
        {
            plan.Outcome = RenameOutcome::ProtectedEntity;
            return plan;
        }

        plan.Status = ValidateEntityName(trimmed);
        if (plan.Status != EntityNameStatus::Valid)
        {
            plan.Outcome = RenameOutcome::InvalidName;
            return plan;
        }

        EntityNamePool pool = MakeNamePool();
        pool.Remove(row->Name);
        plan.Name = pool.Resolve(trimmed);
        plan.Adjusted = plan.Name != trimmed;
        plan.Outcome = plan.Name == row->Name ? RenameOutcome::Unchanged : RenameOutcome::Renamed;
        return plan;
    }

    std::optional<std::vector<EntityId>> ReorderEntities(std::span<const EntityId> order,
        std::span<const EntityId> moved, EntityId insertBefore)
    {
        if (moved.empty())
            return std::nullopt;

        std::unordered_map<EntityId, size_t> position;
        position.reserve(order.size());
        for (size_t index = 0; index < order.size(); ++index)
        {
            if (order[index] == kNoEntity || !position.emplace(order[index], index).second)
                return std::nullopt;
        }

        std::unordered_set<EntityId> movedSet;
        for (const EntityId id : moved)
        {
            if (!position.contains(id) || !movedSet.insert(id).second)
                return std::nullopt;
        }
        if (insertBefore != kNoEntity && !position.contains(insertBefore))
            return std::nullopt;

        constexpr size_t kUnplaced = static_cast<size_t>(-1);
        const size_t dropIndex = insertBefore == kNoEntity ? order.size() : position[insertBefore];
        std::vector<EntityId> block;
        std::vector<EntityId> rest;
        size_t insertAt = kUnplaced;
        for (size_t index = 0; index < order.size(); ++index)
        {
            const EntityId id = order[index];
            if (movedSet.contains(id))
            {
                block.push_back(id);
                continue;
            }
            if (insertAt == kUnplaced && index >= dropIndex)
                insertAt = rest.size();
            rest.push_back(id);
        }
        if (insertAt == kUnplaced)
            insertAt = rest.size();

        rest.insert(rest.begin() + static_cast<std::ptrdiff_t>(insertAt), block.begin(), block.end());
        return rest;
    }
}
