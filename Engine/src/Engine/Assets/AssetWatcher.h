#pragma once

#include "Engine/Assets/AssetFileSystem.h"
#include "Engine/Assets/AssetHandle.h"
#include "Engine/Assets/AssetRegistry.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
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
        // Resolves registry source paths against an explicit project root. Unset, the legacy
        // working-directory/executable-ancestor search is used. Changing the resolver drops the
        // tracked records; the next SyncRegistry or Poll rebuilds them with fresh baselines.
        void SetPathResolver(AssetPathResolver resolver);
        // Poll returns no events (and does no filesystem work) when called again within this
        // interval. Zero, the default, polls on every call.
        void SetMinimumPollInterval(std::chrono::milliseconds interval) { m_MinimumPollInterval = interval; }

        void SyncRegistry(const AssetRegistry& registry);
        std::vector<AssetWatchEvent> Poll(const AssetRegistry& registry);
        void Acknowledge(AssetHandle handle);
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
        std::filesystem::path Resolve(std::string_view sourcePath) const;
        void RebuildIndex();

    private:
        std::vector<Record> m_Records;
        // Handle -> position in m_Records, so reconciliation is linear in the asset count.
        std::unordered_map<AssetHandle, std::size_t> m_RecordIndex;
        AssetPathResolver m_PathResolver;
        std::chrono::milliseconds m_MinimumPollInterval { 0 };
        std::chrono::steady_clock::time_point m_LastPoll {};
        bool m_HasPolled = false;
    };
}
