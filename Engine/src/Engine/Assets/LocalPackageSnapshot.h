#pragma once

#include "Engine/Core/Base.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine
{
    struct LocalPackageSnapshotEntry
    {
        std::string RelativePath;
        u64 SizeBytes = 0;
        std::string Sha256;

        bool operator==(const LocalPackageSnapshotEntry&) const = default;
    };

    struct LocalPackageSnapshotLimits
    {
        u64 MaximumFileCount = 4096;
        u64 MaximumDepth = 16;
        u64 MaximumPathBytes = 1024;
        u64 MaximumSegmentBytes = 255;
        u64 MaximumFileBytes = 1ull * 1024ull * 1024ull * 1024ull;
        u64 MaximumAggregateBytes = 8ull * 1024ull * 1024ull * 1024ull;
        u64 MaximumGltfJsonBytes = 64ull * 1024ull * 1024ull;
    };

    enum class LocalPackageSnapshotHookPoint
    {
        Started,
        StagingCreated,
        InventoryEntry,
        InventoryComplete,
        BeforeFileCopy,
        AfterFileCopy,
        BeforeReenumeration,
        BeforeDependencyValidation,
        BeforeCommit
    };

    struct LocalPackageSnapshotProgress
    {
        LocalPackageSnapshotHookPoint Point = LocalPackageSnapshotHookPoint::Started;
        u64 FilesCompleted = 0;
        u64 FileCount = 0;
        u64 BytesCompleted = 0;
        u64 AggregateBytes = 0;
        // Valid only for the duration of the callback.
        std::string_view RelativePath;
    };

    struct LocalPackageSnapshotOptions
    {
        LocalPackageSnapshotLimits Limits;
        std::function<bool()> IsCancelled;
        std::function<void(const LocalPackageSnapshotProgress&)> Progress;

        // Deliberately test-facing race/cancellation seam. The callback receives
        // canonical relative names only; production code must not derive policy
        // from it or expose the selected source directory through it.
        std::function<void(LocalPackageSnapshotHookPoint, std::string_view)> TestHook;
    };

    class LocalPackageSnapshot
    {
    public:
        LocalPackageSnapshot() = default;
        ~LocalPackageSnapshot();

        LocalPackageSnapshot(const LocalPackageSnapshot&) = delete;
        LocalPackageSnapshot& operator=(const LocalPackageSnapshot&) = delete;
        LocalPackageSnapshot(LocalPackageSnapshot&& other) noexcept;
        LocalPackageSnapshot& operator=(LocalPackageSnapshot&& other) noexcept;

        static bool Create(
            const std::filesystem::path& sourceDirectory,
            const std::filesystem::path& privateStagingParent,
            const LocalPackageSnapshotOptions& options,
            LocalPackageSnapshot& snapshot,
            std::string& error);

        bool IsValid() const { return !m_Directory.empty(); }
        const std::filesystem::path& GetDirectory() const { return m_Directory; }
        const std::vector<LocalPackageSnapshotEntry>& GetEntries() const { return m_Entries; }
        const std::string& GetTreeSha256() const { return m_TreeSha256; }
        const std::string& GetRootRelativePath() const { return m_RootRelativePath; }
        std::filesystem::path GetRootPath() const { return m_Directory / m_RootRelativePath; }

    private:
        void Reset() noexcept;

        std::filesystem::path m_Directory;
        std::vector<LocalPackageSnapshotEntry> m_Entries;
        std::string m_TreeSha256;
        std::string m_RootRelativePath;
        u64 m_DirectoryDevice = 0;
        u64 m_DirectoryInode = 0;
        int m_StagingParentDescriptor = -1;
        int m_DirectoryDescriptor = -1;
        std::string m_StagingName;
    };
}
