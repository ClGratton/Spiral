#pragma once

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Assets/AssetRegistry.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Engine
{
    enum class AssetWatchEventType
    {
        Modified,
        Deleted,
        Restored
    };

    const char* ToString(AssetWatchEventType type);

    struct AssetWatchEvent
    {
        AssetHandle Handle = kInvalidAssetHandle;
        AssetType Type = AssetType::Unknown;
        AssetWatchEventType EventType = AssetWatchEventType::Modified;
        std::string SourcePath;
        std::filesystem::path ResolvedPath;
    };

    class AssetWatcher
    {
    public:
        void SyncRegistry(const AssetRegistry& registry);
        std::vector<AssetWatchEvent> Poll(const AssetRegistry& registry);
        void Clear();

        std::size_t GetTrackedCount() const { return m_Records.size(); }
        std::size_t GetMissingCount() const;

    private:
        struct Snapshot
        {
            bool Exists = false;
            std::filesystem::file_time_type LastWriteTime {};
            std::uintmax_t Size = 0;
        };

        struct Record
        {
            AssetHandle Handle = kInvalidAssetHandle;
            AssetType Type = AssetType::Unknown;
            std::string SourcePath;
            std::filesystem::path ResolvedPath;
            Snapshot LastSnapshot;
        };

        static Snapshot Capture(const std::filesystem::path& path);
        static bool IsDifferent(const Snapshot& previous, const Snapshot& current);
        Record* FindRecord(AssetHandle handle);

    private:
        std::vector<Record> m_Records;
    };
}
