#pragma once

#include "Engine/Assets/FabArchive.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace Engine
{
    // Owner-only staging for hostile package bytes (Linux; every other platform
    // fails closed with an explicit unsupported error and creates nothing).
    //
    // A FabStagingDirectory is a mode-0700 directory this object created through
    // descriptor-relative mkdirat/openat with O_NOFOLLOW. It retains the parent
    // and directory descriptors plus the device/inode identity, and Remove (and
    // the destructor) delete only the exact tree it created: when the name no
    // longer resolves to the same inode, the replacement is left alone.
    bool IsFabStagingSupported();

    // Creates every missing component of `root` with mode 0700 and requires the
    // final directory to be a real directory owned by the effective user with no
    // group/other access. An existing looser directory is refused, never chmod-ed.
    bool PrepareFabStagingRoot(const std::filesystem::path& root, std::string& error);

    struct FabStagedCopy
    {
        u64 Bytes = 0;
        std::string Sha256;
    };

    class FabStagingDirectory
    {
    public:
        FabStagingDirectory() = default;
        ~FabStagingDirectory();

        FabStagingDirectory(const FabStagingDirectory&) = delete;
        FabStagingDirectory& operator=(const FabStagingDirectory&) = delete;
        FabStagingDirectory(FabStagingDirectory&& other) noexcept;
        FabStagingDirectory& operator=(FabStagingDirectory&& other) noexcept;

        // `root` must satisfy PrepareFabStagingRoot. The new directory is named
        // "<prefix>-<random>" and is guaranteed not to have existed.
        static bool Create(const std::filesystem::path& root, std::string_view prefix,
            FabStagingDirectory& out, std::string& error);

        bool IsValid() const { return !m_Path.empty(); }
        // Diagnostic/convenience path only; the retained descriptors are the authority.
        const std::filesystem::path& GetPath() const { return m_Path; }

        // Creates a mode-0700 child with exactly this single-segment name; an
        // existing entry is an error.
        bool CreateChild(std::string_view name, FabStagingDirectory& out, std::string& error) const;

        // Removes one direct child (file or directory tree) this directory owns.
        // A missing child is success. Entries are removed by descriptor-relative
        // unlink without following links.
        bool RemoveChild(std::string_view name, std::string& error) const;

        // Removes exactly the tree created by this object and invalidates it. A
        // replaced or already-removed directory is left alone and is not an error.
        bool Remove(std::string& error);

    private:
        friend class FabZipStagingSink;
        friend bool CopyRegularFileIntoStaging(const std::filesystem::path&, const FabStagingDirectory&,
            std::string_view, u64, const std::function<bool()>&, FabStagedCopy&, std::string&);
        void Reset() noexcept;

        std::filesystem::path m_Path;
        std::string m_Name;
        int m_ParentDescriptor = -1;
        int m_Descriptor = -1;
        u64 m_Device = 0;
        u64 m_Inode = 0;
    };

    // Copies one regular file into `destination` as the single-segment `name`
    // (created with O_EXCL, mode 0600): the source is opened O_NOFOLLOW, must be
    // a regular file no larger than `maximumBytes`, is read once through that
    // descriptor, and must not change size while it is copied. The SHA-256 is of
    // the exact copied bytes, so every later read of the staged copy is of
    // verified content and not of a re-openable user path. The partial copy is
    // removed on failure or cancellation (cancel returns false with error
    // "cancelled").
    bool CopyRegularFileIntoStaging(const std::filesystem::path& source, const FabStagingDirectory& destination,
        std::string_view name, u64 maximumBytes, const std::function<bool()>& isCancelled,
        FabStagedCopy& out, std::string& error);

    // Receives the validated canonical members of ExtractFabZip and writes them
    // beneath `destination` with O_CREAT|O_EXCL|O_NOFOLLOW, mode 0600, creating
    // mode-0700 parent directories descriptor-relatively. Regular files only,
    // no overwrite, exact declared size (more or fewer bytes fail), fsync before
    // a member is committed, and the partial member is removed by exact
    // device/inode on AbortFile. Members committed earlier are discarded by
    // removing the destination. `isCancelled` is polled at every member and
    // every write.
    class FabZipStagingSink final : public FabArchiveSink
    {
    public:
        explicit FabZipStagingSink(const FabStagingDirectory& destination, std::function<bool()> isCancelled = {});
        ~FabZipStagingSink() override;

        FabZipStagingSink(const FabZipStagingSink&) = delete;
        FabZipStagingSink& operator=(const FabZipStagingSink&) = delete;

        u64 CommittedFileCount() const { return m_CommittedFiles; }
        u64 CommittedBytes() const { return m_CommittedBytes; }

        bool BeginFile(std::string_view relativePath, u64 size) override;
        bool Write(std::span<const u8> bytes) override;
        bool EndFile() override;
        void AbortFile() override;

    private:
        void DiscardCurrent() noexcept;

        int m_RootDescriptor = -1;
        std::function<bool()> m_IsCancelled;
        int m_FileDescriptor = -1;
        int m_ParentDescriptor = -1;
        std::string m_Leaf;
        u64 m_Device = 0;
        u64 m_Inode = 0;
        u64 m_Declared = 0;
        u64 m_Written = 0;
        u64 m_CommittedFiles = 0;
        u64 m_CommittedBytes = 0;
    };

    // ZIP -> staging step of the import flow: validates and extracts `input` into
    // `destination` through FabZipStagingSink. On failure the caller removes the
    // destination; nothing outside it is touched. Cancellation returns false
    // with error "cancelled".
    bool ExtractFabZipToStaging(const FabArchiveInput& input, const FabArchiveLimits& limits,
        const FabStagingDirectory& destination, const std::function<bool()>& isCancelled,
        u64* outFileCount, u64* outBytes, std::string& error);
}
