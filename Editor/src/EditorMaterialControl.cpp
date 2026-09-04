#include "EditorMaterialControl.h"

#include <Engine/Core/Log.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <new>
#include <random>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#elif defined(__linux__)
    #include <cerrno>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <sys/syscall.h>
    #include <unistd.h>
#elif defined(__APPLE__)
    #include <unistd.h>
#endif

namespace
{
    constexpr std::string_view kRequestHeader = "SpiralEditorControlRequest 1";
    constexpr std::string_view kReceiptHeader = "SpiralEditorControlReceipt 1";
    constexpr std::string_view kSessionHeader = "SpiralEditorControlSession 1";

    const char* ToString(EditorMaterialControlAction action)
    {
        switch (action)
        {
            case EditorMaterialControlAction::InspectMaterialSurface:
                return "InspectMaterialSurface";
            case EditorMaterialControlAction::SelectEntityPatchMaterialSurface:
                return "SelectEntityPatchMaterialSurface";
        }
        return "Unknown";
    }

    bool IsStableId(std::string_view value)
    {
        if (value.empty() || value.size() > 64 || value.front() == '.')
            return false;
        return std::all_of(value.begin(), value.end(), [](char character)
        {
            return (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '-' || character == '_' || character == '.';
        });
    }

    std::string MakeSessionId()
    {
        std::random_device random;
        const Engine::u64 randomHigh =
            (static_cast<Engine::u64>(random()) << 32) ^ random();
        const Engine::u64 randomLow =
            (static_cast<Engine::u64>(random()) << 32) ^ random();
        const Engine::u64 tick = static_cast<Engine::u64>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::ostringstream stream;
        stream << "editor-" << std::hex << std::setfill('0')
               << std::setw(16) << (randomHigh ^ tick)
               << std::setw(16) << randomLow;
        return stream.str();
    }

    Engine::u64 CurrentProcessId()
    {
#if defined(_WIN32)
        return static_cast<Engine::u64>(GetCurrentProcessId());
#elif defined(__linux__) || defined(__APPLE__)
        return static_cast<Engine::u64>(::getpid());
#else
        return 0;
#endif
    }

    std::string CanonicalProjectPath(const std::filesystem::path& projectPath)
    {
        std::error_code error;
        std::filesystem::path absolute = std::filesystem::absolute(projectPath, error);
        if (error)
            return {};
        std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
        return error ? absolute.lexically_normal().string() : canonical.string();
    }

    std::string Digest(std::string_view contents)
    {
        Engine::u64 value = 14695981039346656037ull;
        for (unsigned char byte : contents)
        {
            value ^= byte;
            value *= 1099511628211ull;
        }
        std::ostringstream stream;
        stream << std::hex << std::setfill('0') << std::setw(16) << value;
        return stream.str();
    }

    bool AtEnd(std::istringstream& stream)
    {
        stream >> std::ws;
        return stream.eof();
    }

    template<typename Integer>
    bool ParseInteger(std::istringstream& stream, Integer& value)
    {
        std::string text;
        if (!(stream >> text) || !AtEnd(stream))
            return false;
        const char* begin = text.data();
        const char* end = text.data() + text.size();
        const auto result = std::from_chars(begin, end, value);
        return result.ec == std::errc {} && result.ptr == end;
    }

    bool ParseQuoted(std::istringstream& stream, std::string& value)
    {
        return static_cast<bool>(stream >> std::quoted(value)) && AtEnd(stream);
    }

    bool ParseSurface(std::istringstream& stream, Engine::MaterialSurface& surface)
    {
        return static_cast<bool>(stream >> surface.BaseColor.X >> surface.BaseColor.Y
            >> surface.BaseColor.Z >> surface.Metallic >> surface.Roughness)
            && AtEnd(stream);
    }

    [[maybe_unused]] bool HasOwnerOnlyPermissions(std::filesystem::perms permissions)
    {
        using Perms = std::filesystem::perms;
        const Perms publicBits = Perms::group_all | Perms::others_all;
        return (permissions & publicBits) == Perms::none;
    }

    bool CreatePrivateDirectory(const std::filesystem::path& path, std::string& error)
    {
#if defined(__linux__)
        if (::mkdir(path.c_str(), 0700) != 0)
        {
            error = errno == EEXIST
                ? "directory_already_exists" : "could_not_create_private_directory";
            return false;
        }
        struct stat status {};
        if (::lstat(path.c_str(), &status) != 0
            || !S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode)
            || status.st_uid != ::geteuid() || (status.st_mode & 0777) != 0700)
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            error = "private_directory_revalidation_failed";
            return false;
        }
        return true;
#else
        std::error_code filesystemError;
        if (!std::filesystem::create_directory(path, filesystemError)
            || filesystemError)
        {
            error = "could_not_create_private_directory";
            return false;
        }
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace, filesystemError);
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(path, filesystemError);
        if (filesystemError || !std::filesystem::is_directory(status)
            || std::filesystem::is_symlink(status)
            || !HasOwnerOnlyPermissions(status.permissions()))
        {
            std::filesystem::remove(path, filesystemError);
            error = "private_directory_revalidation_failed";
            return false;
        }
        return true;
#endif
    }

    bool SyncContainingDirectory(const std::filesystem::path& path)
    {
#if defined(__linux__)
        const int descriptor = ::open(path.parent_path().c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor < 0)
            return false;
        const bool synced = ::fsync(descriptor) == 0;
        const bool closed = ::close(descriptor) == 0;
        return synced && closed;
#else
        (void)path;
        return true;
#endif
    }

    bool WriteOwnerOnlyTemporary(const std::filesystem::path& path,
        std::string_view contents, std::string& error)
    {
#if defined(_WIN32)
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = "could_not_create_temporary";
            return false;
        }
        std::size_t offset = 0;
        bool written = true;
        while (offset < contents.size())
        {
            const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
                contents.size() - offset, std::numeric_limits<DWORD>::max()));
            DWORD completed = 0;
            if (!WriteFile(file, contents.data() + offset, requested, &completed, nullptr)
                || completed == 0)
            {
                written = false;
                break;
            }
            offset += completed;
        }
        written = written && FlushFileBuffers(file) != 0;
        CloseHandle(file);
        if (!written)
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            error = "could_not_write_temporary";
            return false;
        }
