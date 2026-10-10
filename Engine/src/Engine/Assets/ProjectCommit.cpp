#include "Engine/Assets/ProjectCommit.h"

#include "Engine/Assets/ProjectManifest.h"
#include "Engine/Core/AtomicFile.h"
#include "Engine/Core/Sha256.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <system_error>
#include <utility>

#if defined(GE_PLATFORM_LINUX)
    #include <fcntl.h>
    #include <sys/file.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

namespace Engine
{
    namespace
    {
        bool IsLowerSha256Hex(std::string_view text)
        {
            return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char character)
            {
                return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
            });
        }

        std::string AsciiLower(std::string text)
        {
            for (char& character : text)
                if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character - 'A' + 'a');
            return text;
        }

        constexpr size_t kHashChunkBytes = 1u << 20;
    }

    namespace
    {
        enum class VerifyStatus
        {
            Verified,
            Mismatch,
            Cancelled
        };

        VerifyStatus VerifyGenerationDirectoryImpl(const std::filesystem::path& directory,
            const std::vector<ProjectCommitArtifact>& artifacts, bool hashContents,
            const std::function<bool()>& isCancelled, std::string& outError)
        {
            const auto mismatch = [&outError](std::string message)
            {
                outError = std::move(message);
                return VerifyStatus::Mismatch;
            };

            std::map<std::string, std::string> expectedFiles;
            std::set<std::string> expectedDirectories;
            std::set<std::string> foldedNames;
            for (const ProjectCommitArtifact& artifact : artifacts)
            {
                if (!IsPortableProjectRelativePath(artifact.RelativePath) || !IsLowerSha256Hex(artifact.Sha256)
                    || !foldedNames.insert(AsciiLower(artifact.RelativePath)).second)
                    return mismatch("the generation artifact list is malformed or has duplicate names");
                expectedFiles.emplace(artifact.RelativePath, artifact.Sha256);
                for (size_t slash = artifact.RelativePath.find('/'); slash != std::string::npos;
                    slash = artifact.RelativePath.find('/', slash + 1))
                    expectedDirectories.insert(artifact.RelativePath.substr(0, slash));
            }

            std::error_code filesystemError;
            const std::filesystem::file_status rootStatus
                = std::filesystem::symlink_status(directory, filesystemError);
            if (filesystemError || !std::filesystem::is_directory(rootStatus))
                return mismatch("the generation directory is missing or is not a real directory");

            std::set<std::string> found;
            std::vector<std::pair<std::filesystem::path, std::string>> pending { { directory, std::string() } };
            while (!pending.empty())
            {
                const auto [path, prefix] = pending.back();
                pending.pop_back();
                std::filesystem::directory_iterator iterator(path, filesystemError);
                if (filesystemError)
                    return mismatch("could not enumerate the generation directory");
                for (const std::filesystem::directory_entry& entry : iterator)
                {
                    const std::string relative = prefix + entry.path().filename().generic_string();
                    const std::filesystem::file_status status = entry.symlink_status(filesystemError);
                    if (filesystemError)
                        return mismatch("could not inspect a generation entry");
                    if (std::filesystem::is_directory(status) && expectedDirectories.contains(relative))
                        pending.emplace_back(entry.path(), relative + "/");
                    else if (std::filesystem::is_regular_file(status) && expectedFiles.contains(relative))
                        found.insert(relative);
                    else
                        return mismatch("the generation holds an unexpected entry: " + relative);
                }
            }
            for (const auto& [relative, expectedHash] : expectedFiles)
            {
                (void)expectedHash;
                if (!found.contains(relative))
                    return mismatch("the generation is missing an artifact: " + relative);
            }

            if (!hashContents)
                return VerifyStatus::Verified;

            const auto cancelled = [&outError]()
            {
                outError = "generation verification was cancelled";
                return VerifyStatus::Cancelled;
            };
            std::vector<char> buffer(kHashChunkBytes);
            for (const auto& [relative, expectedHash] : expectedFiles)
            {
                if (isCancelled && isCancelled())
                    return cancelled();
                std::ifstream input(directory / std::filesystem::path(relative), std::ios::in | std::ios::binary);
                if (!input)
                    return mismatch("could not open a generation artifact: " + relative);
                Sha256Builder hash;
                while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || input.gcount() > 0)
                {
                    hash.Update(std::span<const u8>(
                        reinterpret_cast<const u8*>(buffer.data()), static_cast<size_t>(input.gcount())));
                    if (isCancelled && isCancelled())
                        return cancelled();
                }
                if (input.bad() || hash.FinalizeHex() != expectedHash)
                    return mismatch("a generation artifact does not match its recorded SHA-256: " + relative);
            }
            return VerifyStatus::Verified;
        }
    }

    bool VerifyGenerationDirectory(const std::filesystem::path& directory,
        const std::vector<ProjectCommitArtifact>& artifacts, bool hashContents,
        const std::function<bool()>& isCancelled, std::string& outError)
    {
        return VerifyGenerationDirectoryImpl(directory, artifacts, hashContents, isCancelled, outError)
            == VerifyStatus::Verified;
    }

