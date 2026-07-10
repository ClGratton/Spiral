#include "Engine/Assets/AssetWatcher.h"

#include "Engine/Assets/AssetFileSystem.h"

#include <algorithm>
#include <system_error>
#include <utility>

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

    void AssetWatcher::SyncRegistry(const AssetRegistry& registry)
    {
        const std::vector<AssetMetadata>& assets = registry.GetAssets();
        m_Records.erase(
            std::remove_if(m_Records.begin(), m_Records.end(), [&assets](const Record& record)
            {
                return std::find_if(assets.begin(), assets.end(), [&record](const AssetMetadata& metadata)
                {
                    return metadata.Handle == record.Handle;
                }) == assets.end();
            }),
            m_Records.end());

        for (const AssetMetadata& metadata : assets)
        {
            Record* record = FindRecord(metadata.Handle);
            if (!record)
            {
                Record newRecord;
                newRecord.Handle = metadata.Handle;
                newRecord.Type = metadata.Type;
                newRecord.SourcePath = metadata.SourcePath;
                newRecord.ResolvedPath = AssetFileSystem::ResolvePath(metadata.SourcePath);
                newRecord.LastSnapshot = Capture(newRecord.ResolvedPath);
                m_Records.push_back(std::move(newRecord));
                continue;
            }

            record->Type = metadata.Type;
            if (record->SourcePath != metadata.SourcePath)
            {
                record->SourcePath = metadata.SourcePath;
                record->ResolvedPath = AssetFileSystem::ResolvePath(metadata.SourcePath);
                record->LastSnapshot = Capture(record->ResolvedPath);
            }
        }
    }

    std::vector<AssetWatchEvent> AssetWatcher::Poll(const AssetRegistry& registry)
    {
        SyncRegistry(registry);

        std::vector<AssetWatchEvent> events;
        for (Record& record : m_Records)
        {
            record.ResolvedPath = AssetFileSystem::ResolvePath(record.SourcePath);
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

    void AssetWatcher::Clear()
    {
        m_Records.clear();
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
        const auto it = std::find_if(m_Records.begin(), m_Records.end(), [handle](const Record& record)
        {
            return record.Handle == handle;
        });

        return it == m_Records.end() ? nullptr : &(*it);
    }
}