#elif defined(__linux__)
        const int descriptor = ::open(path.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor < 0)
        {
            error = "could_not_create_temporary";
            return false;
        }
        std::size_t offset = 0;
        bool written = true;
        while (offset < contents.size())
        {
            const ssize_t completed = ::write(descriptor,
                contents.data() + offset, contents.size() - offset);
            if (completed < 0 && errno == EINTR)
                continue;
            if (completed <= 0)
            {
                written = false;
                break;
            }
            offset += static_cast<std::size_t>(completed);
        }
        written = written && ::fsync(descriptor) == 0;
        written = ::close(descriptor) == 0 && written;
        if (!written)
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            error = "could_not_write_temporary";
            return false;
        }
#else
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            error = "could_not_create_temporary";
            return false;
        }
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        output.close();
        if (!output)
        {
            std::filesystem::remove(path);
            error = "could_not_write_temporary";
            return false;
        }
#endif
        std::error_code permissionError;
        std::filesystem::permissions(path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, permissionError);
        if (permissionError)
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            error = "could_not_set_owner_only_permissions";
            return false;
        }
        return true;
    }

    enum class PublishNoReplaceResult
    {
        Failed,
        PublishedDurable,
        PublishedVisibilityOnly
    };

    PublishNoReplaceResult PublishNoReplace(const std::filesystem::path& temporary,
        const std::filesystem::path& destination,
        bool forceParentDirectorySyncFailure, std::string& error)
    {
        error.clear();
#if defined(_WIN32)
        const bool published = MoveFileExW(temporary.c_str(), destination.c_str(),
            MOVEFILE_WRITE_THROUGH) != 0;
        const DWORD nativeError = published ? ERROR_SUCCESS : GetLastError();
        const bool collision = nativeError == ERROR_FILE_EXISTS
            || nativeError == ERROR_ALREADY_EXISTS;
#elif defined(__linux__)
        constexpr unsigned int renameNoReplace = 1;
        const bool published = ::syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(),
            AT_FDCWD, destination.c_str(), renameNoReplace) == 0;
        const int nativeError = published ? 0 : errno;
        const bool collision = nativeError == EEXIST;
#else
        std::error_code linkError;
        std::filesystem::create_hard_link(temporary, destination, linkError);
        const bool published = !linkError;
        const bool collision = linkError
            && std::filesystem::exists(destination);
#endif
        if (!published)
        {
            std::error_code removeError;
            std::filesystem::remove(temporary, removeError);
            error = collision ? "destination_collision" : "atomic_rename_failed";
            return PublishNoReplaceResult::Failed;
        }
        if (forceParentDirectorySyncFailure || !SyncContainingDirectory(destination))
        {
            // Rename is the visibility commit point. A reader may already have
            // consumed this file, so a later parent-directory fsync failure can
            // only degrade crash durability; it must never erase visible success.
            error = "published_without_parent_directory_sync";
            return PublishNoReplaceResult::PublishedVisibilityOnly;
        }
        return PublishNoReplaceResult::PublishedDurable;
    }

    bool ReadOwnerOnlyRegularFile(const std::filesystem::path& path,
        std::size_t maximumBytes, std::string& contents, std::string& rejection)
    {
#if defined(__linux__)
        const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor < 0)
        {
            rejection = errno == ELOOP
                ? "symlink_request_rejected" : "request_read_failed";
            return false;
        }
        struct stat status {};
        if (::fstat(descriptor, &status) != 0)
        {
            ::close(descriptor);
            rejection = "request_read_failed";
            return false;
        }
        if (!S_ISREG(status.st_mode))
        {
            ::close(descriptor);
            rejection = "non_regular_request_rejected";
            return false;
        }
        if (status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0)
        {
            ::close(descriptor);
            rejection = "request_permissions_not_owner_only";
            return false;
        }
        if (status.st_size < 0
            || static_cast<std::uintmax_t>(status.st_size) > maximumBytes)
        {
            ::close(descriptor);
            rejection = "oversized_request_rejected";
            return false;
        }
        contents.clear();
        contents.reserve(static_cast<std::size_t>(status.st_size));
        std::array<char, 4096> buffer {};
        while (contents.size() <= maximumBytes)
        {
            const ssize_t count = ::read(descriptor, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0)
            {
                ::close(descriptor);
                rejection = "request_read_failed";
                return false;
            }
            if (count == 0)
                break;
            contents.append(buffer.data(), static_cast<std::size_t>(count));
            if (contents.size() > maximumBytes)
            {
                ::close(descriptor);
                rejection = "oversized_request_rejected";
                return false;
            }
        }
        const bool closed = ::close(descriptor) == 0;
        if (!closed || contents.size() != static_cast<std::size_t>(status.st_size))
        {
            rejection = "request_changed_during_read";
            return false;
        }
        return true;
