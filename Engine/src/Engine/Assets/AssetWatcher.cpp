#include "Engine/Assets/AssetWatcher.h"

#include "Engine/Assets/AssetFileSystem.h"
#include "Engine/Core/Base.h"

#include <algorithm>
#include <system_error>
#include <unordered_set>
#include <utility>

#if defined(GE_PLATFORM_LINUX)
    #include <sys/stat.h>
#endif

namespace Engine
{
    const char* ToString(AssetWatchEventType type)
    {
        switch (type)
        {
            case AssetWatchEventType::Modified: return "Modified";
            case AssetWatchEventType::Deleted: return "Deleted";
            case AssetWatchEventType::Restored: return "Restored";
        }

        return "Unknown";
    }

    void AssetWatcher::SetPathResolver(AssetPathResolver resolver)
    {
        m_PathResolver = std::move(resolver);
        m_Records.clear();
        m_RecordIndex.clear();
    }

    std::filesystem::path AssetWatcher::Resolve(std::string_view sourcePath) const
    {
        return m_PathResolver ? m_PathResolver(sourcePath) : AssetFileSystem::ResolvePath(sourcePath);
    }

    void AssetWatcher::RebuildIndex()
    {
        m_RecordIndex.clear();
        m_RecordIndex.reserve(m_Records.size());
        for (std::size_t index = 0; index < m_Records.size(); ++index)
            m_RecordIndex.emplace(m_Records[index].Handle, index);
    }

    void AssetWatcher::SyncRegistry(const AssetRegistry& registry)
    {
        const std::vector<AssetMetadata>& assets = registry.GetAssets();

        // Linear reconciliation: one pass collects the physical handles, one erase drops the
        // records that left the registry, one pass adds or refreshes the rest.
        std::unordered_set<AssetHandle> physical;
        physical.reserve(assets.size());
        for (const AssetMetadata& metadata : assets)
            if (metadata.SourcePolicy == AssetSourcePolicy::PhysicalFile)
                physical.insert(metadata.Handle);

        const std::size_t before = m_Records.size();
        m_Records.erase(
            std::remove_if(m_Records.begin(), m_Records.end(), [&physical](const Record& record)
            {
                return physical.find(record.Handle) == physical.end();
            }),
            m_Records.end());
        if (m_Records.size() != before || m_RecordIndex.size() != m_Records.size())
            RebuildIndex();

        for (const AssetMetadata& metadata : assets)
        {
            if (metadata.SourcePolicy != AssetSourcePolicy::PhysicalFile)
                continue;

            Record* record = FindRecord(metadata.Handle);
            if (!record)
            {
                Record newRecord;
                newRecord.Handle = metadata.Handle;
                newRecord.Type = metadata.Type;
                newRecord.SourcePath = metadata.SourcePath;
                newRecord.ResolvedPath = Resolve(metadata.SourcePath);
                newRecord.LastSnapshot = Capture(newRecord.ResolvedPath);
                m_RecordIndex.emplace(newRecord.Handle, m_Records.size());
                m_Records.push_back(std::move(newRecord));
                continue;
            }

            record->Type = metadata.Type;
            if (record->SourcePath != metadata.SourcePath)
            {
                record->SourcePath = metadata.SourcePath;
                record->ResolvedPath = Resolve(metadata.SourcePath);
                record->LastSnapshot = Capture(record->ResolvedPath);
            }
        }
    }

    std::vector<AssetWatchEvent> AssetWatcher::Poll(const AssetRegistry& registry)
    {
        const auto now = std::chrono::steady_clock::now();
        if (m_MinimumPollInterval.count() > 0 && m_HasPolled && now - m_LastPoll < m_MinimumPollInterval)
            return {};
        m_LastPoll = now;
        m_HasPolled = true;

        SyncRegistry(registry);

        std::vector<AssetWatchEvent> events;
        for (Record& record : m_Records)
        {
            // A present file keeps its resolved path (one stat per poll). Only a missing source is
            // searched for again, because it may have reappeared under another search root.
            if (!record.LastSnapshot.Exists)
                record.ResolvedPath = Resolve(record.SourcePath);
            const Snapshot current = Capture(record.ResolvedPath);
            if (!IsDifferent(record.LastSnapshot, current))
                continue;

            AssetWatchEvent event;
            event.Handle = record.Handle;
            event.Type = record.Type;
            event.SourcePath = record.SourcePath;
            event.ResolvedPath = record.ResolvedPath;
            if (record.LastSnapshot.Exists && !current.Exists)
                event.EventType = AssetWatchEventType::Deleted;
            else if (!record.LastSnapshot.Exists && current.Exists)
                event.EventType = AssetWatchEventType::Restored;
            else
                event.EventType = AssetWatchEventType::Modified;

            events.push_back(std::move(event));
            record.LastSnapshot = current;
        }

        return events;
    }

    void AssetWatcher::Acknowledge(AssetHandle handle)
    {
        Record* record = FindRecord(handle);
        if (record)
            record->LastSnapshot = Capture(record->ResolvedPath);
    }

    void AssetWatcher::Clear()
    {
        m_Records.clear();
        m_RecordIndex.clear();
    }

    std::size_t AssetWatcher::GetMissingCount() const
    {
        return static_cast<std::size_t>(std::count_if(m_Records.begin(), m_Records.end(), [](const Record& record)
        {
            return !record.LastSnapshot.Exists;
        }));
    }

    AssetWatcher::Snapshot AssetWatcher::Capture(const std::filesystem::path& path)
    {
        Snapshot snapshot;

#if !defined(GE_PLATFORM_LINUX)
        std::error_code error;
        snapshot.Exists = std::filesystem::exists(path, error);
        if (error || !snapshot.Exists)
            return snapshot;

        snapshot.LastWriteTime = std::filesystem::last_write_time(path, error);
        if (error)
        {
            snapshot.Exists = false;
            return snapshot;
        }

        if (std::filesystem::is_regular_file(path, error) && !error)
            snapshot.Size = std::filesystem::file_size(path, error);
        if (error)
            snapshot.Size = 0;
#else
        // One stat answers existence, type, size and modification time.
        struct stat status {};
        if (::stat(path.c_str(), &status) != 0)
            return snapshot;
        snapshot.Exists = true;
        snapshot.LastWriteTime = std::filesystem::file_time_type(std::chrono::duration_cast<
            std::filesystem::file_time_type::duration>(std::chrono::seconds(status.st_mtim.tv_sec)
                + std::chrono::nanoseconds(status.st_mtim.tv_nsec)));
        snapshot.Size = S_ISREG(status.st_mode) ? static_cast<std::uintmax_t>(status.st_size) : 0;
#endif

        return snapshot;
    }

    bool AssetWatcher::IsDifferent(const Snapshot& previous, const Snapshot& current)
    {
        if (previous.Exists != current.Exists)
            return true;

        if (!current.Exists)
            return false;

        return previous.LastWriteTime != current.LastWriteTime || previous.Size != current.Size;
    }

    AssetWatcher::Record* AssetWatcher::FindRecord(AssetHandle handle)
    {
        const auto it = m_RecordIndex.find(handle);
        return it == m_RecordIndex.end() ? nullptr : &m_Records[it->second];
    }
}
