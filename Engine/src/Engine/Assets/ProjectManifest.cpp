#include "Engine/Assets/ProjectManifest.h"

#include "Engine/Assets/AssetRegistry.h"
#include "Engine/Core/AtomicFile.h"

#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <vector>

namespace Engine
{
    namespace
    {
        bool ParseFramePacingMode(std::string_view text, FramePacingMode& outMode)
        {
            if (text == "Responsive")
            {
                outMode = FramePacingMode::Responsive;
                return true;
            }
            if (text == "SmoothFrametime")
            {
                outMode = FramePacingMode::SmoothFrametime;
                return true;
            }
            return false;
        }

        // The manifest names modes with identifiers that predate the display
        // strings of ToString(FramePacingMode); keep the persisted spelling here.
        const char* ToManifestFramePacingMode(FramePacingMode mode)
        {
            return mode == FramePacingMode::SmoothFrametime ? "SmoothFrametime" : "Responsive";
        }

        bool ParsePresentationPolicy(std::string_view text, PresentationPolicy& outPolicy)
        {
            if (text == "Synchronized")
            {
                outPolicy = PresentationPolicy::Synchronized;
                return true;
            }
            if (text == "TearingAllowed")
            {
                outPolicy = PresentationPolicy::TearingAllowed;
                return true;
            }
            return false;
        }

        // Scene and AssetRegistry may be absolute in manifests written before
        // format 7 (new projects use a user-chosen root), so only unambiguous
        // hazards are rejected: control bytes and parent-directory segments under
        // either separator.
        bool IsAcceptableLegacyManifestPath(std::string_view path)
        {
            if (path.empty() || path.size() > kMaximumProjectManifestPathBytes)
                return false;
            size_t segmentStart = 0;
            for (size_t index = 0; index <= path.size(); ++index)
            {
                if (index < path.size())
                {
                    const unsigned char character = static_cast<unsigned char>(path[index]);
                    if (character < 0x20 || character == 0x7f)
                        return false;
                }
                if (index == path.size() || path[index] == '/' || path[index] == '\\')
                {
                    if (path.substr(segmentStart, index - segmentStart) == "..")
                        return false;
                    segmentStart = index + 1;
                }
            }
            return true;
        }

        bool ValidateManifestValues(const ProjectManifest& manifest, std::string& outError)
        {
            if (!IsAcceptableLegacyManifestPath(manifest.ScenePath))
                outError = "the Scene path is empty, oversized, contains control bytes, or escapes with '..'";
            else if (!IsAcceptableLegacyManifestPath(manifest.AssetRegistryPath))
                outError = "the AssetRegistry path is empty, oversized, contains control bytes, or escapes with '..'";
            else if (!manifest.FabReceiptsPath.empty()
                && !IsPortableProjectRelativePath(manifest.FabReceiptsPath))
                outError = "the FabReceipts path must be a strict portable project-relative path";
            else if (manifest.ProjectRevision == (std::numeric_limits<u64>::max)())
                outError = "the ProjectRevision is out of range";
            else if (!IsValidFramePacingPolicy(manifest.FramePacingPolicy))
                outError = "the frame pacing policy is invalid";
            else if (!IsValidRendererColorPipelineSettings(manifest.ColorPipelineSettings))
                outError = "the color pipeline settings are invalid";
            else
                return true;
            return false;
        }