#else
        std::error_code statusError;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(path, statusError);
        if (statusError || std::filesystem::is_symlink(status))
            rejection = "symlink_request_rejected";
        else if (!std::filesystem::is_regular_file(status))
            rejection = "non_regular_request_rejected";
        else if (!HasOwnerOnlyPermissions(status.permissions()))
            rejection = "request_permissions_not_owner_only";
        else
        {
            const std::uintmax_t size = std::filesystem::file_size(path, statusError);
            if (statusError || size > maximumBytes)
                rejection = "oversized_request_rejected";
            else
            {
                std::ifstream input(path, std::ios::binary);
                contents.assign(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
                if (!input.eof() || contents.size() != size)
                    rejection = "request_read_failed";
            }
        }
        return rejection.empty();
#endif
    }

    std::string FormatReceipt(const EditorMaterialControlReceipt& receipt)
    {
        const auto writeSurface = [](std::ostringstream& stream,
            std::string_view label, const Engine::MaterialSurface& surface)
        {
            stream << label << ' ' << std::setprecision(std::numeric_limits<float>::max_digits10)
                   << surface.BaseColor.X << ' ' << surface.BaseColor.Y << ' '
                   << surface.BaseColor.Z << ' ' << surface.Metallic << ' '
                   << surface.Roughness << '\n';
        };

        std::ostringstream stream;
        stream << kReceiptHeader << '\n'
               << "RequestId " << std::quoted(receipt.RequestId) << '\n'
               << "SessionId " << std::quoted(receipt.SessionId) << '\n'
               << "RequestDigest " << receipt.RequestDigest << '\n'
               << "Action " << (receipt.ActionKnown ? ToString(receipt.Action) : "Unknown") << '\n'
               << "Status " << (receipt.Succeeded ? "Succeeded" : "Rejected") << '\n'
               << "Reason " << std::quoted(receipt.Reason) << '\n'
               << "Frame " << receipt.Frame << '\n'
               << "Effect " << receipt.Effect << '\n'
               << "Recovery " << receipt.Recovery << '\n'
               << "EntityId " << receipt.EntityId << '\n'
               << "EntityName " << std::quoted(receipt.EntityName) << '\n'
               << "MaterialHandle " << receipt.MaterialHandle << '\n';
        writeSurface(stream, "BeforeSurface", receipt.Before);
        writeSurface(stream, "AfterSurface", receipt.After);
        stream << "AffectedEntityCount " << receipt.AffectedEntityCount << '\n'
               << "AffectedEntitySampleCount " << receipt.AffectedEntityIds.size() << '\n'
               << "AffectedEntityIds";
        for (Engine::EntityId entity : receipt.AffectedEntityIds)
            stream << ' ' << entity;
        stream << '\n'
               << "AffectedEntityIdsTruncated "
               << (receipt.AffectedEntityIdsTruncated ? "yes" : "no") << '\n'
               << "RendererGeneration " << receipt.RendererGeneration << '\n'
               << "UndoDepthBefore " << receipt.UndoDepthBefore << '\n'
               << "UndoDepthAfter " << receipt.UndoDepthAfter << '\n'
               << "RedoDepthBefore " << receipt.RedoDepthBefore << '\n'
               << "RedoDepthAfter " << receipt.RedoDepthAfter << '\n'
               << "SelectionCommitted " << (receipt.SelectionCommitted ? "yes" : "no") << '\n'
               << "PivotRetargeted " << (receipt.PivotRetargeted ? "yes" : "no") << '\n'
               << "RendererReadbackVerified "
               << (receipt.RendererReadbackVerified ? "yes" : "no") << '\n';
        return stream.str();
    }

    bool ParseRequest(std::string_view contents, std::string_view expectedRequestId,
        EditorMaterialControlRequest& request, std::string& error)
    {
        std::istringstream input { std::string(contents) };
        std::string line;
        if (!std::getline(input, line) || line != kRequestHeader)
        {
            error = "unsupported_schema";
            return false;
        }

        enum Field : unsigned int
        {
            RequestId = 1u << 0,
            SessionId = 1u << 1,
            Action = 1u << 2,
            EntityId = 1u << 3,
            ExpectedEntityName = 1u << 4,
            MaterialHandle = 1u << 5,
            ExpectedSurface = 1u << 6,
            NewSurface = 1u << 7,
            Scope = 1u << 8
        };
        unsigned int seen = 0;
        const auto claim = [&seen](Field field)
        {
            if ((seen & field) != 0)
                return false;
            seen |= field;
            return true;
        };

        while (std::getline(input, line))
        {
            if (line.empty())
            {
                error = "empty_or_trailing_field";
                return false;
            }
            std::istringstream fieldStream(line);
            std::string key;
            if (!(fieldStream >> key))
            {
                error = "invalid_field";
                return false;
            }
            if (key == "RequestId")
            {
                if (!claim(Field::RequestId) || !ParseQuoted(fieldStream, request.RequestId))
                {
                    error = "invalid_or_duplicate_request_id";
                    return false;
                }
            }
            else if (key == "SessionId")
            {
                if (!claim(Field::SessionId) || !ParseQuoted(fieldStream, request.SessionId))
                {
                    error = "invalid_or_duplicate_session_id";
                    return false;
                }
            }
            else if (key == "Action")
            {
                std::string action;
                if (!claim(Field::Action) || !(fieldStream >> action) || !AtEnd(fieldStream))
                {
                    error = "invalid_or_duplicate_action";
                    return false;
                }
                if (action == "InspectMaterialSurface")
                    request.Action = EditorMaterialControlAction::InspectMaterialSurface;
                else if (action == "SelectEntityPatchMaterialSurface")
                    request.Action = EditorMaterialControlAction::SelectEntityPatchMaterialSurface;
                else
                {
                    error = "unsupported_action";
                    return false;
                }
            }
            else if (key == "EntityId")
            {
                if (!claim(Field::EntityId) || !ParseInteger(fieldStream, request.EntityId))
                {
                    error = "invalid_or_duplicate_entity_id";
                    return false;
                }
            }
            else if (key == "ExpectedEntityName")
            {
                if (!claim(Field::ExpectedEntityName)
                    || !ParseQuoted(fieldStream, request.ExpectedEntityName))
                {
                    error = "invalid_or_duplicate_entity_name";
                    return false;
                }
            }
            else if (key == "MaterialHandle")
            {
                if (!claim(Field::MaterialHandle)
                    || !ParseInteger(fieldStream, request.MaterialHandle))
                {
                    error = "invalid_or_duplicate_material_handle";
                    return false;
                }
            }
            else if (key == "ExpectedSurface")
            {
                if (!claim(Field::ExpectedSurface)
                    || !ParseSurface(fieldStream, request.ExpectedSurface))
                {
                    error = "invalid_or_duplicate_expected_surface";
                    return false;
                }
                request.HasExpectedSurface = true;
            }
            else if (key == "NewSurface")
            {
                if (!claim(Field::NewSurface) || !ParseSurface(fieldStream, request.NewSurface))
                {
                    error = "invalid_or_duplicate_new_surface";
                    return false;
                }
                request.HasNewSurface = true;
            }
            else if (key == "Scope")
            {
                std::string scope;
                if (!claim(Field::Scope) || !(fieldStream >> scope) || !AtEnd(fieldStream))
                {
                    error = "invalid_or_duplicate_scope";
                    return false;
                }
                request.SharedMaterialScope = scope == "SharedMaterial";
                if (!request.SharedMaterialScope)
                {
                    error = "unsupported_scope";
                    return false;
                }
            }
            else
            {
                error = "unknown_field";
                return false;
            }
        }

        constexpr unsigned int common = Field::RequestId | Field::SessionId | Field::Action
            | Field::EntityId | Field::ExpectedEntityName | Field::MaterialHandle;
        if ((seen & common) != common)
        {
            error = "missing_required_field";
            return false;
        }
        if (!IsStableId(request.RequestId) || request.RequestId != expectedRequestId)
        {
            error = "request_id_mismatch";
            return false;
        }
        if (request.EntityId == Engine::kInvalidEntityId
            || request.ExpectedEntityName.empty() || request.ExpectedEntityName.size() > 256
            || request.MaterialHandle == Engine::kInvalidAssetHandle)
        {
            error = "invalid_identity";
            return false;
        }
        if (request.Action == EditorMaterialControlAction::InspectMaterialSurface)
        {
            if ((seen & (Field::ExpectedSurface | Field::NewSurface | Field::Scope)) != 0)
            {
                error = "unexpected_action_field";
                return false;
            }
        }
        else
        {
            constexpr unsigned int patch = Field::ExpectedSurface | Field::NewSurface | Field::Scope;
            if ((seen & patch) != patch
                || !Engine::IsValidMaterialSurface(request.ExpectedSurface)
                || !Engine::IsValidMaterialSurface(request.NewSurface))
            {
                error = "invalid_or_missing_surface";
                return false;
            }
        }
        return true;
    }
}