#if defined(GE_PLATFORM_LINUX)
    namespace
    {
        std::vector<std::string> SplitSegments(std::string_view path)
        {
            std::vector<std::string> segments;
            size_t start = 0;
            while (start <= path.size())
            {
                const size_t separator = path.find('/', start);
                const size_t end = separator == std::string_view::npos ? path.size() : separator;
                segments.emplace_back(path.substr(start, end - start));
                if (separator == std::string_view::npos)
                    break;
                start = separator + 1;
            }
            return segments;
        }

        bool IsUnder(std::string_view path, std::string_view directory)
        {
            return path.size() > directory.size() && path.starts_with(directory)
                && path[directory.size()] == '/';
        }

        class Descriptor
        {
        public:
            Descriptor() = default;
            explicit Descriptor(int fd) : m_Fd(fd) {}
            ~Descriptor() { Reset(); }
            Descriptor(const Descriptor&) = delete;
            Descriptor& operator=(const Descriptor&) = delete;
            Descriptor(Descriptor&& other) noexcept : m_Fd(std::exchange(other.m_Fd, -1)) {}
            Descriptor& operator=(Descriptor&& other) noexcept
            {
                if (this != &other)
                {
                    Reset();
                    m_Fd = std::exchange(other.m_Fd, -1);
                }
                return *this;
            }
            int Get() const { return m_Fd; }
            bool IsOpen() const { return m_Fd >= 0; }
            void Reset(int fd = -1)
            {
                if (m_Fd >= 0)
                    close(m_Fd);
                m_Fd = fd;
            }

        private:
            int m_Fd = -1;
        };

        struct CreatedEntry
        {
            std::vector<std::string> Parents;  // directory chain from the project root
            std::string Name;
            dev_t Device = 0;
            ino_t Inode = 0;
        };

        // Opens a directory chain beneath `root` one component at a time without
        // following links. With `created` (and create=true) missing directories
        // are made and recorded so a failed transaction can remove them again.
        Descriptor OpenChain(int root, const std::vector<std::string>& segments, size_t count,
            bool create, std::vector<CreatedEntry>* created)
        {
            Descriptor current(dup(root));
            for (size_t index = 0; index < count && current.IsOpen(); ++index)
            {
                const char* name = segments[index].c_str();
                constexpr int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
                int next = openat(current.Get(), name, flags);
                if (next < 0 && errno == ENOENT && create)
                {
                    if (mkdirat(current.Get(), name, 0755) == 0)
                    {
                        next = openat(current.Get(), name, flags);
                        // The new directory's own entry lives in its parent: syncing both orders the
                        // whole created chain before the manifest rename that makes it reachable.
                        // A filesystem that journals directories independently (btrfs, f2fs) could
                        // otherwise lose "fab/" while keeping a manifest that names it.
                        if (next >= 0)
                            (void)fsync(next);
                        (void)fsync(current.Get());
                        struct stat made {};
                        if (next >= 0 && created && fstat(next, &made) == 0)
                        {
                            CreatedEntry entry;
                            entry.Parents.assign(segments.begin(), segments.begin() + static_cast<std::ptrdiff_t>(index));
                            entry.Name = segments[index];
                            entry.Device = made.st_dev;
                            entry.Inode = made.st_ino;
                            created->push_back(std::move(entry));
                        }
                    }
                    else if (errno == EEXIST)
                        next = openat(current.Get(), name, flags);
                }
                const int savedErrno = errno;
                current.Reset(next);
                errno = savedErrno;
            }
            return current;
        }

        bool WriteAll(int fd, std::string_view bytes)
        {
            size_t offset = 0;
            while (offset < bytes.size())
            {
                const ssize_t written = write(fd, bytes.data() + offset, bytes.size() - offset);
                if (written > 0)
                {
                    offset += static_cast<size_t>(written);
                    continue;
                }
                if (written < 0 && errno == EINTR)
                    continue;
                return false;
            }
            return true;
        }

        void SyncTree(const std::filesystem::path& directory)
        {
            std::error_code filesystemError;
            std::vector<std::filesystem::path> directories { directory };
            for (std::filesystem::recursive_directory_iterator iterator(directory, filesystemError), end;
                !filesystemError && iterator != end; iterator.increment(filesystemError))
            {
                const std::filesystem::file_status status = iterator->symlink_status(filesystemError);
                if (filesystemError)
                    break;
                if (std::filesystem::is_directory(status))
                    directories.push_back(iterator->path());
                else if (std::filesystem::is_regular_file(status))
                {
                    const int fd = open(iterator->path().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
                    if (fd >= 0)
                    {
                        (void)fsync(fd);
                        close(fd);
                    }
                }
            }
            for (auto it = directories.rbegin(); it != directories.rend(); ++it)
            {
                const int fd = open(it->c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (fd >= 0)
                {
                    (void)fsync(fd);
                    close(fd);
                }
            }
        }

        class CommitTransaction
        {
        public:
            explicit CommitTransaction(const ProjectCommitRequest& request)
                : m_Request(request) {}

            ProjectCommitResult Run()
            {
                if (!ValidateRequest())
                    return m_Result;

                m_Root.Reset(open(m_Request.ProjectRoot.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
                if (!m_Root.IsOpen())
                    return Abort(ProjectCommitError::IoFailure, "could not open the project root");
                if (!Step(ProjectCommitHook::Begin, {}))
                    return Abort();
                if (flock(m_Root.Get(), LOCK_EX | LOCK_NB) != 0)
                    return Abort(ProjectCommitError::LockUnavailable,
                        "another project commit holds the project lock");
                if (!Step(ProjectCommitHook::Locked, {}) || !VerifyBase()
                    || !Step(ProjectCommitHook::BaseVerified, {}))
                    return Abort();

                if (m_Request.Generation && !PublishGeneration())
                    return Abort();

                for (const ProjectCommitFile& file : m_Request.RevisionFiles)
                    if (!WriteRevisionFile(file))
                        return Abort();

                if (!CheckPointerTargets() || !Step(ProjectCommitHook::BeforePointer, {})
                    || !CheckNotCancelled())
                    return Abort();

                bool directoryDurable = false;
                std::string writeError;
                if (!WriteFileAtomically(m_Request.ProjectRoot / std::filesystem::path(m_Request.ManifestRelativePath),
                        m_Request.ManifestBytes, writeError, &directoryDurable))
                    return Abort(ProjectCommitError::IoFailure,
                        "could not atomically replace the project manifest: " + writeError);

                // Irrevocable from here. Nothing below may report a rollback.
                m_Result.Outcome = directoryDurable ? ProjectCommitOutcome::Committed
                                                    : ProjectCommitOutcome::CommittedDurabilityUnconfirmed;
                m_Result.Error = ProjectCommitError::None;
                m_Result.Message.clear();
                m_Result.CommittedManifestSha256 = Sha256Builder::HashString(m_Request.ManifestBytes);
                m_Result.UndoBarrier = { true, m_PreviousRevision, m_NewManifest.ProjectRevision };

                if (m_Request.TestHook
                    && m_Request.TestHook(ProjectCommitHook::AfterPointer, {})
                        == ProjectCommitHookAction::SimulateCrash)
                    return m_Result;

                std::string reloadError;
                if (m_Request.ValidateCommitted && !m_Request.ValidateCommitted(reloadError))
                {
                    m_Result.Outcome = ProjectCommitOutcome::CommittedRecoveryRequired;
                    m_Result.Error = ProjectCommitError::ReloadValidationFailed;
                    m_Result.Message = "the committed project failed reload validation: " + reloadError;
                }
                return m_Result;
            }

        private:
            ProjectCommitResult Abort(ProjectCommitError error = ProjectCommitError::None,
                std::string message = {})
            {
                if (error != ProjectCommitError::None)
                    Record(error, std::move(message));
                if (!m_Crashed)
                    RemoveCreatedEntries();
                m_Result.Outcome = m_Result.Error == ProjectCommitError::Cancelled
                    ? ProjectCommitOutcome::Cancelled : ProjectCommitOutcome::NotCommitted;
                return m_Result;
            }

            bool Record(ProjectCommitError error, std::string message)
            {
                if (m_Result.Error == ProjectCommitError::None)
                {
                    m_Result.Error = error;
                    m_Result.Message = std::move(message);
                }
                return false;
            }

            bool Step(ProjectCommitHook point, std::string_view detail)
            {
                if (!m_Request.TestHook)
                    return true;
                switch (m_Request.TestHook(point, detail))
                {
                    case ProjectCommitHookAction::Continue:
                        return true;
                    case ProjectCommitHookAction::Cancel:
                        return Record(ProjectCommitError::Cancelled, "the commit was cancelled");
                    case ProjectCommitHookAction::Fail:
                        return Record(ProjectCommitError::InjectedFailure, "injected failure");
                    case ProjectCommitHookAction::SimulateCrash:
                        m_Crashed = true;
                        return Record(ProjectCommitError::InjectedFailure, "simulated crash");
                }
                return true;
            }

            bool CheckNotCancelled()
            {
                if (m_Request.IsCancelled && m_Request.IsCancelled())
                    return Record(ProjectCommitError::Cancelled, "the commit was cancelled");
                return true;
            }

            bool ValidateRequest()
            {
                const auto invalid = [this](ProjectCommitError error, std::string message)
                {
                    Record(error, std::move(message));
                    return false;
                };

                std::error_code filesystemError;
                if (m_Request.ProjectRoot.empty()
                    || !std::filesystem::is_directory(m_Request.ProjectRoot, filesystemError))
                    return invalid(ProjectCommitError::InvalidRequest, "the project root is not a directory");
                if (!IsPortableProjectRelativePath(m_Request.ManifestRelativePath))
                    return invalid(ProjectCommitError::PathEscape, "the manifest path is not a portable project-relative path");
                std::string parseError;
                if (!DeserializeProjectManifest(m_Request.ManifestBytes, m_NewManifest, parseError))
                    return invalid(ProjectCommitError::InvalidManifest, "the candidate manifest is invalid: " + parseError);
                if (!m_Request.ExpectedManifestSha256.empty() && !IsLowerSha256Hex(m_Request.ExpectedManifestSha256))
                    return invalid(ProjectCommitError::InvalidRequest, "the expected manifest hash is malformed");

                std::set<std::string> names { AsciiLower(m_Request.ManifestRelativePath) };
                std::string generationRoot;
                if (m_Request.Generation)
                {
                    const ProjectCommitGeneration& generation = *m_Request.Generation;
                    if (!IsPortableProjectRelativePath(generation.RelativeRoot))
                        return invalid(ProjectCommitError::PathEscape, "the generation root is not a portable project-relative path");
                    if (generation.Artifacts.empty())
                        return invalid(ProjectCommitError::InvalidRequest, "the generation lists no artifacts");
                    for (const ProjectCommitArtifact& artifact : generation.Artifacts)
                        if (!IsPortableProjectRelativePath(artifact.RelativePath) || !IsLowerSha256Hex(artifact.Sha256))
                            return invalid(ProjectCommitError::InvalidRequest, "a generation artifact entry is malformed");
                    generationRoot = AsciiLower(generation.RelativeRoot);
                    if (!names.insert(generationRoot).second || IsUnder(AsciiLower(m_Request.ManifestRelativePath), generationRoot))
                        return invalid(ProjectCommitError::InvalidRequest, "the manifest lies inside the generation");
                }
                for (const ProjectCommitFile& file : m_Request.RevisionFiles)
                {
                    if (!IsPortableProjectRelativePath(file.RelativePath))
                        return invalid(ProjectCommitError::PathEscape, "a revision file path is not a portable project-relative path");
                    const std::string folded = AsciiLower(file.RelativePath);
                    if (!names.insert(folded).second
                        || (!generationRoot.empty() && IsUnder(folded, generationRoot)))
                        return invalid(ProjectCommitError::InvalidRequest, "revision file paths collide with each other, the manifest, or the generation");
                }
                m_ManifestSegments = SplitSegments(m_Request.ManifestRelativePath);
                return true;
            }

            bool VerifyBase()
            {
                const Descriptor parent = OpenChain(
                    m_Root.Get(), m_ManifestSegments, m_ManifestSegments.size() - 1, false, nullptr);
                std::string current;
                bool exists = false;
                if (parent.IsOpen())
                {
                    const int fd = openat(parent.Get(), m_ManifestSegments.back().c_str(),
                        O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
                    if (fd >= 0)
                    {
                        const Descriptor file(fd);
                        struct stat status {};
                        if (fstat(file.Get(), &status) != 0 || !S_ISREG(status.st_mode)
                            || static_cast<u64>(status.st_size) > kMaximumProjectManifestBytes)
                            return Record(ProjectCommitError::InvalidManifest, "the current manifest is not a bounded regular file");
                        current.resize(static_cast<size_t>(status.st_size));
                        size_t offset = 0;
                        while (offset < current.size())
                        {
                            const ssize_t count = read(file.Get(), current.data() + offset, current.size() - offset);
                            if (count > 0)
                                offset += static_cast<size_t>(count);
                            else if (count < 0 && errno == EINTR)
                                continue;
                            else
                                return Record(ProjectCommitError::IoFailure, "could not read the current manifest");
                        }
                        exists = true;
                    }
                    else if (errno == ELOOP)
                        return Record(ProjectCommitError::PathEscape, "the manifest is a symbolic link");
                    else if (errno != ENOENT)
                        return Record(ProjectCommitError::IoFailure, "could not open the current manifest");
                }
                else if (errno == ELOOP || errno == ENOTDIR)
                    return Record(ProjectCommitError::PathEscape, "the manifest path crosses a link or a non-directory");
                else if (errno != ENOENT)
                    return Record(ProjectCommitError::IoFailure, "could not open the manifest directory");

                const std::string currentHash = exists ? Sha256Builder::HashString(current) : std::string();
                if (currentHash != m_Request.ExpectedManifestSha256)
                    return Record(ProjectCommitError::BaseChanged,
                        "the project manifest changed since the candidate was built");

                m_PreviousRevision = 0;
                if (exists)
                {
                    ProjectManifest base;
                    std::string parseError;
                    if (!DeserializeProjectManifest(current, base, parseError))
                        return Record(ProjectCommitError::InvalidManifest,
                            "the current project manifest is unparsable and is not repaired: " + parseError);
                    m_PreviousRevision = base.ProjectRevision;
                }
                if (m_NewManifest.ProjectRevision != m_PreviousRevision + 1)
                    return Record(ProjectCommitError::InvalidManifest,
                        "the candidate ProjectRevision must be exactly one greater than the current revision");
                return true;
            }

            bool PublishGeneration()
            {
                const ProjectCommitGeneration& generation = *m_Request.Generation;
                const std::vector<std::string> segments = SplitSegments(generation.RelativeRoot);
                const Descriptor parent = OpenChain(m_Root.Get(), segments, segments.size() - 1, false, nullptr);
                const int parentError = errno;
                if (!parent.IsOpen() && parentError != ENOENT)
                    return Record(parentError == ELOOP || parentError == ENOTDIR ? ProjectCommitError::PathEscape
                                                                                   : ProjectCommitError::IoFailure,
                        "the generation parent is not a real directory chain");
                const std::filesystem::path finalPath
                    = m_Request.ProjectRoot / std::filesystem::path(generation.RelativeRoot);

                struct stat existing {};
                bool exists = parent.IsOpen()
                    && fstatat(parent.Get(), segments.back().c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0;
                if (parent.IsOpen() && !exists && errno != ENOENT)
                    return Record(ProjectCommitError::IoFailure, "could not inspect the generation location");
                if (exists && !S_ISDIR(existing.st_mode))
                    return Record(ProjectCommitError::GenerationMismatch, "the generation location is not a real directory");

                std::string verifyError;
                if (!exists)
                {
                    if (generation.StagedDirectory.empty())
                        return Record(ProjectCommitError::GenerationMissing,
                            "the generation does not exist and no staged directory was supplied");
                    if (!CheckNotCancelled())
                        return false;
                    const VerifyStatus stagedStatus = VerifyGenerationDirectoryImpl(
                        generation.StagedDirectory, generation.Artifacts, true, m_Request.IsCancelled, verifyError);
                    if (stagedStatus != VerifyStatus::Verified)
                        return Record(stagedStatus == VerifyStatus::Cancelled ? ProjectCommitError::Cancelled
                                                                              : ProjectCommitError::GenerationMismatch,
                            "the staged generation failed verification: " + verifyError);
                    if (!Step(ProjectCommitHook::GenerationVerified, "staged") || !CheckNotCancelled())
                        return false;
                    SyncTree(generation.StagedDirectory);

                    // Missing parents are made without following links, so a link
                    // cannot redirect the no-replace rename outside the project.
                    const Descriptor ensured = OpenChain(m_Root.Get(), segments, segments.size() - 1, true, &m_Created);
                    if (!ensured.IsOpen())
                    {
                        const int ensureError = errno;
                        return Record(ensureError == ELOOP || ensureError == ENOTDIR ? ProjectCommitError::PathEscape
                                                                                     : ProjectCommitError::IoFailure,
                            "the generation parent is not a real directory chain");
                    }

                    std::string publishError;
                    switch (PublishDirectoryNoReplace(generation.StagedDirectory, finalPath, publishError))
                    {
                        case DirectoryPublishStatus::Published:
                        case DirectoryPublishStatus::PublishedDurabilityUnconfirmed:
                            m_Result.Generation = ProjectCommitGenerationDisposition::Published;
                            return Step(ProjectCommitHook::GenerationPublished, {});
                        case DirectoryPublishStatus::AlreadyExists:
                            exists = true;  // lost a race to another publisher: adopt-or-fail below
                            break;
                        case DirectoryPublishStatus::Unsupported:
                            return Record(ProjectCommitError::Unsupported, publishError);
                        case DirectoryPublishStatus::Failed:
                            return Record(ProjectCommitError::GenerationPublishFailed, publishError);
                    }
                }

                if (!Step(ProjectCommitHook::GenerationVerified, "existing"))
                    return false;
                const VerifyStatus existingStatus = VerifyGenerationDirectoryImpl(
                    finalPath, generation.Artifacts, true, m_Request.IsCancelled, verifyError);
                if (existingStatus != VerifyStatus::Verified)
                    return Record(existingStatus == VerifyStatus::Cancelled ? ProjectCommitError::Cancelled
                                                                            : ProjectCommitError::GenerationMismatch,
                        "an existing generation does not match and is not replaced: " + verifyError);
                m_Result.Generation = ProjectCommitGenerationDisposition::AdoptedExisting;
                return Step(ProjectCommitHook::GenerationPublished, {});
            }

            bool WriteRevisionFile(const ProjectCommitFile& file)
            {
                if (!Step(ProjectCommitHook::BeforeRevisionFile, file.RelativePath) || !CheckNotCancelled())
                    return false;
                const std::vector<std::string> segments = SplitSegments(file.RelativePath);
                const Descriptor parent = OpenChain(m_Root.Get(), segments, segments.size() - 1, true, &m_Created);
                if (!parent.IsOpen())
                {
                    const int openError = errno;
                    return Record(openError == ELOOP || openError == ENOTDIR ? ProjectCommitError::PathEscape
                                                                             : ProjectCommitError::IoFailure,
                        "a revision file parent is not a real directory chain");
                }

                const int fd = openat(parent.Get(), segments.back().c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
                const int createError = errno;
                if (fd < 0)
                    return Record(createError == EEXIST ? ProjectCommitError::RevisionFileExists
                                                  : ProjectCommitError::IoFailure,
                        "could not exclusively create a revision file: " + file.RelativePath);
                Descriptor output(fd);
                struct stat created {};
                if (fstat(output.Get(), &created) == 0)
                {
                    CreatedEntry entry;
                    entry.Parents.assign(segments.begin(), segments.end() - 1);
                    entry.Name = segments.back();
                    entry.Device = created.st_dev;
                    entry.Inode = created.st_ino;
                    m_Created.push_back(std::move(entry));
                }
                if (!WriteAll(output.Get(), file.Bytes) || fsync(output.Get()) != 0)
                    return Record(ProjectCommitError::IoFailure, "could not write a revision file: " + file.RelativePath);
                output.Reset();
                (void)fsync(parent.Get());
                return Step(ProjectCommitHook::RevisionFileWritten, file.RelativePath);
            }

            bool CheckPointerTargets()
            {
                for (const std::string* path : { &m_NewManifest.ScenePath, &m_NewManifest.AssetRegistryPath,
                         &m_NewManifest.FabReceiptsPath })
                {
                    std::error_code filesystemError;
                    if (!path->empty()
                        && !std::filesystem::is_regular_file(m_Request.ProjectRoot / std::filesystem::path(*path), filesystemError))
                        return Record(ProjectCommitError::InvalidManifest,
                            "the candidate manifest names a file that does not exist: " + *path);
                }
                return true;
            }

            void RemoveCreatedEntries()
            {
                // Newest first: files before the directories created for them. Only
                // entries whose identity still matches what this transaction made.
                for (auto entry = m_Created.rbegin(); entry != m_Created.rend(); ++entry)
                {
                    const Descriptor parent = OpenChain(m_Root.Get(), entry->Parents, entry->Parents.size(), false, nullptr);
                    struct stat status {};
                    if (!parent.IsOpen()
                        || fstatat(parent.Get(), entry->Name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0
                        || status.st_dev != entry->Device || status.st_ino != entry->Inode)
                        continue;
                    (void)unlinkat(parent.Get(), entry->Name.c_str(), S_ISDIR(status.st_mode) ? AT_REMOVEDIR : 0);
                }
                m_Created.clear();
            }

            const ProjectCommitRequest& m_Request;
            ProjectManifest m_NewManifest;
            std::vector<std::string> m_ManifestSegments;
            Descriptor m_Root;
            std::vector<CreatedEntry> m_Created;
            u64 m_PreviousRevision = 0;
            bool m_Crashed = false;
            ProjectCommitResult m_Result;
        };
    }

    bool IsProjectCommitSupported()
    {
        return true;
    }

    ProjectCommitResult CommitProjectRevision(const ProjectCommitRequest& request)
    {
        return CommitTransaction(request).Run();
    }
#else
    bool IsProjectCommitSupported()
    {
        return false;
    }

    ProjectCommitResult CommitProjectRevision(const ProjectCommitRequest&)
    {
        ProjectCommitResult result;
        result.Outcome = ProjectCommitOutcome::NotCommitted;
        result.Error = ProjectCommitError::Unsupported;
        result.Message = "project commit needs no-replace directory publication and descriptor-relative "
                         "exclusive creation, which are implemented for Linux only; Windows parity is a "
                         "separately gated slice. Nothing was changed.";
        return result;
    }
#endif
}