        // Canonical unsigned decimal: digits only, no sign, no leading zero, no overflow.
        bool ParseCanonicalUnsigned(std::string_view text, u64& outValue)
        {
            if (text.empty() || text.size() > 20 || (text.size() > 1 && text.front() == '0'))
                return false;
            u64 value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc() || end != text.data() + text.size())
                return false;
            outValue = value;
            return true;
        }
    }

    bool IsPortableProjectRelativePath(std::string_view path)
    {
        return !path.empty() && AssetRegistry::IsValidCookedRoot(path);
    }

    bool SerializeProjectManifest(
        const ProjectManifest& manifest, std::string& outBytes, std::string& outError)
    {
        if (!ValidateManifestValues(manifest, outError))
            return false;

        std::ostringstream output;
        output.imbue(std::locale::classic());
        output << std::setprecision(std::numeric_limits<double>::max_digits10);
        output << "SpiralProject " << kProjectManifestFormatVersion << '\n';
        output << "Scene " << std::quoted(manifest.ScenePath) << '\n';
        output << "AssetRegistry " << std::quoted(manifest.AssetRegistryPath) << '\n';
        if (!manifest.FabReceiptsPath.empty())
            output << "FabReceipts " << std::quoted(manifest.FabReceiptsPath) << '\n';
        output << "ProjectRevision " << manifest.ProjectRevision << '\n';
        output << "FramePacingMode " << ToManifestFramePacingMode(manifest.FramePacingPolicy.Mode) << '\n';
        output << "FramePacingTargetFps " << manifest.FramePacingPolicy.SmoothTargetFramesPerSecond << '\n';
        output << "PresentationPolicy " << ToString(manifest.PresentationPolicy) << '\n';
        output << "ManualExposureEV100 " << manifest.ColorPipelineSettings.ManualExposureEV100 << '\n';
        output << "PostToneMapSaturation " << manifest.ColorPipelineSettings.PostToneMapSaturation << '\n';
        output << "PostToneMapContrast " << manifest.ColorPipelineSettings.PostToneMapContrast << '\n';
        output << "ExposureMode " << ToString(manifest.ColorPipelineSettings.ExposureMode) << '\n';
        output << "CameraApertureFNumber " << manifest.ColorPipelineSettings.CameraApertureFNumber << '\n';
        output << "CameraShutterSeconds " << manifest.ColorPipelineSettings.CameraShutterSeconds << '\n';
        output << "CameraISO " << manifest.ColorPipelineSettings.CameraISO << '\n';
        if (!output || output.str().size() > kMaximumProjectManifestBytes)
        {
            outError = "the serialized project manifest is invalid or oversized";
            return false;
        }

        outBytes = output.str();
        outError.clear();
        return true;
    }

    bool DeserializeProjectManifest(
        std::string_view bytes, ProjectManifest& outManifest, std::string& outError)
    {
        if (bytes.size() > kMaximumProjectManifestBytes)
        {
            outError = "the project manifest exceeds the size limit";
            return false;
        }
        for (const char byte : bytes)
        {
            const unsigned char character = static_cast<unsigned char>(byte);
            if (character == 0 || (character < 0x20 && character != '\n' && character != '\r'
                && character != '\t'))
            {
                outError = "the project manifest contains a NUL or control byte";
                return false;
            }
        }

        std::istringstream input { std::string(bytes) };
        input.imbue(std::locale::classic());

        std::string magic;
        std::string versionText;
        u64 parsedVersion = 0;
        if (!(input >> magic >> versionText) || magic != "SpiralProject"
            || !ParseCanonicalUnsigned(versionText, parsedVersion) || parsedVersion < 1
            || parsedVersion > kProjectManifestFormatVersion)
        {
            outError = "the project manifest header is missing, malformed, or from an unsupported version";
            return false;
        }
        const u32 version = static_cast<u32>(parsedVersion);
        // The current writer always ends the file with a newline, so a file cut
        // inside its last value (a valid shorter number) is detectably truncated.
        if (version >= 7 && bytes.back() != '\n')
        {
            outError = "the project manifest is truncated: it does not end with a newline";
            return false;
        }

        ProjectManifest manifest;
        std::vector<std::string> seenKeys;
        const auto fail = [&outError](std::string message)
        {
            outError = std::move(message);
            return false;
        };

        bool readFramePacingMode = version == 1;
        bool readFramePacingTarget = version == 1;
        bool readPresentationPolicy = version < 3;
        bool readManualExposure = version < 4;
        bool readPostToneMapSaturation = version < 5;
        bool readPostToneMapContrast = version < 5;
        bool readExposureMode = version < 6;
        bool readCameraAperture = version < 6;
        bool readCameraShutter = version < 6;
        bool readCameraISO = version < 6;
        bool readProjectRevision = version < 7;
        const auto readQuoted = [&input](std::string& outText)
        {
            input >> std::ws;
            if (input.peek() != '"')
                return false;
            return static_cast<bool>(input >> std::quoted(outText));
        };

        std::string key;
        while (input >> key)
        {
            for (const std::string& seen : seenKeys)
            {
                if (seen == key)
                    return fail("the project manifest repeats the key " + key);
            }
            seenKeys.push_back(key);

            if (key == "Scene")
            {
                if (!readQuoted(manifest.ScenePath))
                    return fail("the Scene value is not a quoted string");
            }
            else if (key == "AssetRegistry")
            {
                if (!readQuoted(manifest.AssetRegistryPath))
                    return fail("the AssetRegistry value is not a quoted string");
            }
            else if (version >= 7 && key == "FabReceipts")
            {
                if (!readQuoted(manifest.FabReceiptsPath) || manifest.FabReceiptsPath.empty())
                    return fail("the FabReceipts value is not a nonempty quoted string");
            }
            else if (version >= 7 && key == "ProjectRevision")
            {
                std::string text;
                if (!(input >> text) || !ParseCanonicalUnsigned(text, manifest.ProjectRevision))
                    return fail("the ProjectRevision is not a canonical unsigned integer");
                readProjectRevision = true;
            }
            else if (version >= 2 && key == "FramePacingMode")
            {
                std::string mode;
                if (!(input >> mode) || !ParseFramePacingMode(mode, manifest.FramePacingPolicy.Mode))
                    return fail("the FramePacingMode is unknown");
                readFramePacingMode = true;
            }
            else if (version >= 2 && key == "FramePacingTargetFps")
            {
                if (!(input >> manifest.FramePacingPolicy.SmoothTargetFramesPerSecond))
                    return fail("the FramePacingTargetFps is not a finite number");
                readFramePacingTarget = true;
            }
            else if (version >= 3 && key == "PresentationPolicy")
            {
                std::string policy;
                if (!(input >> policy) || !ParsePresentationPolicy(policy, manifest.PresentationPolicy))
                    return fail("the PresentationPolicy is unknown");
                readPresentationPolicy = true;
            }
            else if (version >= 4 && key == "ManualExposureEV100")
            {
                if (!(input >> manifest.ColorPipelineSettings.ManualExposureEV100))
                    return fail("the ManualExposureEV100 is not a finite number");
                readManualExposure = true;
            }
            else if (version >= 5 && key == "PostToneMapSaturation")
            {
                if (!(input >> manifest.ColorPipelineSettings.PostToneMapSaturation))
                    return fail("the PostToneMapSaturation is not a finite number");
                readPostToneMapSaturation = true;
            }
            else if (version >= 5 && key == "PostToneMapContrast")
            {
                if (!(input >> manifest.ColorPipelineSettings.PostToneMapContrast))
                    return fail("the PostToneMapContrast is not a finite number");
                readPostToneMapContrast = true;
            }
            else if (version >= 6 && key == "ExposureMode")
            {
                std::string mode;
                if (!(input >> mode)
                    || !ParseRendererExposureMode(mode, manifest.ColorPipelineSettings.ExposureMode))
                    return fail("the ExposureMode is unknown");
                readExposureMode = true;
            }
            else if (version >= 6 && key == "CameraApertureFNumber")
            {
                if (!(input >> manifest.ColorPipelineSettings.CameraApertureFNumber))
                    return fail("the CameraApertureFNumber is not a finite number");
                readCameraAperture = true;
            }
            else if (version >= 6 && key == "CameraShutterSeconds")
            {
                if (!(input >> manifest.ColorPipelineSettings.CameraShutterSeconds))
                    return fail("the CameraShutterSeconds is not a finite number");
                readCameraShutter = true;
            }
            else if (version >= 6 && key == "CameraISO")
            {
                if (!(input >> manifest.ColorPipelineSettings.CameraISO))
                    return fail("the CameraISO is not a finite number");
                readCameraISO = true;
            }
            else
                return fail("the project manifest has an unknown key for this version: " + key);

            if (!input)
                return fail("the project manifest has a truncated or malformed value for " + key);
        }

        const bool complete = readFramePacingMode && readFramePacingTarget && readPresentationPolicy
            && readManualExposure && readPostToneMapSaturation && readPostToneMapContrast
            && readExposureMode && readCameraAperture && readCameraShutter && readCameraISO
            && readProjectRevision && !manifest.ScenePath.empty() && !manifest.AssetRegistryPath.empty();
        if (!complete)
            return fail("the project manifest is missing a required key");
        if (!ValidateManifestValues(manifest, outError))
            return false;

        outManifest = std::move(manifest);
        outError.clear();
        return true;
    }

    bool StoreProjectManifest(const std::filesystem::path& path, const ProjectManifest& manifest,
        std::string& outError, bool* outDirectoryDurable)
    {
        if (outDirectoryDurable)
            *outDirectoryDurable = false;
        std::string bytes;
        return SerializeProjectManifest(manifest, bytes, outError)
            && WriteFileAtomically(path, bytes, outError, outDirectoryDurable);
    }

    bool ReadProjectManifestBytes(
        const std::filesystem::path& path, std::string& outBytes, std::string& outError)
    {
        std::ifstream input(path, std::ios::in | std::ios::binary);
        if (!input)
        {
            outError = "could not open the project manifest";
            return false;
        }

        // Read one byte past the limit so an oversized file is rejected without
        // buffering it.
        std::string bytes;
        char chunk[4096];
        while (input.read(chunk, sizeof(chunk)) || input.gcount() > 0)
        {
            bytes.append(chunk, static_cast<size_t>(input.gcount()));
            if (bytes.size() > kMaximumProjectManifestBytes)
            {
                outError = "the project manifest exceeds the size limit";
                return false;
            }
        }
        if (input.bad())
        {
            outError = "could not read the project manifest";
            return false;
        }

        outBytes = std::move(bytes);
        outError.clear();
        return true;
    }

    bool LoadProjectManifest(
        const std::filesystem::path& path, ProjectManifest& outManifest, std::string& outError)
    {
        std::string bytes;
        return ReadProjectManifestBytes(path, bytes, outError)
            && DeserializeProjectManifest(bytes, outManifest, outError);
    }
}