bool EditorMaterialControlMailbox::Initialize(const std::filesystem::path& root,
    const std::filesystem::path& projectPath, std::string& error)
{
    if (IsOpen())
    {
        error = "mailbox_already_open";
        return false;
    }
    if (!root.is_absolute())
    {
        error = "control_directory_must_be_absolute";
        return false;
    }
    try
    {
        m_Terminals.reserve(MaximumTerminalRequests);
    }
    catch (const std::bad_alloc&)
    {
        error = "could_not_reserve_terminal_capacity";
        return false;
    }
    if (!CreatePrivateDirectory(root, error))
    {
        if (error == "directory_already_exists")
            error = "control_directory_must_not_exist";
        return false;
    }
    m_Root = root;
    m_Requests = root / "requests";
    m_Responses = root / "responses";
    const auto abandonInitialization = [this, &root]()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        m_Root.clear();
        m_Requests.clear();
        m_Responses.clear();
        m_SessionId.clear();
        m_ProjectPath.clear();
        m_ProcessId = 0;
        m_AcceptingRequests = false;
        m_DurabilityDegradationCount = 0;
        m_ForceParentDirectorySyncFailureOnce = false;
    };
    for (const std::filesystem::path& directory : { m_Requests, m_Responses })
    {
        if (!CreatePrivateDirectory(directory, error))
        {
            abandonInitialization();
            return false;
        }
    }
    m_SessionId = MakeSessionId();
    m_ProcessId = CurrentProcessId();
    m_ProjectPath = CanonicalProjectPath(projectPath);
    if (m_ProjectPath.empty())
    {
        error = "could_not_resolve_project_identity";
        abandonInitialization();
        return false;
    }
    m_AcceptingRequests = true;
    m_ClosedPublished = false;
    m_ImmediatePoll = true;
    m_DurabilityDegradationCount = 0;
    m_ForceParentDirectorySyncFailureOnce = false;
    m_NextDirectoryPoll = {};
    if (!PublishSessionFile("Ready", error))
    {
        abandonInitialization();
        return false;
    }
    if (m_DurabilityDegradationCount != 0)
        TransitionToClosed("ready_manifest_parent_sync_failed");
    Engine::Log::Info("EditorMaterialControlV1 state=ready session=", m_SessionId,
        " path=", m_Root.string(), " maxBytes=", MaximumRequestBytes,
        " maxPerFrame=", MaximumRequestsPerFrame,
        " maxRetained=", MaximumTerminalRequests);
    return true;
}

bool EditorMaterialControlMailbox::PublishFileNoReplace(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination, std::string_view purpose,
    bool closeOnDurabilityDegradation, std::string& error)
{
    const bool forceSyncFailure = m_ForceParentDirectorySyncFailureOnce;
    m_ForceParentDirectorySyncFailureOnce = false;
    const PublishNoReplaceResult result = PublishNoReplace(
        temporary, destination, forceSyncFailure, error);
    if (result == PublishNoReplaceResult::Failed)
        return false;
    if (result == PublishNoReplaceResult::PublishedVisibilityOnly)
    {
        ++m_DurabilityDegradationCount;
        Engine::Log::Error(
            "Editor material-control publication is visible but not confirmed crash-durable: ",
            purpose, "; committed visibility is preserved");
        if (closeOnDurabilityDegradation)
            TransitionToClosed("parent_directory_sync_failed_after_visible_publish");
    }
    return true;
}

void EditorMaterialControlMailbox::Close()
{
    if (!IsOpen())
        return;
    TransitionToClosed("editor_detach");
    Engine::Log::Info("EditorMaterialControlV1 state=closed session=", m_SessionId,
        " terminal=", m_Terminals.size(), " collisions=", m_ResponseCollisionCount);
}

bool EditorMaterialControlMailbox::EnsureProjectIdentity(
    const std::filesystem::path& projectPath)
{
    if (!IsOpen())
        return true;
    const std::string canonical = CanonicalProjectPath(projectPath);
    if (!canonical.empty() && canonical == m_ProjectPath)
        return true;
    if (!m_AcceptingRequests)
        return false;
    Engine::Log::Error("Editor material-control project identity changed; session project=",
        m_ProjectPath, " currentProject=", canonical.empty() ? "<invalid>" : canonical);
    TransitionToClosed("project_identity_changed");
    return false;
}

