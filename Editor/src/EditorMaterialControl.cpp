#include "EditorMaterialControl.h"

#include <Engine/Core/Log.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
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
    constexpr std::string_view kRequestHeader = "SpiralEditorControlRequest 5";
    constexpr std::string_view kReceiptHeader = "SpiralEditorControlReceipt 5";
    constexpr std::string_view kSessionHeader = "SpiralEditorControlSession 5";

    const char* ToString(EditorMaterialControlAction action)
    {
        switch (action)
        {
            case EditorMaterialControlAction::InspectMaterialSurface:
                return "InspectMaterialSurface";
            case EditorMaterialControlAction::SelectEntityPatchMaterialSurface:
                return "SelectEntityPatchMaterialSurface";
            case EditorMaterialControlAction::InspectEntity: return "InspectEntity";
            case EditorMaterialControlAction::SelectEntity: return "SelectEntity";
            case EditorMaterialControlAction::SetEntityTransform: return "SetEntityTransform";
            case EditorMaterialControlAction::SetTypedLight: return "SetTypedLight";
            case EditorMaterialControlAction::SetProjectColorPipeline: return "SetProjectColorPipeline";
            case EditorMaterialControlAction::SetViewportMainCameraPose: return "SetViewportMainCameraPose";
            case EditorMaterialControlAction::SetSceneDebugVisualization:
                return "SetSceneDebugVisualization";
            case EditorMaterialControlAction::SetMeshRendererFlags:
                return "SetMeshRendererFlags";
            case EditorMaterialControlAction::PickAtViewportPoint: return "PickAtViewportPoint";
            case EditorMaterialControlAction::FocusSelection: return "FocusSelection";
            case EditorMaterialControlAction::InspectFabImport: return "InspectFabImport";
            case EditorMaterialControlAction::SelectFabPackage: return "SelectFabPackage";
            case EditorMaterialControlAction::SetFabProvenance: return "SetFabProvenance";
            case EditorMaterialControlAction::ConfirmFabProvenance: return "ConfirmFabProvenance";
            case EditorMaterialControlAction::CommitFabImport: return "CommitFabImport";
            case EditorMaterialControlAction::CancelFabImport: return "CancelFabImport";
            case EditorMaterialControlAction::DismissFabImport: return "DismissFabImport";
            case EditorMaterialControlAction::PlaceMeshAsset: return "PlaceMeshAsset";
            case EditorMaterialControlAction::SetEntityMeshRendererAssets:
                return "SetEntityMeshRendererAssets";
            case EditorMaterialControlAction::SaveProjectState: return "SaveProjectState";
            case EditorMaterialControlAction::ValidateProject: return "ValidateProject";
            case EditorMaterialControlAction::SetFabPanelVisible: return "SetFabPanelVisible";
            case EditorMaterialControlAction::InspectFabPanel: return "InspectFabPanel";
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

    bool ParseTransform(std::istringstream& stream, Engine::Math::SectorLocalPosition& position,
        Engine::Math::Vec3& rotation, Engine::Math::Vec3& scale)
    {
        return static_cast<bool>(stream >> position.Sector.X >> position.Sector.Y >> position.Sector.Z
            >> position.Local.X >> position.Local.Y >> position.Local.Z
            >> rotation.X >> rotation.Y >> rotation.Z
            >> scale.X >> scale.Y >> scale.Z) && AtEnd(stream);
    }

    bool ParseLight(std::istringstream& stream, Engine::LightComponent& light)
    {
        std::string type, unit, shadows;
        if (!(stream >> type >> light.Color.X >> light.Color.Y >> light.Color.Z
                >> light.PhotometricValue >> unit >> light.Range >> light.InnerConeDegrees
                >> light.OuterConeDegrees >> shadows) || !AtEnd(stream)
            || !Engine::TryParseLightType(type, light.Type)
            || !Engine::TryParseLightPhotometricUnit(unit, light.PhotometricUnit))
            return false;
        if (shadows == "yes") light.CastsShadows = true;
        else if (shadows == "no") light.CastsShadows = false;
        else return false;
        return Engine::IsValidLightComponent(light);
    }

    bool ParseColorPipeline(std::istringstream& stream, Engine::RendererColorPipelineSettings& settings)
    {
        std::string mode;
        return static_cast<bool>(stream >> settings.ManualExposureEV100
            >> settings.PostToneMapSaturation >> settings.PostToneMapContrast >> mode
            >> settings.CameraApertureFNumber >> settings.CameraShutterSeconds >> settings.CameraISO)
            && AtEnd(stream) && Engine::ParseRendererExposureMode(mode, settings.ExposureMode)
            && Engine::IsValidRendererColorPipelineSettings(settings);
    }

    bool ParseDebugVisualization(std::istringstream& stream,
        Engine::SceneDebugView& view, bool& showSelectedBounds)
    {
        std::string viewText;
        std::string boundsText;
        if (!(stream >> viewText >> boundsText) || !AtEnd(stream)
            || !Engine::TryParseSceneDebugView(viewText, view))
            return false;
        if (boundsText == "yes")
            showSelectedBounds = true;
        else if (boundsText == "no")
            showSelectedBounds = false;
        else
            return false;
        return true;
    }

    bool ParseMeshRendererFlags(std::istringstream& stream,
        bool& visible, bool& castsShadows)
    {
        std::string visibleText;
        std::string shadowsText;
        if (!(stream >> visibleText >> shadowsText) || !AtEnd(stream))
            return false;
        if (visibleText == "yes") visible = true;
        else if (visibleText == "no") visible = false;
        else return false;
        if (shadowsText == "yes") castsShadows = true;
        else if (shadowsText == "no") castsShadows = false;
        else return false;
        return true;
    }

    // Two finite coordinates in [0, 1]. Parsed with from_chars so a locale can never
    // change the decimal separator.
    bool ParseViewportPoint(std::istringstream& stream, double& x, double& y)
    {
        std::string first;
        std::string second;
        if (!(stream >> first >> second) || !AtEnd(stream))
            return false;
        const auto parse = [](const std::string& text, double& value)
        {
            const char* begin = text.data();
            const char* end = text.data() + text.size();
            const auto result = std::from_chars(begin, end, value);
            return result.ec == std::errc {} && result.ptr == end && std::isfinite(value)
                && value >= 0.0 && value <= 1.0;
        };
        return parse(first, x) && parse(second, y);
    }

    bool IsLowerHex64(std::string_view text)
    {
        return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char character)
        {
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        });
    }

    bool ParseHex64(std::istringstream& stream, std::string& value)
    {
        return (stream >> value) && AtEnd(stream) && IsLowerHex64(value);
    }

    // A single bounded identifier-like token (kinds, relations, license names).
    bool ParseWord(std::istringstream& stream, std::string& value, std::size_t maximum = 48)
    {
        if (!(stream >> value) || !AtEnd(stream) || value.empty() || value.size() > maximum)
            return false;
        return std::all_of(value.begin(), value.end(), [](char character)
        {
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9') || character == '-' || character == '_';
        });
    }

    template<typename Enum, std::size_t Count>
    bool ParseFabEnum(const std::string& text, const std::array<Enum, Count>& values, Enum& out)
    {
        for (Enum candidate : values)
        {
            if (text == Engine::ToString(candidate))
            {
                out = candidate;
                return true;
            }
        }
        return false;
    }

    bool ParseQuotedBounded(std::istringstream& stream, std::string& value, std::size_t maximumBytes)
    {
        return ParseQuoted(stream, value) && value.size() <= maximumBytes
            && std::none_of(value.begin(), value.end(), [](char character)
                { return static_cast<unsigned char>(character) < 0x20 || character == 0x7f; });
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

    std::string TokenOrNone(const std::string& value)
    {
        return value.empty() ? std::string("none") : value;
    }

    std::string SanitizeFabText(std::string_view text)
    {
        // Receipts are line-oriented: a message must never carry a control byte
        // or an unbounded body. Replacement keeps the receipt well-formed.
        std::string result;
        result.reserve(std::min<std::size_t>(text.size(), 512));
        for (unsigned char byte : text)
        {
            if (result.size() >= 512)
                break;
            result.push_back(byte < 0x20 || byte == 0x7f ? ' ' : static_cast<char>(byte));
        }
        return result;
    }

    void WriteHandleList(std::ostringstream& stream, std::string_view label,
        const std::vector<Engine::AssetHandle>& handles)
    {
        stream << label;
        const std::size_t count = std::min(handles.size(),
            EditorMaterialControlMailbox::MaximumFabResultHandles);
        for (std::size_t index = 0; index < count; ++index)
            stream << ' ' << handles[index];
        stream << '\n';
    }

    // The schema-4 Fab block. Its line order is part of the receipt contract and
    // is mirrored by Scripts/EditorMaterialControl.py.
    void WriteFabBlock(std::ostringstream& stream, const EditorFabControlReceipt& fab)
    {
        const auto yesNo = [](bool value) { return value ? "yes" : "no"; };
        stream << "FabState " << fab.State << '\n'
               << "FabJobId " << fab.JobId << '\n'
               << "FabCancelRequested " << yesNo(fab.CancelRequested) << '\n'
               << "FabProgress " << fab.FilesCompleted << ' ' << fab.FileCount << ' '
               << fab.BytesCompleted << ' ' << fab.BytesTotal << '\n'
               << "FabSourceKind " << fab.SourceKind << '\n'
               << "FabSourceOrigin " << fab.SourceOrigin << '\n'
               << "FabSourceName " << std::quoted(SanitizeFabText(fab.SourceName)) << '\n'
               << "FabErrorCode " << fab.ErrorCode << '\n'
               << "FabMessage " << std::quoted(SanitizeFabText(fab.Message)) << '\n'
               << "FabLastRejection " << std::quoted(SanitizeFabText(fab.LastRejection)) << '\n'
               << "FabNote " << std::quoted(SanitizeFabText(fab.Note)) << '\n'
               << "FabFormat " << fab.Format << '\n'
               << "FabSourceSha256 " << TokenOrNone(fab.SourceSha256) << '\n'
               << "FabExpandedTreeSha256 " << TokenOrNone(fab.ExpandedTreeSha256) << '\n'
               << "FabSummary " << fab.SummaryVertices << ' ' << fab.SummaryTriangles << ' '
               << fab.SummaryPrimitives << ' ' << fab.SummaryTextures << ' '
               << fab.SummaryFiles << ' ' << fab.SummaryBytes << '\n'
               << "FabSummaryMaterial " << std::quoted(SanitizeFabText(fab.SummaryMaterial)) << '\n'
               << "FabProvenanceValid " << yesNo(fab.ProvenanceValid) << '\n'
               << "FabProvenanceConfirmed " << yesNo(fab.ProvenanceConfirmed) << '\n'
               << "FabProvenanceDigest " << TokenOrNone(fab.ProvenanceDigest) << '\n'
               << "FabProvenanceError " << std::quoted(SanitizeFabText(fab.ProvenanceError)) << '\n'
               << "FabRelation " << fab.Relation << '\n'
               << "FabStreamId " << TokenOrNone(fab.StreamId) << '\n'
               << "FabGenerationId " << TokenOrNone(fab.GenerationId) << '\n'
               << "FabProjectChanged " << yesNo(fab.ProjectChanged) << '\n'
               << "FabAssignmentApplied " << yesNo(fab.AssignmentApplied) << '\n'
               << "FabCommitOutcome " << fab.CommitOutcome << '\n'
               << "FabManifestRevision " << fab.ManifestRevision << '\n'
               << "FabManifestSha256 " << TokenOrNone(fab.ManifestSha256) << '\n'
               << "FabMeshAsset " << fab.MeshAsset << '\n'
               << "FabMaterialAsset " << fab.MaterialAsset << '\n'
               << "FabResultHandleCount " << fab.ResultHandleCount << '\n';
        WriteHandleList(stream, "FabResultHandles", fab.ResultHandles);
        stream << "FabProjectReceiptCount " << fab.ProjectReceiptCount << '\n';
        WriteHandleList(stream, "FabProjectMeshAssets", fab.ProjectMeshAssets);
        WriteHandleList(stream, "FabProjectMaterialAssets", fab.ProjectMaterialAssets);
        stream << "FabProjectStructural " << fab.ProjectStructural << '\n'
               << "FabProjectStructuralMessage "
               << std::quoted(SanitizeFabText(fab.ProjectStructuralMessage)) << '\n'
               << "FabProjectValidation " << fab.ProjectValidation << '\n'
               << "FabProjectValidationMessage "
               << std::quoted(SanitizeFabText(fab.ProjectValidationMessage)) << '\n'
               << "FabPanel " << fab.PanelState << ' ' << yesNo(fab.PanelInitialized) << ' '
               << yesNo(fab.PanelFailed) << ' ' << yesNo(fab.PanelVisible) << ' '
               << yesNo(fab.PanelKeyboardOwnedByPage) << ' ' << yesNo(fab.PanelTextureValid) << ' '
               << yesNo(fab.PanelLoading) << ' ' << fab.PanelFramesReceived << ' '
               << fab.PanelFrameWidth << ' ' << fab.PanelFrameHeight << ' '
               << fab.PanelNavigationDenials << ' ' << fab.PanelDownloadsCompleted << '\n'
               << "FabPanelHost " << std::quoted(SanitizeFabText(fab.PanelHost)) << '\n'
               << "FabPanelError " << std::quoted(SanitizeFabText(fab.PanelError)) << '\n';
    }

    // The schema-5 viewport block. Its line order is part of the receipt contract and
    // is mirrored by Scripts/EditorMaterialControl.py.
    void WriteViewportBlock(std::ostringstream& stream, const EditorViewportControlReceipt& viewport)
    {
        const auto yesNo = [](bool value) { return value ? "yes" : "no"; };
        // Earlier blocks leave the stream at float precision; these are doubles that
        // the helper compares numerically, so print them round-trip exact.
        const std::streamsize previousPrecision =
            stream.precision(std::numeric_limits<double>::max_digits10);
        const auto writePose = [&stream](std::string_view label, const double (&pose)[6])
        {
            stream << label;
            for (const double value : pose)
                stream << ' ' << value;
            stream << '\n';
        };
        stream << "ViewportPick " << viewport.PickState << ' ' << viewport.PickEntityId << ' '
               << viewport.PickDistance << ' ' << viewport.PickRefinement << ' '
               << viewport.PickCandidates << ' ' << viewport.PickBoxHits << ' '
               << viewport.PickTrianglesTested << '\n'
               << "ViewportPickPoint " << viewport.PickNormalizedX << ' ' << viewport.PickNormalizedY
               << ' ' << viewport.PickPixelX << ' ' << viewport.PickPixelY << ' '
               << yesNo(viewport.PickRectVirtual) << '\n'
               << "ViewportRect " << viewport.RectX << ' ' << viewport.RectY << ' ' << viewport.RectWidth
               << ' ' << viewport.RectHeight << ' ' << viewport.RectAspect << ' '
               << viewport.RectFovDegrees << '\n'
               << "ViewportFocus " << viewport.FocusState << ' ' << viewport.FocusSubject << ' '
               << yesNo(viewport.FocusAnimated) << ' ' << viewport.FocusMargin << '\n';
        writePose("ViewportFocusBefore", viewport.FocusBefore);
        writePose("ViewportFocusAfter", viewport.FocusAfter);
        stream << "ViewportFocusBounds " << viewport.FocusCenter[0] << ' ' << viewport.FocusCenter[1]
               << ' ' << viewport.FocusCenter[2] << ' ' << viewport.FocusRadius << ' '
               << viewport.FocusDistance << '\n';
        stream.precision(previousPrecision);
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
        const auto writeTransform = [](std::ostringstream& stream,
            std::string_view label, const Engine::TransformComponent& transform)
        {
            const Engine::Math::SectorLocalPosition& position = transform.GetPosition();
            stream << label << ' ' << position.Sector.X << ' ' << position.Sector.Y << ' '
                   << position.Sector.Z << ' ' << position.Local.X << ' ' << position.Local.Y << ' '
                   << position.Local.Z << ' ' << transform.RotationDegrees.X << ' '
                   << transform.RotationDegrees.Y << ' ' << transform.RotationDegrees.Z << ' '
                   << transform.Scale.X << ' ' << transform.Scale.Y << ' ' << transform.Scale.Z << '\n';
        };
        const auto writeCamera = [](std::ostringstream& stream,
            std::string_view label, const Engine::CameraComponent& camera)
        {
            stream << label << ' ' << (camera.Primary ? "yes" : "no") << ' '
                   << camera.Projection.VerticalFovDegrees << ' ' << camera.Projection.NearClip << ' '
                   << camera.Projection.FarClip << ' ' << camera.BackgroundColor.X << ' '
                   << camera.BackgroundColor.Y << ' ' << camera.BackgroundColor.Z << '\n';
        };
        const auto writeLight = [](std::ostringstream& stream,
            std::string_view label, const Engine::LightComponent& light)
        {
            stream << label << ' ' << Engine::ToString(light.Type) << ' '
                   << light.Color.X << ' ' << light.Color.Y << ' ' << light.Color.Z << ' '
                   << light.PhotometricValue << ' ' << Engine::ToString(light.PhotometricUnit) << ' '
                   << light.Range << ' ' << light.InnerConeDegrees << ' '
                   << light.OuterConeDegrees << ' ' << (light.CastsShadows ? "yes" : "no") << '\n';
        };
        const auto writeMeshRenderer = [](std::ostringstream& stream,
            std::string_view label, const Engine::MeshRendererComponent& meshRenderer)
        {
            stream << label << ' ' << meshRenderer.MeshAsset << ' ' << meshRenderer.MaterialAsset << ' '
                   << std::quoted(meshRenderer.MeshName) << ' '
                   << (meshRenderer.Visible ? "yes" : "no") << ' '
                   << (meshRenderer.CastsShadows ? "yes" : "no") << '\n';
        };
        const auto writeColorPipeline = [](std::ostringstream& stream,
            std::string_view label, const Engine::RendererColorPipelineSettings& settings)
        {
            stream << label << ' ' << settings.ManualExposureEV100 << ' '
                   << settings.PostToneMapSaturation << ' ' << settings.PostToneMapContrast << ' '
                   << Engine::ToString(settings.ExposureMode) << ' '
                   << settings.CameraApertureFNumber << ' ' << settings.CameraShutterSeconds << ' '
                   << settings.CameraISO << '\n';
        };
        const auto writeDebugVisualization = [](std::ostringstream& stream,
            std::string_view label, Engine::SceneDebugView view,
            bool showSelectedBounds)
        {
            stream << label << ' ' << Engine::ToString(view) << ' '
                   << (showSelectedBounds ? "yes" : "no") << '\n';
        };

        std::ostringstream stream;
        stream << std::setprecision(std::numeric_limits<double>::max_digits10)
               << kReceiptHeader << '\n'
               << "RequestId " << std::quoted(receipt.RequestId) << '\n'
               << "SessionId " << std::quoted(receipt.SessionId) << '\n'
               << "ProjectPath " << std::quoted(receipt.ProjectPath) << '\n'
               << "RequestDigest " << receipt.RequestDigest << '\n'
               << "Action " << (receipt.ActionKnown ? ToString(receipt.Action) : "Unknown") << '\n'
               << "Status " << (receipt.Succeeded ? "Succeeded" : "Rejected") << '\n'
               << "Reason " << std::quoted(receipt.Reason) << '\n'
               << "Frame " << receipt.Frame << '\n'
               << "Effect " << receipt.Effect << '\n'
               << "Recovery " << receipt.Recovery << '\n'
               << "Persistence " << receipt.Persistence << '\n'
               << "Saved " << (receipt.Saved ? "yes" : "no") << '\n'
               << "EntityId " << receipt.EntityId << '\n'
               << "EntityName " << std::quoted(receipt.EntityName) << '\n'
               << "MainCameraEntityId " << receipt.MainCameraEntityId << '\n'
               << "IsMainCamera " << (receipt.IsMainCamera ? "yes" : "no") << '\n'
               << "SelectedEntityIdBefore " << receipt.SelectedEntityIdBefore << '\n'
               << "SelectedEntityIdAfter " << receipt.SelectedEntityIdAfter << '\n'
               << "MaterialHandle " << receipt.MaterialHandle << '\n';
        writeSurface(stream, "BeforeSurface", receipt.Before);
        writeSurface(stream, "AfterSurface", receipt.After);
        writeTransform(stream, "BeforeTransform", receipt.BeforeTransform);
        writeTransform(stream, "AfterTransform", receipt.AfterTransform);
        stream << "BeforeCameraPresent " << (receipt.BeforeCameraPresent ? "yes" : "no") << '\n';
        writeCamera(stream, "BeforeCamera", receipt.BeforeCamera);
        stream << "AfterCameraPresent " << (receipt.AfterCameraPresent ? "yes" : "no") << '\n';
        writeCamera(stream, "AfterCamera", receipt.AfterCamera);
        stream << "BeforeLightPresent " << (receipt.BeforeLightPresent ? "yes" : "no") << '\n';
        writeLight(stream, "BeforeLight", receipt.BeforeLight);
        stream << "AfterLightPresent " << (receipt.AfterLightPresent ? "yes" : "no") << '\n';
        writeLight(stream, "AfterLight", receipt.AfterLight);
        stream << "BeforeMeshRendererPresent "
               << (receipt.BeforeMeshRendererPresent ? "yes" : "no") << '\n';
        writeMeshRenderer(stream, "BeforeMeshRenderer", receipt.BeforeMeshRenderer);
        stream << "AfterMeshRendererPresent "
               << (receipt.AfterMeshRendererPresent ? "yes" : "no") << '\n';
        writeMeshRenderer(stream, "AfterMeshRenderer", receipt.AfterMeshRenderer);
        writeColorPipeline(stream, "BeforeColorPipeline", receipt.BeforeColorPipeline);
        writeColorPipeline(stream, "AfterColorPipeline", receipt.AfterColorPipeline);
        writeDebugVisualization(stream, "BeforeDebugVisualization",
            receipt.BeforeDebugView, receipt.BeforeShowSelectedBounds);
        writeDebugVisualization(stream, "AfterDebugVisualization",
            receipt.AfterDebugView, receipt.AfterShowSelectedBounds);
        stream << "AffectedEntityCount " << receipt.AffectedEntityCount << '\n'
               << "AffectedEntitySampleCount " << receipt.AffectedEntityIds.size() << '\n'
               << "AffectedEntityIds";
        for (Engine::EntityId entity : receipt.AffectedEntityIds)
            stream << ' ' << entity;
        stream << '\n'
               << "AffectedEntityIdsTruncated "
               << (receipt.AffectedEntityIdsTruncated ? "yes" : "no") << '\n'
               << "RendererGeneration " << receipt.RendererGeneration << '\n'
               << "DebugVisualizationGeneration "
               << receipt.DebugVisualizationGeneration << '\n'
               << "UndoDepthBefore " << receipt.UndoDepthBefore << '\n'
               << "UndoDepthAfter " << receipt.UndoDepthAfter << '\n'
               << "RedoDepthBefore " << receipt.RedoDepthBefore << '\n'
               << "RedoDepthAfter " << receipt.RedoDepthAfter << '\n'
               << "SelectionCommitted " << (receipt.SelectionCommitted ? "yes" : "no") << '\n'
               << "PivotRetargeted " << (receipt.PivotRetargeted ? "yes" : "no") << '\n'
               << "RendererReadbackVerified "
               << (receipt.RendererReadbackVerified ? "yes" : "no") << '\n'
               << "PostconditionVerified "
               << (receipt.PostconditionVerified ? "yes" : "no") << '\n'
               << "RollbackVerified " << (receipt.RollbackVerified ? "yes" : "no") << '\n'
               << "EditorCameraSynchronized "
               << (receipt.EditorCameraSynchronized ? "yes" : "no") << '\n';
        WriteFabBlock(stream, receipt.Fab);
        WriteViewportBlock(stream, receipt.Viewport);
        return stream.str();
    }

    bool ParseRequest(std::string_view contents, std::string_view expectedRequestId,
        EditorMaterialControlRequest& request, std::string& error)
    {
        std::istringstream input { std::string(contents) };
        std::string line;
        if (!std::getline(input, line) || line != kRequestHeader)
        {
            error = "unsupported_schema_expected_v5";
            return false;
        }

        enum Field : Engine::u64
        {
            RequestId = 1ull << 0,
            SessionId = 1ull << 1,
            ProjectPath = 1ull << 2,
            Action = 1ull << 3,
            EntityId = 1ull << 4,
            ExpectedEntityName = 1ull << 5,
            MaterialHandle = 1ull << 6,
            ExpectedSurface = 1ull << 7,
            NewSurface = 1ull << 8,
            Scope = 1ull << 9,
            ExpectedTransform = 1ull << 10,
            NewTransform = 1ull << 11,
            ExpectedLight = 1ull << 12,
            NewLight = 1ull << 13,
            ExpectedColorPipeline = 1ull << 14,
            NewColorPipeline = 1ull << 15,
            ExpectedSelectedEntityId = 1ull << 16,
            ExpectedDebugVisualization = 1ull << 17,
            NewDebugVisualization = 1ull << 18,
            ExpectedMeshRendererFlags = 1ull << 19,
            NewMeshRendererFlags = 1ull << 20,
            ExpectedFabJobId = 1ull << 21,
            InboxName = 1ull << 22,
            ExpectedSourceKind = 1ull << 23,
            ExpectedSourceSha256 = 1ull << 24,
            ProductIdentity = 1ull << 25,
            ProductName = 1ull << 26,
            Publisher = 1ull << 27,
            VersionOrDownloadLabel = 1ull << 28,
            LicenseFamily = 1ull << 29,
            LicenseTier = 1ull << 30,
            AttributionText = 1ull << 31,
            AttributionLink = 1ull << 32,
            NoAI = 1ull << 33,
            GeneratedWithAI = 1ull << 34,
            RawSourcePolicy = 1ull << 35,
            ExpectedProvenanceDigest = 1ull << 36,
            ExpectedGenerationId = 1ull << 37,
            ExpectedRelation = 1ull << 38,
            ExpectedMeshAsset = 1ull << 39,
            ExpectedMaterialAsset = 1ull << 40,
            NewMeshAsset = 1ull << 41,
            NewMaterialAsset = 1ull << 42,
            MeshAsset = 1ull << 43,
            ExpectedManifestSha256 = 1ull << 44,
            PanelVisible = 1ull << 45,
            ViewportPoint = 1ull << 46,
            FocusAnimation = 1ull << 47
        };
        Engine::u64 seen = 0;
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
            else if (key == "ProjectPath")
            {
                if (!claim(Field::ProjectPath) || !ParseQuoted(fieldStream, request.ProjectPath))
                {
                    error = "invalid_or_duplicate_project_path";
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
                else if (action == "InspectEntity") request.Action = EditorMaterialControlAction::InspectEntity;
                else if (action == "SelectEntity") request.Action = EditorMaterialControlAction::SelectEntity;
                else if (action == "SetEntityTransform") request.Action = EditorMaterialControlAction::SetEntityTransform;
                else if (action == "SetTypedLight") request.Action = EditorMaterialControlAction::SetTypedLight;
                else if (action == "SetProjectColorPipeline") request.Action = EditorMaterialControlAction::SetProjectColorPipeline;
                else if (action == "SetViewportMainCameraPose") request.Action = EditorMaterialControlAction::SetViewportMainCameraPose;
                else if (action == "SetSceneDebugVisualization") request.Action = EditorMaterialControlAction::SetSceneDebugVisualization;
                else if (action == "SetMeshRendererFlags") request.Action = EditorMaterialControlAction::SetMeshRendererFlags;
                else if (action == "PickAtViewportPoint") request.Action = EditorMaterialControlAction::PickAtViewportPoint;
                else if (action == "FocusSelection") request.Action = EditorMaterialControlAction::FocusSelection;
                else if (action == "InspectFabImport") request.Action = EditorMaterialControlAction::InspectFabImport;
                else if (action == "SelectFabPackage") request.Action = EditorMaterialControlAction::SelectFabPackage;
                else if (action == "SetFabProvenance") request.Action = EditorMaterialControlAction::SetFabProvenance;
                else if (action == "ConfirmFabProvenance") request.Action = EditorMaterialControlAction::ConfirmFabProvenance;
                else if (action == "CommitFabImport") request.Action = EditorMaterialControlAction::CommitFabImport;
                else if (action == "CancelFabImport") request.Action = EditorMaterialControlAction::CancelFabImport;
                else if (action == "DismissFabImport") request.Action = EditorMaterialControlAction::DismissFabImport;
                else if (action == "PlaceMeshAsset") request.Action = EditorMaterialControlAction::PlaceMeshAsset;
                else if (action == "SetEntityMeshRendererAssets") request.Action = EditorMaterialControlAction::SetEntityMeshRendererAssets;
                else if (action == "SaveProjectState") request.Action = EditorMaterialControlAction::SaveProjectState;
                else if (action == "ValidateProject") request.Action = EditorMaterialControlAction::ValidateProject;
                else if (action == "SetFabPanelVisible") request.Action = EditorMaterialControlAction::SetFabPanelVisible;
                else if (action == "InspectFabPanel") request.Action = EditorMaterialControlAction::InspectFabPanel;
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
            else if (key == "ExpectedTransform")
            {
                if (!claim(Field::ExpectedTransform) || !ParseTransform(fieldStream,
                        request.ExpectedTransformPosition, request.ExpectedTransform.RotationDegrees,
                        request.ExpectedTransform.Scale)) { error = "invalid_expected_transform"; return false; }
                request.HasExpectedTransform = true;
            }
            else if (key == "NewTransform")
            {
                if (!claim(Field::NewTransform) || !ParseTransform(fieldStream,
                        request.NewTransformPosition, request.NewTransform.RotationDegrees,
                        request.NewTransform.Scale)) { error = "invalid_new_transform"; return false; }
                request.HasNewTransform = true;
            }
            else if (key == "ExpectedLight")
            {
                if (!claim(Field::ExpectedLight) || !ParseLight(fieldStream, request.ExpectedLight)) { error = "invalid_expected_light"; return false; }
                request.HasExpectedLight = true;
            }
            else if (key == "NewLight")
            {
                if (!claim(Field::NewLight) || !ParseLight(fieldStream, request.NewLight)) { error = "invalid_new_light"; return false; }
                request.HasNewLight = true;
            }
            else if (key == "ExpectedColorPipeline")
            {
                if (!claim(Field::ExpectedColorPipeline) || !ParseColorPipeline(fieldStream, request.ExpectedColorPipeline)) { error = "invalid_expected_color_pipeline"; return false; }
                request.HasExpectedColorPipeline = true;
            }
            else if (key == "NewColorPipeline")
            {
                if (!claim(Field::NewColorPipeline) || !ParseColorPipeline(fieldStream, request.NewColorPipeline)) { error = "invalid_new_color_pipeline"; return false; }
                request.HasNewColorPipeline = true;
            }
            else if (key == "ExpectedSelectedEntityId")
            {
                if (!claim(Field::ExpectedSelectedEntityId)
                    || !ParseInteger(fieldStream, request.ExpectedSelectedEntityId))
                {
                    error = "invalid_or_duplicate_expected_selected_entity_id";
                    return false;
                }
                request.HasExpectedSelectedEntityId = true;
            }
            else if (key == "ExpectedDebugVisualization")
            {
                if (!claim(Field::ExpectedDebugVisualization)
                    || !ParseDebugVisualization(fieldStream,
                        request.ExpectedDebugView,
                        request.ExpectedShowSelectedBounds))
                {
                    error = "invalid_expected_debug_visualization";
                    return false;
                }
                request.HasExpectedDebugVisualization = true;
            }
            else if (key == "NewDebugVisualization")
            {
                if (!claim(Field::NewDebugVisualization)
                    || !ParseDebugVisualization(fieldStream,
                        request.NewDebugView, request.NewShowSelectedBounds))
                {
                    error = "invalid_new_debug_visualization";
                    return false;
                }
                request.HasNewDebugVisualization = true;
            }
            else if (key == "ExpectedMeshRendererFlags")
            {
                if (!claim(Field::ExpectedMeshRendererFlags)
                    || !ParseMeshRendererFlags(fieldStream,
                        request.ExpectedMeshVisible,
                        request.ExpectedMeshCastsShadows))
                {
                    error = "invalid_expected_mesh_renderer_flags";
                    return false;
                }
                request.HasExpectedMeshRendererFlags = true;
            }
            else if (key == "NewMeshRendererFlags")
            {
                if (!claim(Field::NewMeshRendererFlags)
                    || !ParseMeshRendererFlags(fieldStream,
                        request.NewMeshVisible,
                        request.NewMeshCastsShadows))
                {
                    error = "invalid_new_mesh_renderer_flags";
                    return false;
                }
                request.HasNewMeshRendererFlags = true;
            }
            else if (key == "ExpectedFabJobId")
            {
                if (!claim(Field::ExpectedFabJobId)
                    || !ParseInteger(fieldStream, request.Fab.ExpectedJobId))
                {
                    error = "invalid_or_duplicate_expected_fab_job_id";
                    return false;
                }
                request.Fab.HasExpectedJobId = true;
            }
            else if (key == "InboxName")
            {
                if (!claim(Field::InboxName)
                    || !ParseQuotedBounded(fieldStream, request.Fab.InboxName, 128))
                {
                    error = "invalid_or_duplicate_inbox_name";
                    return false;
                }
            }
            else if (key == "ExpectedSourceKind")
            {
                if (!claim(Field::ExpectedSourceKind)
                    || !ParseWord(fieldStream, request.Fab.ExpectedSourceKind, 16)
                    || (request.Fab.ExpectedSourceKind != "zip" && request.Fab.ExpectedSourceKind != "glb"
                        && request.Fab.ExpectedSourceKind != "gltf" && request.Fab.ExpectedSourceKind != "directory"))
                {
                    error = "invalid_or_duplicate_expected_source_kind";
                    return false;
                }
            }
            else if (key == "ExpectedSourceSha256")
            {
                if (!claim(Field::ExpectedSourceSha256)
                    || !ParseHex64(fieldStream, request.Fab.ExpectedSourceSha256))
                {
                    error = "invalid_or_duplicate_expected_source_sha256";
                    return false;
                }
            }
            else if (key == "ProductIdentity")
            {
                if (!claim(Field::ProductIdentity)
                    || !ParseQuotedBounded(fieldStream, request.Fab.ProductIdentity, 512))
                {
                    error = "invalid_or_duplicate_product_identity";
                    return false;
                }
            }
            else if (key == "ProductName")
            {
                if (!claim(Field::ProductName)
                    || !ParseQuotedBounded(fieldStream, request.Fab.ProductName, 256))
                {
                    error = "invalid_or_duplicate_product_name";
                    return false;
                }
            }
            else if (key == "Publisher")
            {
                if (!claim(Field::Publisher)
                    || !ParseQuotedBounded(fieldStream, request.Fab.Publisher, 256))
                {
                    error = "invalid_or_duplicate_publisher";
                    return false;
                }
            }
            else if (key == "VersionOrDownloadLabel")
            {
                if (!claim(Field::VersionOrDownloadLabel)
                    || !ParseQuotedBounded(fieldStream, request.Fab.VersionOrDownloadLabel, 128))
                {
                    error = "invalid_or_duplicate_version_label";
                    return false;
                }
            }
            else if (key == "LicenseFamily")
            {
                std::string text;
                static constexpr std::array<Engine::FabLicenseFamily, 5> families = {
                    Engine::FabLicenseFamily::Unknown, Engine::FabLicenseFamily::FabStandard,
                    Engine::FabLicenseFamily::CreativeCommonsAttribution,
                    Engine::FabLicenseFamily::LegacyUnrealMarketplace,
                    Engine::FabLicenseFamily::ReferenceOnly };
                if (!claim(Field::LicenseFamily) || !ParseWord(fieldStream, text)
                    || !ParseFabEnum(text, families, request.Fab.LicenseFamily))
                {
                    error = "invalid_or_duplicate_license_family";
                    return false;
                }
            }
            else if (key == "LicenseTier")
            {
                std::string text;
                static constexpr std::array<Engine::FabLicenseTier, 4> tiers = {
                    Engine::FabLicenseTier::Unknown, Engine::FabLicenseTier::NotApplicable,
                    Engine::FabLicenseTier::Personal, Engine::FabLicenseTier::Professional };
                if (!claim(Field::LicenseTier) || !ParseWord(fieldStream, text)
                    || !ParseFabEnum(text, tiers, request.Fab.LicenseTier))
                {
                    error = "invalid_or_duplicate_license_tier";
                    return false;
                }
            }
            else if (key == "AttributionText")
            {
                if (!claim(Field::AttributionText)
                    || !ParseQuotedBounded(fieldStream, request.Fab.AttributionText,
                        kEditorFabControlMaximumAttributionBytes))
                {
                    error = "invalid_duplicate_or_oversized_attribution_text";
                    return false;
                }
            }
            else if (key == "AttributionLink")
            {
                if (!claim(Field::AttributionLink)
                    || !ParseQuotedBounded(fieldStream, request.Fab.AttributionLink, 512))
                {
                    error = "invalid_or_duplicate_attribution_link";
                    return false;
                }
            }
            else if (key == "NoAI" || key == "GeneratedWithAI")
            {
                const bool noAI = key == "NoAI";
                std::string text;
                static constexpr std::array<Engine::FabMetadataFlag, 3> flags = {
                    Engine::FabMetadataFlag::Unknown, Engine::FabMetadataFlag::No,
                    Engine::FabMetadataFlag::Yes };
                Engine::FabMetadataFlag& target = noAI ? request.Fab.NoAI : request.Fab.GeneratedWithAI;
                if (!claim(noAI ? Field::NoAI : Field::GeneratedWithAI) || !ParseWord(fieldStream, text)
                    || !ParseFabEnum(text, flags, target))
                {
                    error = noAI ? "invalid_or_duplicate_no_ai" : "invalid_or_duplicate_generated_with_ai";
                    return false;
                }
            }
            else if (key == "RawSourcePolicy")
            {
                std::string text;
                static constexpr std::array<Engine::FabRawSourcePolicy, 3> policies = {
                    Engine::FabRawSourcePolicy::Unknown, Engine::FabRawSourcePolicy::ExcludedFromProject,
                    Engine::FabRawSourcePolicy::PrivateProjectOnly };
                if (!claim(Field::RawSourcePolicy) || !ParseWord(fieldStream, text)
                    || !ParseFabEnum(text, policies, request.Fab.RawSourcePolicy))
                {
                    error = "invalid_or_duplicate_raw_source_policy";
                    return false;
                }
            }
            else if (key == "ExpectedProvenanceDigest")
            {
                if (!claim(Field::ExpectedProvenanceDigest)
                    || !ParseHex64(fieldStream, request.Fab.ExpectedProvenanceDigest))
                {
                    error = "invalid_or_duplicate_expected_provenance_digest";
                    return false;
                }
            }
            else if (key == "ExpectedGenerationId")
            {
                if (!claim(Field::ExpectedGenerationId)
                    || !ParseHex64(fieldStream, request.Fab.ExpectedGenerationId))
                {
                    error = "invalid_or_duplicate_expected_generation_id";
                    return false;
                }
            }
            else if (key == "ExpectedRelation")
            {
                if (!claim(Field::ExpectedRelation)
                    || !ParseWord(fieldStream, request.Fab.ExpectedRelation, 32))
                {
                    error = "invalid_or_duplicate_expected_relation";
                    return false;
                }
            }
            else if (key == "ExpectedMeshAsset" || key == "ExpectedMaterialAsset"
                || key == "NewMeshAsset" || key == "NewMaterialAsset" || key == "MeshAsset")
            {
                Field field = Field::MeshAsset;
                Engine::AssetHandle* target = &request.Fab.MeshAsset;
                if (key == "ExpectedMeshAsset")
                {
                    field = Field::ExpectedMeshAsset;
                    target = &request.Fab.ExpectedMeshAsset;
                }
                else if (key == "ExpectedMaterialAsset")
                {
                    field = Field::ExpectedMaterialAsset;
                    target = &request.Fab.ExpectedMaterialAsset;
                }
                else if (key == "NewMeshAsset")
                {
                    field = Field::NewMeshAsset;
                    target = &request.Fab.NewMeshAsset;
                }
                else if (key == "NewMaterialAsset")
                {
                    field = Field::NewMaterialAsset;
                    target = &request.Fab.NewMaterialAsset;
                }
                if (!claim(field) || !ParseInteger(fieldStream, *target))
                {
                    error = "invalid_or_duplicate_asset_handle";
                    return false;
                }
            }
            else if (key == "ExpectedManifestSha256")
            {
                if (!claim(Field::ExpectedManifestSha256)
                    || !ParseHex64(fieldStream, request.Fab.ExpectedManifestSha256))
                {
                    error = "invalid_or_duplicate_expected_manifest_sha256";
                    return false;
                }
            }
            else if (key == "PanelVisible")
            {
                std::string text;
                if (!claim(Field::PanelVisible) || !(fieldStream >> text) || !AtEnd(fieldStream)
                    || (text != "yes" && text != "no"))
                {
                    error = "invalid_or_duplicate_panel_visible";
                    return false;
                }
                request.Fab.PanelVisible = text == "yes";
            }
            else if (key == "ViewportPoint")
            {
                if (!claim(Field::ViewportPoint)
                    || !ParseViewportPoint(fieldStream, request.ViewportNormalizedX,
                        request.ViewportNormalizedY))
                {
                    error = "invalid_or_duplicate_viewport_point";
                    return false;
                }
                request.HasViewportPoint = true;
            }
            else if (key == "FocusAnimation")
            {
                std::string text;
                if (!claim(Field::FocusAnimation) || !(fieldStream >> text) || !AtEnd(fieldStream)
                    || (text != "yes" && text != "no"))
                {
                    error = "invalid_or_duplicate_focus_animation";
                    return false;
                }
                request.FocusAnimate = text == "yes";
                request.HasFocusAnimation = true;
            }
            else
            {
                error = "unknown_field";
                return false;
            }
        }

        constexpr Engine::u64 common = Field::RequestId | Field::SessionId
            | Field::ProjectPath | Field::Action;
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
        if (request.ProjectPath.empty() || request.ProjectPath.size() > 4096)
        {
            error = "invalid_identity";
            return false;
        }
        const bool commitAssignment = request.Action == EditorMaterialControlAction::CommitFabImport
            && (seen & (Field::EntityId | Field::ExpectedEntityName | Field::ExpectedMeshAsset
                   | Field::ExpectedMaterialAsset)) != 0;
        const bool needsEntity = (request.Action != EditorMaterialControlAction::SetProjectColorPipeline
                && request.Action != EditorMaterialControlAction::SetSceneDebugVisualization
                && !IsViewportControlAction(request.Action)
                && !IsFabControlAction(request.Action))
            || request.Action == EditorMaterialControlAction::SetEntityMeshRendererAssets
            || commitAssignment;
        if (needsEntity && (request.EntityId == Engine::kInvalidEntityId
            || request.ExpectedEntityName.empty() || request.ExpectedEntityName.size() > 256))
        { error = "invalid_entity_identity"; return false; }
        const Engine::u64 entityIdentity = Field::EntityId | Field::ExpectedEntityName;
        // Required fields must all be present; optional fields may be present.
        // Every other action uses an empty optional set, so its mask is exact.
        Engine::u64 exactFields = common;
        Engine::u64 optionalFields = 0;
        switch (request.Action)
        {
            case EditorMaterialControlAction::InspectMaterialSurface:
                exactFields |= entityIdentity | Field::MaterialHandle;
                break;
            case EditorMaterialControlAction::SelectEntityPatchMaterialSurface:
                exactFields |= entityIdentity | Field::MaterialHandle | Field::ExpectedSurface
                    | Field::NewSurface | Field::Scope;
                if (!Engine::IsValidMaterialSurface(request.ExpectedSurface)
                    || !Engine::IsValidMaterialSurface(request.NewSurface))
                {
                    error = "invalid_or_missing_surface";
                    return false;
                }
                break;
            case EditorMaterialControlAction::InspectEntity:
                exactFields |= entityIdentity;
                break;
            case EditorMaterialControlAction::SelectEntity:
                exactFields |= entityIdentity | Field::ExpectedSelectedEntityId;
                break;
            case EditorMaterialControlAction::SetEntityTransform:
            case EditorMaterialControlAction::SetViewportMainCameraPose:
                exactFields |= entityIdentity | Field::ExpectedTransform | Field::NewTransform;
                break;
            case EditorMaterialControlAction::SetTypedLight:
                exactFields |= entityIdentity | Field::ExpectedLight | Field::NewLight;
                break;
            case EditorMaterialControlAction::SetProjectColorPipeline:
                exactFields |= Field::ExpectedColorPipeline | Field::NewColorPipeline;
                break;
            case EditorMaterialControlAction::SetSceneDebugVisualization:
                exactFields |= Field::ExpectedSelectedEntityId
                    | Field::ExpectedDebugVisualization
                    | Field::NewDebugVisualization;
                break;
            case EditorMaterialControlAction::SetMeshRendererFlags:
                exactFields |= entityIdentity | Field::ExpectedMeshRendererFlags
                    | Field::NewMeshRendererFlags;
                break;
            case EditorMaterialControlAction::PickAtViewportPoint:
                exactFields |= Field::ViewportPoint | Field::ExpectedSelectedEntityId;
                break;
            case EditorMaterialControlAction::FocusSelection:
                exactFields |= Field::FocusAnimation | Field::ExpectedSelectedEntityId;
                break;
            case EditorMaterialControlAction::InspectFabImport:
                optionalFields = Field::ExpectedFabJobId;
                break;
            case EditorMaterialControlAction::SelectFabPackage:
                exactFields |= Field::InboxName | Field::ExpectedSourceKind;
                optionalFields = Field::ExpectedSourceSha256;
                break;
            case EditorMaterialControlAction::SetFabProvenance:
                exactFields |= Field::ExpectedFabJobId | Field::ProductIdentity | Field::ProductName
                    | Field::Publisher | Field::VersionOrDownloadLabel | Field::LicenseFamily
                    | Field::LicenseTier | Field::NoAI | Field::GeneratedWithAI | Field::RawSourcePolicy;
                optionalFields = Field::AttributionText | Field::AttributionLink;
                break;
            case EditorMaterialControlAction::ConfirmFabProvenance:
                exactFields |= Field::ExpectedFabJobId | Field::ExpectedProvenanceDigest;
                break;
            case EditorMaterialControlAction::CommitFabImport:
                exactFields |= Field::ExpectedFabJobId | Field::ExpectedGenerationId
                    | Field::ExpectedRelation;
                optionalFields = entityIdentity | Field::ExpectedMeshAsset | Field::ExpectedMaterialAsset;
                break;
            case EditorMaterialControlAction::CancelFabImport:
            case EditorMaterialControlAction::DismissFabImport:
                exactFields |= Field::ExpectedFabJobId;
                break;
            case EditorMaterialControlAction::PlaceMeshAsset:
                exactFields |= Field::MeshAsset | Field::ExpectedSelectedEntityId;
                break;
            case EditorMaterialControlAction::SetEntityMeshRendererAssets:
                exactFields |= entityIdentity | Field::ExpectedMeshAsset | Field::ExpectedMaterialAsset
                    | Field::NewMeshAsset | Field::NewMaterialAsset;
                break;
            case EditorMaterialControlAction::SaveProjectState:
                optionalFields = Field::ExpectedManifestSha256;
                break;
            case EditorMaterialControlAction::SetFabPanelVisible:
                exactFields |= Field::PanelVisible;
                break;
            case EditorMaterialControlAction::ValidateProject:
            case EditorMaterialControlAction::InspectFabPanel:
                break;
        }
        if ((seen & exactFields) != exactFields || (seen & ~(exactFields | optionalFields)) != 0)
        {
            error = "missing_or_unexpected_action_field";
            return false;
        }
        if (request.Action == EditorMaterialControlAction::CommitFabImport)
        {
            const Engine::u64 assignmentFields = Field::EntityId | Field::ExpectedEntityName
                | Field::ExpectedMeshAsset | Field::ExpectedMaterialAsset;
            if ((seen & assignmentFields) != 0 && (seen & assignmentFields) != assignmentFields)
            {
                error = "incomplete_assignment_fields";
                return false;
            }
            request.Fab.HasAssignment = (seen & assignmentFields) == assignmentFields;
        }
        if ((request.Action == EditorMaterialControlAction::InspectMaterialSurface
                || request.Action == EditorMaterialControlAction::SelectEntityPatchMaterialSurface)
            && request.MaterialHandle == Engine::kInvalidAssetHandle)
        {
            error = "invalid_material_identity";
            return false;
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
    m_FabInbox = root / "fab-inbox";
    const auto abandonInitialization = [this, &root]()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        m_Root.clear();
        m_Requests.clear();
        m_Responses.clear();
        m_FabInbox.clear();
        m_SessionId.clear();
        m_ProjectPath.clear();
        m_ProcessId = 0;
        m_AcceptingRequests = false;
        m_DurabilityDegradationCount = 0;
        m_ForceParentDirectorySyncFailureOnce = false;
    };
    for (const std::filesystem::path& directory : { m_Requests, m_Responses, m_FabInbox })
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
    Engine::Log::Info("EditorMaterialControlV5 state=ready session=", m_SessionId,
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
    Engine::Log::Info("EditorMaterialControlV5 state=closed session=", m_SessionId,
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
             << "RequestSchema 5\nReceiptSchema 5\n"
             << "Actions InspectMaterialSurface,SelectEntityPatchMaterialSurface,InspectEntity,SelectEntity,SetEntityTransform,SetTypedLight,SetProjectColorPipeline,SetViewportMainCameraPose,SetSceneDebugVisualization,SetMeshRendererFlags,PickAtViewportPoint,FocusSelection,InspectFabImport,SelectFabPackage,SetFabProvenance,ConfirmFabProvenance,CommitFabImport,CancelFabImport,DismissFabImport,PlaceMeshAsset,SetEntityMeshRendererAssets,SaveProjectState,ValidateProject,SetFabPanelVisible,InspectFabPanel\n"
             << "FabInbox " << std::quoted(m_FabInbox.string()) << '\n'
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
    Engine::Log::Info("EditorMaterialControlV5 accepting=no reason=", reason,
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
    receipt.ProjectPath = m_ProjectPath;
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
    if (rejection.empty() && request.ProjectPath != m_ProjectPath)
        rejection = "wrong_project";

    EditorMaterialControlTransaction transaction;
    if (rejection.empty())
        transaction = handler(request, frame);
    else
        transaction.Receipt.Reason = rejection;
    EditorMaterialControlReceipt& receipt = transaction.Receipt;
    receipt.RequestId = requestId;
    receipt.SessionId = m_SessionId;
    receipt.ProjectPath = m_ProjectPath;
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
        receipt.PostconditionVerified = false;
        receipt.RollbackVerified = false;
        receipt.EditorCameraSynchronized = false;
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
        receipt.AfterTransform = receipt.BeforeTransform;
        receipt.AfterCameraPresent = receipt.BeforeCameraPresent;
        receipt.AfterCamera = receipt.BeforeCamera;
        receipt.AfterLightPresent = receipt.BeforeLightPresent;
        receipt.AfterLight = receipt.BeforeLight;
        receipt.AfterMeshRendererPresent = receipt.BeforeMeshRendererPresent;
        receipt.AfterMeshRenderer = receipt.BeforeMeshRenderer;
        receipt.AfterColorPipeline = receipt.BeforeColorPipeline;
        receipt.AfterDebugView = receipt.BeforeDebugView;
        receipt.AfterShowSelectedBounds = receipt.BeforeShowSelectedBounds;
        receipt.SelectedEntityIdAfter = receipt.SelectedEntityIdBefore;
        receipt.SelectionCommitted = false;
        receipt.PivotRetargeted = false;
        receipt.PostconditionVerified = false;
        receipt.RollbackVerified = true;
        receipt.EditorCameraSynchronized = false;
        receipt.Viewport = {};
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

bool EditorMaterialControlMailbox::PublishSceneControlTargetForSmoke(
    std::string_view contents, std::string& error)
{
    if (!IsOpen() || contents.empty() || contents.size() > MaximumResponseBytes)
    {
        error = "invalid_scene_control_smoke_target";
        return false;
    }
    const std::filesystem::path temporary = m_Root
        / (".scene-control-target." + std::to_string(++m_TemporarySequence) + ".tmp");
    return WriteOwnerOnlyTemporary(temporary, contents, error)
        && PublishFileNoReplace(temporary, m_Root / "scene-control-target.info",
            "scene-control smoke target", true, error);
}

bool EditorMaterialControlMailbox::PublishFabControlTargetForSmoke(
    std::string_view contents, std::string& error)
{
    if (!IsOpen() || contents.empty() || contents.size() > MaximumResponseBytes)
    {
        error = "invalid_fab_control_smoke_target";
        return false;
    }
    const std::filesystem::path temporary = m_Root
        / (".fab-control-target." + std::to_string(++m_TemporarySequence) + ".tmp");
    return WriteOwnerOnlyTemporary(temporary, contents, error)
        && PublishFileNoReplace(temporary, m_Root / "fab-control-target.info",
            "fab-control smoke target", true, error);
}

bool EditorMaterialControlMailbox::PublishViewportPickingTargetForSmoke(
    std::string_view contents, std::string& error)
{
    if (!IsOpen() || contents.empty() || contents.size() > MaximumResponseBytes)
    {
        error = "invalid_viewport_picking_smoke_target";
        return false;
    }
    const std::filesystem::path temporary = m_Root
        / (".viewport-picking-target." + std::to_string(++m_TemporarySequence) + ".tmp");
    return WriteOwnerOnlyTemporary(temporary, contents, error)
        && PublishFileNoReplace(temporary, m_Root / "viewport-picking-target.info",
            "viewport-picking smoke target", true, error);
}

std::string EditorMaterialControlMailbox::FormatInspectRequest(std::string_view requestId,
    std::string_view sessionId, std::string_view projectPath, Engine::EntityId entityId,
    std::string_view expectedEntityName, Engine::AssetHandle materialHandle)
{
    const std::string canonicalProjectPath = CanonicalProjectPath(projectPath);
    std::ostringstream stream;
    stream << kRequestHeader << '\n'
           << "RequestId " << std::quoted(requestId) << '\n'
           << "SessionId " << std::quoted(sessionId) << '\n'
           << "ProjectPath " << std::quoted(canonicalProjectPath) << '\n'
           << "Action InspectMaterialSurface\n"
           << "EntityId " << entityId << '\n'
           << "ExpectedEntityName " << std::quoted(expectedEntityName) << '\n'
           << "MaterialHandle " << materialHandle << '\n';
    return stream.str();
}

std::string EditorMaterialControlMailbox::FormatInspectEntityRequest(
    std::string_view requestId, std::string_view sessionId,
    std::string_view projectPath, Engine::EntityId entityId,
    std::string_view expectedEntityName)
{
    const std::string canonicalProjectPath = CanonicalProjectPath(projectPath);
    std::ostringstream stream;
    stream << kRequestHeader << '\n'
           << "RequestId " << std::quoted(requestId) << '\n'
           << "SessionId " << std::quoted(sessionId) << '\n'
           << "ProjectPath " << std::quoted(canonicalProjectPath) << '\n'
           << "Action InspectEntity\n"
           << "EntityId " << entityId << '\n'
           << "ExpectedEntityName " << std::quoted(expectedEntityName) << '\n';
    return stream.str();
}

std::string EditorMaterialControlMailbox::FormatPatchRequest(std::string_view requestId,
    std::string_view sessionId, std::string_view projectPath, Engine::EntityId entityId,
    std::string_view expectedEntityName, Engine::AssetHandle materialHandle,
    const Engine::MaterialSurface& expectedSurface,
    const Engine::MaterialSurface& newSurface)
{
    const std::string canonicalProjectPath = CanonicalProjectPath(projectPath);
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
           << "ProjectPath " << std::quoted(canonicalProjectPath) << '\n'
           << "Action SelectEntityPatchMaterialSurface\n"
           << "EntityId " << entityId << '\n'
           << "ExpectedEntityName " << std::quoted(expectedEntityName) << '\n'
           << "MaterialHandle " << materialHandle << '\n';
    writeSurface(stream, "ExpectedSurface", expectedSurface);
    writeSurface(stream, "NewSurface", newSurface);
    stream << "Scope SharedMaterial\n";
    return stream.str();
}