bool EditorMaterialControlMailbox::PublishSessionFile(
    std::string_view state, std::string& error)
{
    std::ostringstream contents;
    contents << kSessionHeader << '\n'
             << "SessionId " << std::quoted(m_SessionId) << '\n'
             << "State " << state << '\n'
             << "ProcessId " << m_ProcessId << '\n'
             << "ProjectPath " << std::quoted(m_ProjectPath) << '\n'
             << "RequestSchema 1\nReceiptSchema 1\n"
             << "Actions InspectMaterialSurface,SelectEntityPatchMaterialSurface\n"
             << "MaximumRequestBytes " << MaximumRequestBytes << '\n'
             << "MaximumRequestsPerFrame " << MaximumRequestsPerFrame << '\n'
             << "MaximumTerminalRequests " << MaximumTerminalRequests << '\n'
             << "MaximumAffectedEntityIds " << MaximumAffectedEntityIds << '\n';
    const std::filesystem::path destination = m_Root
        / (state == "Ready" ? "session.info" : "session.closed");
    const std::filesystem::path temporary = m_Root
        / (".session." + std::to_string(++m_TemporarySequence) + ".tmp");
    return WriteOwnerOnlyTemporary(temporary, contents.str(), error)
        && PublishFileNoReplace(temporary, destination,
            state == "Ready" ? "ready session manifest" : "closed session manifest",
            false, error);
}

void EditorMaterialControlMailbox::TransitionToClosed(std::string_view reason)
{
    m_AcceptingRequests = false;
    if (m_ClosedPublished || !IsOpen())
        return;
    std::string error;
    m_ClosedPublished = PublishSessionFile("Closed", error);
    if (!m_ClosedPublished)
        Engine::Log::Error("Editor material-control close publication failed: ", error);
    else if (!error.empty())
        Engine::Log::Error(
            "Editor material-control close is visible but not confirmed crash-durable: ",
            error);
    Engine::Log::Info("EditorMaterialControlV1 accepting=no reason=", reason,
        " retained=", m_Terminals.size());
}

const EditorMaterialControlReceipt* EditorMaterialControlMailbox::FindTerminalReceipt(
    std::string_view requestId) const
{
    const auto found = std::find_if(m_Terminals.begin(), m_Terminals.end(),
        [requestId](const TerminalEntry& entry) { return entry.RequestId == requestId; });
    return found == m_Terminals.end() ? nullptr : &found->Receipt;
}

const std::string* EditorMaterialControlMailbox::FindTerminalText(
    std::string_view requestId) const
{
    const auto found = std::find_if(m_Terminals.begin(), m_Terminals.end(),
        [requestId](const TerminalEntry& entry) { return entry.RequestId == requestId; });
    return found == m_Terminals.end() ? nullptr : &found->Text;
}

bool EditorMaterialControlMailbox::PublishResponse(
    const TerminalEntry& terminal, bool allowExisting, std::string& error)
{
    error.clear();
    const std::filesystem::path destination = m_Responses
        / (terminal.RequestId + ".response");
    std::error_code statusError;
    const std::filesystem::file_status existing =
        std::filesystem::symlink_status(destination, statusError);
    if (existing.type() != std::filesystem::file_type::not_found)
    {
        if (allowExisting)
        {
            std::string existingText;
            std::string readError;
            if (ReadOwnerOnlyRegularFile(destination, MaximumResponseBytes,
                    existingText, readError)
                && existingText == terminal.Text)
                return true;
            error = "existing_response_does_not_match_cached_terminal";
            return false;
        }
        error = "response_collision";
        return false;
    }
    std::filesystem::path temporary;
    return StageResponse(terminal.RequestId, terminal.Text, temporary, error)
        && PublishFileNoReplace(temporary, destination, "terminal response", true, error);
}

bool EditorMaterialControlMailbox::PublishRecoveryResponse(
    const TerminalEntry& terminal, std::string& error)
{
    error.clear();
    const std::filesystem::path destination = m_Responses
        / (terminal.RequestId + ".recovery.response");
    std::error_code statusError;
    if (std::filesystem::symlink_status(destination, statusError).type()
        != std::filesystem::file_type::not_found)
    {
        error = "recovery_response_collision";
        return false;
    }
    std::filesystem::path temporary;
    return StageResponse(terminal.RequestId, terminal.Text, temporary, error)
        && PublishFileNoReplace(
            temporary, destination, "recovery-required response", true, error);
}

bool EditorMaterialControlMailbox::StageResponse(std::string_view requestId,
    std::string_view text, std::filesystem::path& temporary, std::string& error)
{
    if (text.size() > MaximumResponseBytes)
    {
        error = "response_exceeds_bounded_schema";
        return false;
    }
    temporary = m_Responses / ("." + std::string(requestId) + "."
        + std::to_string(++m_TemporarySequence) + ".tmp");
    return WriteOwnerOnlyTemporary(temporary, text, error);
}

bool EditorMaterialControlMailbox::RequeueClaimedRequest(
    const std::filesystem::path& claimed, std::string_view requestId,
    std::string_view requestBytes, std::string& error)
{
    const std::filesystem::path destination = m_Requests
        / (std::string(requestId) + ".request");
#if defined(_WIN32)
    const bool moved = MoveFileExW(claimed.c_str(), destination.c_str(),
        MOVEFILE_WRITE_THROUGH) != 0;
    const DWORD nativeError = moved ? ERROR_SUCCESS : GetLastError();
    const bool collision = nativeError == ERROR_FILE_EXISTS
        || nativeError == ERROR_ALREADY_EXISTS;
#elif defined(__linux__)
    constexpr unsigned int renameNoReplace = 1;
    const bool moved = ::syscall(SYS_renameat2, AT_FDCWD, claimed.c_str(),
        AT_FDCWD, destination.c_str(), renameNoReplace) == 0;
    const int nativeError = moved ? 0 : errno;
    const bool collision = nativeError == EEXIST;
#else
    std::error_code linkError;
    std::filesystem::create_hard_link(claimed, destination, linkError);
    const bool moved = !linkError;
    const bool collision = linkError && std::filesystem::exists(destination);
    if (moved)
    {
        std::error_code ignored;
        std::filesystem::remove(claimed, ignored);
    }
#endif
    if (moved)
    {
        if (!SyncContainingDirectory(destination))
        {
            error = "requeued_request_visible_without_parent_directory_sync";
            ++m_DurabilityDegradationCount;
            Engine::Log::Error(
                "Editor material-control request requeue is visible but not confirmed "
                "crash-durable; retained request is preserved");
            TransitionToClosed("requeue_parent_directory_sync_failed");
        }
        return true;
    }
    if (collision)
    {
        std::string pendingBytes;
        std::string readError;
        if (ReadOwnerOnlyRegularFile(destination, MaximumRequestBytes,
                pendingBytes, readError)
            && pendingBytes == requestBytes)
        {
            std::error_code ignored;
            std::filesystem::remove(claimed, ignored);
            return true;
        }
        error = "different_request_occupied_requeue_destination";
    }
    else
    {
        error = "request_requeue_rename_failed";
    }
    return false;
}

bool EditorMaterialControlMailbox::PublishCollisionReceipt(
    std::string_view requestId, std::string& error)
{
    EditorMaterialControlReceipt receipt;
    receipt.RequestId = std::string(requestId);
    receipt.SessionId = m_SessionId;
    receipt.RequestDigest = "not-applicable";
    receipt.Reason = "request_id_conflict";
    receipt.Frame = 0;
    TerminalEntry terminal { receipt.RequestId, receipt.RequestDigest,
        {}, receipt, FormatReceipt(receipt) };
    const std::filesystem::path destination = m_Responses
        / (terminal.RequestId + ".collision.response");
    std::error_code statusError;
    if (std::filesystem::symlink_status(destination, statusError).type()
        != std::filesystem::file_type::not_found)
    {
        std::string existingText;
        std::string readError;
        if (ReadOwnerOnlyRegularFile(destination, MaximumResponseBytes,
                existingText, readError)
            && existingText == terminal.Text)
            return true;
        error = "existing_collision_response_is_not_exact";
        return false;
    }
    const std::filesystem::path temporary = m_Responses
        / ("." + terminal.RequestId + ".collision."
            + std::to_string(++m_TemporarySequence) + ".tmp");
    return WriteOwnerOnlyTemporary(temporary, terminal.Text, error)
        && PublishFileNoReplace(
            temporary, destination, "request-ID conflict response", true, error);
}

void EditorMaterialControlMailbox::Drain(Engine::u64 frame, const Handler& handler)
{
    if (!IsOpen() || !m_AcceptingRequests)
        return;
    if (m_Terminals.size() >= MaximumTerminalRequests)
    {
        TransitionToClosed("terminal_capacity_reached");
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!m_ImmediatePoll && now < m_NextDirectoryPoll)
    {
        ++m_CadenceSkipCount;
        return;
    }
    m_ImmediatePoll = false;
    ++m_DirectoryPollCount;
    std::vector<std::filesystem::path> candidates;
    std::error_code iterationError;
    std::size_t scanned = 0;
    for (std::filesystem::directory_iterator iterator(m_Requests, iterationError), end;
        !iterationError && iterator != end && scanned < MaximumEntriesScannedPerFrame;
        iterator.increment(iterationError), ++scanned)
    {
        const std::filesystem::path path = iterator->path();
        const std::string filename = path.filename().string();
        if (!filename.empty() && filename.front() != '.' && path.extension() == ".request")
            candidates.push_back(path);
    }
    std::sort(candidates.begin(), candidates.end());
    const std::size_t remainingCapacity =
        MaximumTerminalRequests - m_Terminals.size();
    const std::size_t frameLimit = std::min(MaximumRequestsPerFrame, remainingCapacity);
    if (candidates.size() > frameLimit)
        candidates.resize(frameLimit);
    for (const std::filesystem::path& path : candidates)
    {
        if (m_Terminals.size() >= MaximumTerminalRequests)
        {
            TransitionToClosed("terminal_capacity_reached");
            break;
        }
        ProcessRequest(path, frame, handler);
        if (!m_AcceptingRequests)
            break;
    }
    if (m_Terminals.size() >= MaximumTerminalRequests)
        TransitionToClosed("terminal_capacity_reached");
    else if (candidates.size() == frameLimit && frameLimit != 0)
        m_ImmediatePoll = true;
    else
        m_NextDirectoryPoll = now + std::chrono::milliseconds(16);
}

void EditorMaterialControlMailbox::ProcessRequest(const std::filesystem::path& path,
    Engine::u64 frame, const Handler& handler)
{
    const std::string requestId = path.stem().string();
    if (!IsStableId(requestId))
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return;
    }

    const std::filesystem::path claimedPath = m_Requests
        / (".processing." + requestId + "."
            + std::to_string(++m_TemporarySequence));
    std::error_code claimError;
    std::filesystem::rename(path, claimedPath, claimError);
    if (claimError)
        return;
    std::string contents;
    std::string rejection;
    const bool requestReplayable = ReadOwnerOnlyRegularFile(
        claimedPath, MaximumRequestBytes, contents, rejection);

    const std::string digest = requestReplayable ? Digest(contents) : "unavailable";
    const auto prior = std::find_if(m_Terminals.begin(), m_Terminals.end(),
        [&requestId](const TerminalEntry& entry) { return entry.RequestId == requestId; });
    if (prior != m_Terminals.end())
    {
        std::string retryError;
        if (!prior->RequestReplayable || !requestReplayable
            || prior->RequestBytes != contents)
        {
            ++m_ResponseCollisionCount;
            Engine::Log::Warn(
                "Editor material-control request ID is non-replayable or changed payload: ",
                requestId);
            if (!PublishCollisionReceipt(requestId, retryError))
            {
                if (!RequeueClaimedRequest(
                    claimedPath, requestId, contents, retryError))
                    TransitionToClosed("duplicate_collision_receipt_and_requeue_failed");
                return;
            }
        }
        else if (!PublishResponse(*prior, true, retryError))
        {
            ++m_ResponseCollisionCount;
            Engine::Log::Error("Editor material-control cached response verification failed: ",
                retryError);
            if (!PublishCollisionReceipt(requestId, retryError))
            {
                if (!RequeueClaimedRequest(
                    claimedPath, requestId, contents, retryError))
                    TransitionToClosed("cached_response_and_requeue_failed");
                return;
            }
        }
        std::filesystem::remove(claimedPath, claimError);
        return;
    }

    const std::filesystem::path responsePath = m_Responses / (requestId + ".response");
    std::error_code responseStatusError;
    if (std::filesystem::symlink_status(responsePath, responseStatusError).type()
        != std::filesystem::file_type::not_found)
    {
        ++m_ResponseCollisionCount;
        std::string collisionError;
        if (!PublishCollisionReceipt(requestId, collisionError))
        {
            if (!RequeueClaimedRequest(
                claimedPath, requestId, contents, collisionError))
                TransitionToClosed("response_collision_receipt_and_requeue_failed");
            return;
        }
        std::filesystem::remove(claimedPath, claimError);
        return;
    }

    EditorMaterialControlRequest request;
    if (rejection.empty())
        ParseRequest(contents, requestId, request, rejection);
    if (rejection.empty() && request.SessionId != m_SessionId)
        rejection = "wrong_session";

    EditorMaterialControlTransaction transaction;
    if (rejection.empty())
        transaction = handler(request, frame);
    else
        transaction.Receipt.Reason = rejection;
    EditorMaterialControlReceipt& receipt = transaction.Receipt;
    receipt.RequestId = requestId;
    receipt.SessionId = m_SessionId;
    receipt.RequestDigest = digest;
    receipt.Frame = frame;
    if (rejection.empty())
    {
        receipt.Action = request.Action;
        receipt.ActionKnown = true;
    }
    TerminalEntry terminal { requestId, digest, contents, receipt, {} };
    terminal.RequestReplayable = requestReplayable;
    terminal.Text = FormatReceipt(terminal.Receipt);

    if (!transaction.Mutating)
    {
        std::string publishError;
        if (!PublishResponse(terminal, false, publishError))
        {
            const bool collision = publishError == "response_collision"
                || publishError == "destination_collision"
                || publishError == "existing_response_does_not_match_cached_terminal";
            if (collision)
            {
                ++m_ResponseCollisionCount;
                if (PublishCollisionReceipt(requestId, publishError))
                {
                    std::filesystem::remove(claimedPath, claimError);
                    return;
                }
            }
            Engine::Log::Error("Editor material-control receipt publication failed before mutation: ",
                publishError);
            if (!RequeueClaimedRequest(
                claimedPath, requestId, contents, publishError))
                TransitionToClosed("nonmutating_receipt_and_requeue_failed");
            return;
        }
        m_Terminals.push_back(std::move(terminal));
        std::filesystem::remove(claimedPath, claimError);
        return;
    }

    if (!receipt.Succeeded || !transaction.Commit || !transaction.Rollback)
    {
        Engine::Log::Error("Editor material-control handler returned an invalid mutation transaction");
        receipt.Succeeded = false;
        receipt.Reason = "invalid_internal_transaction";
        receipt.Effect = "None";
        transaction.Mutating = false;
        terminal.Receipt = receipt;
        terminal.Text = FormatReceipt(receipt);
        std::string publishError;
        if (PublishResponse(terminal, false, publishError))
        {
            m_Terminals.push_back(std::move(terminal));
            std::filesystem::remove(claimedPath, claimError);
        }
        else if (!RequeueClaimedRequest(
            claimedPath, requestId, contents, publishError))
            TransitionToClosed("invalid_transaction_receipt_and_requeue_failed");
        return;
    }

    std::filesystem::path stagedResponse;
    std::string publishError;
    if (!StageResponse(requestId, terminal.Text, stagedResponse, publishError))
    {
        Engine::Log::Error("Editor material-control receipt staging failed before mutation: ",
            publishError);
        if (!RequeueClaimedRequest(claimedPath, requestId, contents, publishError))
            TransitionToClosed("receipt_staging_and_requeue_failed");
        return;
    }

    const auto closeAfterUnverifiedRollback = [&](std::string_view reason)
    {
        std::error_code ignored;
        std::filesystem::remove(stagedResponse, ignored);
        receipt.Succeeded = false;
        receipt.Reason = std::string(reason);
        receipt.Effect = "RecoveryRequired";
        receipt.Recovery = "RestartSession";
        receipt.RendererReadbackVerified = false;
        terminal.Receipt = receipt;
        terminal.Text = FormatReceipt(receipt);
        std::string recoveryReceiptError;
        if (PublishResponse(terminal, false, recoveryReceiptError)
            || PublishRecoveryResponse(terminal, recoveryReceiptError))
            m_Terminals.push_back(std::move(terminal));
        else
            Engine::Log::Error(
                "Editor material-control recovery-required receipt could not be published: ",
                recoveryReceiptError);
        std::filesystem::remove(claimedPath, ignored);
        TransitionToClosed(reason);
    };

    std::string commitError;
    if (!transaction.Commit(commitError))
    {
        if (!transaction.Rollback(receipt))
        {
            closeAfterUnverifiedRollback("rollback_verification_failed");
            return;
        }
        std::error_code ignored;
        std::filesystem::remove(stagedResponse, ignored);
        receipt.Succeeded = false;
        receipt.Reason = commitError.empty()
            ? "commit_failed_and_rolled_back" : commitError + "_rolled_back";
        receipt.Effect = "RolledBack";
        receipt.Recovery = "None";
        receipt.After = receipt.Before;
        terminal.Receipt = receipt;
        terminal.Text = FormatReceipt(receipt);
        Engine::Log::Error("Editor material-control commit rolled back: ", receipt.Reason);
        if (!PublishResponse(terminal, false, publishError))
        {
            if ((publishError == "response_collision"
                    || publishError == "destination_collision")
                && PublishCollisionReceipt(requestId, publishError))
            {
                ++m_ResponseCollisionCount;
                std::filesystem::remove(claimedPath, claimError);
                return;
            }
            if (!RequeueClaimedRequest(claimedPath, requestId, contents, publishError))
                TransitionToClosed("rollback_receipt_and_requeue_failed");
            return;
        }
        m_Terminals.push_back(std::move(terminal));
        std::filesystem::remove(claimedPath, claimError);
        return;
    }

    if (PublishFileNoReplace(
            stagedResponse, responsePath, "committed mutation response", true, publishError))
    {
        m_Terminals.push_back(std::move(terminal));
        std::filesystem::remove(claimedPath, claimError);
        return;
    }

    if (!transaction.Rollback(receipt))
    {
        closeAfterUnverifiedRollback("postcommit_rollback_verification_failed");
        return;
    }
    Engine::Log::Error(
        "Editor material-control committed mutation rolled back after final receipt publication failure: ",
        publishError);
    if (publishError == "destination_collision")
    {
        ++m_ResponseCollisionCount;
        if (PublishCollisionReceipt(requestId, publishError))
        {
            std::filesystem::remove(claimedPath, claimError);
            return;
        }
    }
    if (!RequeueClaimedRequest(claimedPath, requestId, contents, publishError))
        TransitionToClosed("postcommit_rollback_requeue_failed");
}

bool EditorMaterialControlMailbox::PublishRequestForSmoke(
    std::string_view requestId, std::string_view contents, std::string& error)
{
    if (!IsOpen() || !m_AcceptingRequests || !IsStableId(requestId))
    {
        error = "invalid_smoke_request";
        return false;
    }
    const std::filesystem::path temporary = m_Requests
        / ("." + std::string(requestId) + "." + std::to_string(++m_TemporarySequence) + ".tmp");
    const std::filesystem::path destination = m_Requests
        / (std::string(requestId) + ".request");
    const bool published = WriteOwnerOnlyTemporary(temporary, contents, error)
        && PublishFileNoReplace(temporary, destination, "smoke request", true, error);
    if (published)
        m_ImmediatePoll = true;
    return published;
}

bool EditorMaterialControlMailbox::PublishRawResponseForSmoke(
    std::string_view requestId, std::string_view contents, std::string& error)
{
    if (!IsOpen() || !IsStableId(requestId))
    {
        error = "invalid_smoke_response";
        return false;
    }
    const std::filesystem::path temporary = m_Responses
        / ("." + std::string(requestId) + "." + std::to_string(++m_TemporarySequence) + ".tmp");
    const std::filesystem::path destination = m_Responses
        / (std::string(requestId) + ".response");
    return WriteOwnerOnlyTemporary(temporary, contents, error)
        && PublishFileNoReplace(temporary, destination, "smoke raw response", true, error);
}

bool EditorMaterialControlMailbox::PublishLiveTargetForSmoke(
    Engine::EntityId entityId, std::string_view entityName,
    Engine::AssetHandle materialHandle, const Engine::MaterialSurface& before,
    const Engine::MaterialSurface& after, std::string& error)
{
    if (!IsOpen() || entityId == Engine::kInvalidEntityId
        || materialHandle == Engine::kInvalidAssetHandle
        || entityName.empty() || !Engine::IsValidMaterialSurface(before)
        || !Engine::IsValidMaterialSurface(after))
    {
        error = "invalid_live_smoke_target";
        return false;
    }
    const auto writeSurface = [](std::ostringstream& stream,
        std::string_view label, const Engine::MaterialSurface& surface)
    {
        stream << label << ' ' << std::setprecision(std::numeric_limits<float>::max_digits10)
               << surface.BaseColor.X << ' ' << surface.BaseColor.Y << ' '
               << surface.BaseColor.Z << ' ' << surface.Metallic << ' '
               << surface.Roughness << '\n';
    };
    std::ostringstream contents;
    contents << "SpiralEditorMaterialControlTarget 1\n"
             << "SessionId " << std::quoted(m_SessionId) << '\n'
             << "EntityId " << entityId << '\n'
             << "EntityName " << std::quoted(entityName) << '\n'
             << "MaterialHandle " << materialHandle << '\n';
    writeSurface(contents, "BeforeSurface", before);
    writeSurface(contents, "AfterSurface", after);
    const std::filesystem::path temporary = m_Root
        / (".live-target." + std::to_string(++m_TemporarySequence) + ".tmp");
    return WriteOwnerOnlyTemporary(temporary, contents.str(), error)
        && PublishFileNoReplace(temporary, m_Root / "live-target.info",
            "live smoke target", true, error);
}

std::string EditorMaterialControlMailbox::FormatInspectRequest(std::string_view requestId,
    std::string_view sessionId, Engine::EntityId entityId,
    std::string_view expectedEntityName, Engine::AssetHandle materialHandle)
{
    std::ostringstream stream;
    stream << kRequestHeader << '\n'
           << "RequestId " << std::quoted(requestId) << '\n'
           << "SessionId " << std::quoted(sessionId) << '\n'
           << "Action InspectMaterialSurface\n"
           << "EntityId " << entityId << '\n'
           << "ExpectedEntityName " << std::quoted(expectedEntityName) << '\n'
           << "MaterialHandle " << materialHandle << '\n';
    return stream.str();
}

std::string EditorMaterialControlMailbox::FormatPatchRequest(std::string_view requestId,
    std::string_view sessionId, Engine::EntityId entityId,
    std::string_view expectedEntityName, Engine::AssetHandle materialHandle,
    const Engine::MaterialSurface& expectedSurface,
    const Engine::MaterialSurface& newSurface)
{
    const auto writeSurface = [](std::ostringstream& stream,
        std::string_view label, const Engine::MaterialSurface& surface)
    {
        stream << label << ' ' << std::setprecision(std::numeric_limits<float>::max_digits10)
               << surface.BaseColor.X << ' ' << surface.BaseColor.Y << ' '
               << surface.BaseColor.Z << ' ' << surface.Metallic << ' '
               << surface.Roughness << '\n';
    };
    std::ostringstream stream;
    stream << kRequestHeader << '\n'
           << "RequestId " << std::quoted(requestId) << '\n'
           << "SessionId " << std::quoted(sessionId) << '\n'
           << "Action SelectEntityPatchMaterialSurface\n"
           << "EntityId " << entityId << '\n'
           << "ExpectedEntityName " << std::quoted(expectedEntityName) << '\n'
           << "MaterialHandle " << materialHandle << '\n';
    writeSurface(stream, "ExpectedSurface", expectedSurface);
    writeSurface(stream, "NewSurface", newSurface);
    stream << "Scope SharedMaterial\n";
    return stream.str();
}
