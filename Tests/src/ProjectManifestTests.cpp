#include "ProjectManifestTests.h"

#include "Engine/Assets/MaterialAsset.h"
#include "Engine/Assets/ProjectManifest.h"
#include "Engine/Core/AtomicFile.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__linux__)
    #include <csignal>
    #include <sys/resource.h>
    #include <unistd.h>
#endif

namespace
{
    using namespace Engine;

    class TempRoot
    {
    public:
        TempRoot()
        {
            static std::atomic<u64> sequence { 0 };
            std::error_code error;
            const u64 tick = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
            m_Path = std::filesystem::temp_directory_path(error)
                / ("spiral-project-manifest-test-" + std::to_string(tick) + "-"
                    + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
            m_Ready = !error && std::filesystem::create_directory(m_Path, error) && !error;
        }

        ~TempRoot()
        {
            if (!m_Ready)
                return;
            std::error_code error;
            for (const std::filesystem::directory_entry& entry :
                std::filesystem::recursive_directory_iterator(m_Path, std::filesystem::directory_options::skip_permission_denied, error))
                if (entry.is_directory(error))
                    std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_all,
                        std::filesystem::perm_options::add, error);
            std::filesystem::remove_all(m_Path, error);
        }

        TempRoot(const TempRoot&) = delete;
        TempRoot& operator=(const TempRoot&) = delete;

        bool IsReady() const { return m_Ready; }
        const std::filesystem::path& Path() const { return m_Path; }

    private:
        std::filesystem::path m_Path;
        bool m_Ready = false;
    };

    std::string ReadAll(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    bool WriteAll(const std::filesystem::path& path, std::string_view bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(output);
    }

    bool ReplaceOnce(std::string& text, std::string_view from, std::string_view to)
    {
        const size_t offset = text.find(from);
        if (offset == std::string::npos)
            return false;
        text.replace(offset, from.size(), to);
        return true;
    }

    // Names every entry of a directory so "nothing was left behind" is checkable.
    std::vector<std::string> ListNames(const std::filesystem::path& directory)
    {
        std::vector<std::string> names;
        std::error_code error;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, error))
            names.push_back(entry.path().filename().string());
        std::sort(names.begin(), names.end());
        return names;
    }

    // Hand-written independent oracle for the current format. Not produced by the code under test.
    constexpr std::string_view kGoldenV7 =
        "SpiralProject 7\n"
        "Scene \"Scenes/Main.spiral\"\n"
        "AssetRegistry \"Assets/assets.spiralassets\"\n"
        "FabReceipts \"Assets/Fab/receipts.spiralfab\"\n"
        "ProjectRevision 3\n"
        "FramePacingMode SmoothFrametime\n"
        "FramePacingTargetFps 144\n"
        "PresentationPolicy TearingAllowed\n"
        "ManualExposureEV100 -1.5\n"
        "PostToneMapSaturation 0.25\n"
        "PostToneMapContrast 1.5\n"
        "ExposureMode CameraCalibration\n"
        "CameraApertureFNumber 2\n"
        "CameraShutterSeconds 0.25\n"
        "CameraISO 200\n";

    ProjectManifest GoldenManifest()
    {
        ProjectManifest manifest;
        manifest.ScenePath = "Scenes/Main.spiral";
        manifest.AssetRegistryPath = "Assets/assets.spiralassets";
        manifest.FabReceiptsPath = "Assets/Fab/receipts.spiralfab";
        manifest.ProjectRevision = 3;
        manifest.FramePacingPolicy = { FramePacingMode::SmoothFrametime, 144.0 };
        manifest.PresentationPolicy = PresentationPolicy::TearingAllowed;
        manifest.ColorPipelineSettings.ManualExposureEV100 = -1.5;
        manifest.ColorPipelineSettings.PostToneMapSaturation = 0.25;
        manifest.ColorPipelineSettings.PostToneMapContrast = 1.5;
        manifest.ColorPipelineSettings.ExposureMode = RendererExposureMode::CameraCalibration;
        manifest.ColorPipelineSettings.CameraApertureFNumber = 2.0;
        manifest.ColorPipelineSettings.CameraShutterSeconds = 0.25;
        manifest.ColorPipelineSettings.CameraISO = 200.0;
        return manifest;
    }

    ProjectManifest Sentinel()
    {
        ProjectManifest manifest;
        manifest.ScenePath = "sentinel.spiral";
        manifest.AssetRegistryPath = "sentinel.spiralassets";
        manifest.FabReceiptsPath = "sentinel/receipts.spiralfab";
        manifest.ProjectRevision = 41;
        manifest.FramePacingPolicy = { FramePacingMode::SmoothFrametime, 123.0 };
        manifest.PresentationPolicy = PresentationPolicy::TearingAllowed;
        manifest.ColorPipelineSettings = { 1.0, 0.75, 1.25 };
        return manifest;
    }

#if defined(__linux__)
    // Real mid-write failure: the kernel refuses to grow a file past the soft
    // limit (EFBIG), after a partial write. SIGXFSZ is ignored for the duration.
    class ScopedFileSizeLimit
    {
    public:
        explicit ScopedFileSizeLimit(rlim_t bytes)
        {
            m_Ready = getrlimit(RLIMIT_FSIZE, &m_Previous) == 0;
            if (!m_Ready)
                return;
            m_PreviousHandler = std::signal(SIGXFSZ, SIG_IGN);
            rlimit limit = m_Previous;
            limit.rlim_cur = bytes;
            m_Applied = setrlimit(RLIMIT_FSIZE, &limit) == 0;
        }

        ~ScopedFileSizeLimit()
        {
            if (m_Ready)
            {
                setrlimit(RLIMIT_FSIZE, &m_Previous);
                std::signal(SIGXFSZ, m_PreviousHandler);
            }
        }

        ScopedFileSizeLimit(const ScopedFileSizeLimit&) = delete;
        ScopedFileSizeLimit& operator=(const ScopedFileSizeLimit&) = delete;

        bool IsApplied() const { return m_Applied; }

    private:
        rlimit m_Previous {};
        void (*m_PreviousHandler)(int) = SIG_DFL;
        bool m_Ready = false;
        bool m_Applied = false;
    };
#endif
}

namespace SpiralTests
{
    bool TestProjectManifestCodec()
    {
        bool passed = true;
        const auto Check = [&passed](bool condition, std::string_view message)
        {
            if (!condition)
            {
                std::cerr << "Project manifest codec test failed: " << message << '\n';
                passed = false;
            }
        };

        // ----- format 7 golden, both directions -----
        std::string bytes;
        std::string error;
        const ProjectManifest golden = GoldenManifest();
        Check(SerializeProjectManifest(golden, bytes, error) && bytes == kGoldenV7,
            "format 7 serializes to the hand-written golden bytes");
        ProjectManifest parsed;
        Check(DeserializeProjectManifest(kGoldenV7, parsed, error) && parsed == golden,
            "format 7 golden parses to the expected fields");
        std::string reordered(kGoldenV7);
        ReplaceOnce(reordered, "ProjectRevision 3\n", "");
        reordered += "ProjectRevision 3\n";
        ProjectManifest reorderedParsed;
        Check(DeserializeProjectManifest(reordered, reorderedParsed, error) && reorderedParsed == golden,
            "key order is not significant");

        // ----- deterministic migration of every earlier format -----
        struct Migration
        {
            std::string_view Name;
            std::string_view Text;
            std::string_view ExpectedV7;
        };
        const Migration migrations[] = {
            { "v1",
                "SpiralProject 1\nScene \"legacy.spiral\"\nAssetRegistry \"legacy.spiralassets\"\n",
                "SpiralProject 7\nScene \"legacy.spiral\"\nAssetRegistry \"legacy.spiralassets\"\nProjectRevision 0\n"
                "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
                "ManualExposureEV100 0\nPostToneMapSaturation 1\nPostToneMapContrast 1\n"
                "ExposureMode ManualEV100\nCameraApertureFNumber 1\nCameraShutterSeconds 1\nCameraISO 100\n" },
            { "v3",
                "SpiralProject 3\nScene \"v3.spiral\"\nAssetRegistry \"v3.spiralassets\"\n"
                "FramePacingMode SmoothFrametime\nFramePacingTargetFps 90\nPresentationPolicy TearingAllowed\n",
                "SpiralProject 7\nScene \"v3.spiral\"\nAssetRegistry \"v3.spiralassets\"\nProjectRevision 0\n"
                "FramePacingMode SmoothFrametime\nFramePacingTargetFps 90\nPresentationPolicy TearingAllowed\n"
                "ManualExposureEV100 0\nPostToneMapSaturation 1\nPostToneMapContrast 1\n"
                "ExposureMode ManualEV100\nCameraApertureFNumber 1\nCameraShutterSeconds 1\nCameraISO 100\n" },
            { "v5",
                "SpiralProject 5\nScene \"v5.spiral\"\nAssetRegistry \"v5.spiralassets\"\n"
                "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
                "ManualExposureEV100 -1\nPostToneMapSaturation 0.25\nPostToneMapContrast 1.5\n",
                "SpiralProject 7\nScene \"v5.spiral\"\nAssetRegistry \"v5.spiralassets\"\nProjectRevision 0\n"
                "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
                "ManualExposureEV100 -1\nPostToneMapSaturation 0.25\nPostToneMapContrast 1.5\n"
                "ExposureMode ManualEV100\nCameraApertureFNumber 1\nCameraShutterSeconds 1\nCameraISO 100\n" },
            // The checked-in default project as the previous writer produced it.
            { "v6 default project",
                "SpiralProject 6\nScene \"output/scenes/sample.spiral\"\nAssetRegistry \"output/assets/sample.assets\"\n"
                "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
                "ManualExposureEV100 0\nPostToneMapSaturation 1\nPostToneMapContrast 1\n"
                "ExposureMode ManualEV100\nCameraApertureFNumber 1\nCameraShutterSeconds 1\nCameraISO 100\n",
                "SpiralProject 7\nScene \"output/scenes/sample.spiral\"\nAssetRegistry \"output/assets/sample.assets\"\n"
                "ProjectRevision 0\n"
                "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
                "ManualExposureEV100 0\nPostToneMapSaturation 1\nPostToneMapContrast 1\n"
                "ExposureMode ManualEV100\nCameraApertureFNumber 1\nCameraShutterSeconds 1\nCameraISO 100\n" },
            { "v6 camera calibration",
                "SpiralProject 6\nScene \"v6.spiral\"\nAssetRegistry \"v6.spiralassets\"\n"
                "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
                "ManualExposureEV100 -1\nPostToneMapSaturation 0.25\nPostToneMapContrast 1.5\n"
                "ExposureMode CameraCalibration\nCameraApertureFNumber 2\nCameraShutterSeconds 0.25\nCameraISO 200\n",
                "SpiralProject 7\nScene \"v6.spiral\"\nAssetRegistry \"v6.spiralassets\"\nProjectRevision 0\n"
                "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
                "ManualExposureEV100 -1\nPostToneMapSaturation 0.25\nPostToneMapContrast 1.5\n"
                "ExposureMode CameraCalibration\nCameraApertureFNumber 2\nCameraShutterSeconds 0.25\nCameraISO 200\n" },
        };
        for (const Migration& migration : migrations)
        {
            ProjectManifest migrated;
            std::string migratedBytes;
            const bool parsedOk = DeserializeProjectManifest(migration.Text, migrated, error);
            Check(parsedOk && migrated.FabReceiptsPath.empty() && migrated.ProjectRevision == 0,
                std::string(migration.Name) + " migrates to no receipts and revision 0");
            Check(parsedOk && SerializeProjectManifest(migrated, migratedBytes, error)
                    && migratedBytes == migration.ExpectedV7,
                std::string(migration.Name) + " serializes to the hand-written format 7 golden");
            ProjectManifest again;
            std::string againBytes;
            Check(parsedOk && DeserializeProjectManifest(migratedBytes, again, error) && again == migrated
                    && SerializeProjectManifest(again, againBytes, error) && againBytes == migratedBytes,
                std::string(migration.Name) + " migration is idempotent");
        }

        // Legacy absolute Scene/AssetRegistry paths (projects created in a user-chosen root) stay readable.
        ProjectManifest legacyAbsolute;
        std::string legacyText(migrations[3].Text);
        ReplaceOnce(legacyText, "output/scenes/sample.spiral", "/home/user/My Project/Scenes/Main.spiral");
        ReplaceOnce(legacyText, "output/assets/sample.assets", "C:\\\\Users\\\\user\\\\Assets\\\\assets.spiralassets");
        Check(DeserializeProjectManifest(legacyText, legacyAbsolute, error)
                && legacyAbsolute.ScenePath == "/home/user/My Project/Scenes/Main.spiral"
                && legacyAbsolute.AssetRegistryPath == "C:\\Users\\user\\Assets\\assets.spiralassets",
            "absolute and backslash legacy Scene and AssetRegistry paths remain readable");

        // ----- rejection is transactional -----
        const ProjectManifest sentinel = Sentinel();
        const auto Rejects = [&](std::string_view name, std::string_view text)
        {
            ProjectManifest target = sentinel;
            std::string rejectError;
            const bool rejected = !DeserializeProjectManifest(text, target, rejectError);
            Check(rejected && !rejectError.empty() && target == sentinel,
                std::string("rejects transactionally: ") + std::string(name));
        };
        const std::string valid(kGoldenV7);
        const auto Mutated = [&valid](std::string_view from, std::string_view to)
        {
            std::string text = valid;
            ReplaceOnce(text, from, to);
            return text;
        };

        Rejects("empty input", "");
        Rejects("magic only", "SpiralProject");
        Rejects("future version", Mutated("SpiralProject 7", "SpiralProject 8"));
        Rejects("version zero", Mutated("SpiralProject 7", "SpiralProject 0"));
        Rejects("leading-zero version", Mutated("SpiralProject 7", "SpiralProject 07"));
        Rejects("version with suffix", Mutated("SpiralProject 7", "SpiralProject 7x"));
        Rejects("wrong magic", Mutated("SpiralProject", "SpiralProjecT"));

        // Every strict prefix of a format 7 file is rejected, including a cut inside the
        // last number, which would otherwise be a valid shorter number.
        size_t acceptedPrefixes = 0;
        for (size_t length = 0; length < valid.size(); ++length)
        {
            ProjectManifest target = sentinel;
            std::string prefixError;
            if (DeserializeProjectManifest(std::string_view(valid).substr(0, length), target, prefixError)
                || !(target == sentinel))
                ++acceptedPrefixes;
        }
        Check(acceptedPrefixes == 0, "every truncated prefix of a format 7 manifest is rejected");

        Rejects("duplicate Scene", valid + "Scene \"other.spiral\"\n");
        Rejects("duplicate ProjectRevision", valid + "ProjectRevision 4\n");
        Rejects("duplicate FabReceipts", valid + "FabReceipts \"Assets/Fab/other.spiralfab\"\n");
        Rejects("duplicate CameraISO", valid + "CameraISO 200\n");
        Rejects("unknown key", valid + "Bogus 1\n");
        Rejects("unknown key prefix collision", valid + "SceneX \"a\"\n");
        Rejects("format 6 with ProjectRevision",
            std::string(migrations[3].Text) + "ProjectRevision 1\n");
        Rejects("format 6 with FabReceipts",
            std::string(migrations[3].Text) + "FabReceipts \"Assets/Fab/r.spiralfab\"\n");
        Rejects("format 7 without ProjectRevision", Mutated("ProjectRevision 3\n", ""));
        Rejects("format 7 without FramePacingMode", Mutated("FramePacingMode SmoothFrametime\n", ""));
        Rejects("format 7 without AssetRegistry", Mutated("AssetRegistry \"Assets/assets.spiralassets\"\n", ""));
        Rejects("format 7 without trailing newline", valid.substr(0, valid.size() - 1));

        for (const char* revision : { "01", "-1", "1.5", "0x10", "18446744073709551615", "18446744073709551616", "abc" })
            Rejects(std::string("ProjectRevision ") + revision, Mutated("ProjectRevision 3", std::string("ProjectRevision ") + revision));
        Rejects("ProjectRevision without value", Mutated("ProjectRevision 3\n", "ProjectRevision\n"));

        Rejects("exposure above range", Mutated("ManualExposureEV100 -1.5", "ManualExposureEV100 17"));
        Rejects("exposure overflow", Mutated("ManualExposureEV100 -1.5", "ManualExposureEV100 1e9999"));
        Rejects("exposure not a number", Mutated("ManualExposureEV100 -1.5", "ManualExposureEV100 abc"));
        Rejects("exposure nan", Mutated("ManualExposureEV100 -1.5", "ManualExposureEV100 nan"));
        Rejects("smooth fps zero", Mutated("FramePacingTargetFps 144", "FramePacingTargetFps 0"));
        Rejects("smooth fps above range", Mutated("FramePacingTargetFps 144", "FramePacingTargetFps 1001"));
        Rejects("saturation above range", Mutated("PostToneMapSaturation 0.25", "PostToneMapSaturation 2.25"));
        Rejects("iso zero", Mutated("CameraISO 200", "CameraISO 0"));
        Rejects("unknown presentation policy", Mutated("TearingAllowed", "NotAPolicy"));
        Rejects("unknown exposure mode", Mutated("CameraCalibration", "AperturePriority"));
        Rejects("unknown frame pacing mode", Mutated("SmoothFrametime", "Turbo"));

        Rejects("unquoted Scene", Mutated("Scene \"Scenes/Main.spiral\"", "Scene Scenes/Main.spiral"));
        Rejects("unterminated Scene quote", Mutated("Scene \"Scenes/Main.spiral\"", "Scene \"Scenes/Main.spiral"));
        Rejects("empty Scene", Mutated("Scene \"Scenes/Main.spiral\"", "Scene \"\""));
        Rejects("empty FabReceipts", Mutated("FabReceipts \"Assets/Fab/receipts.spiralfab\"", "FabReceipts \"\""));
        Rejects("embedded NUL", Mutated("Scene \"Scenes/Main.spiral\"", std::string("Scene \"Scenes/Ma\0in.spiral\"", 27)));
        Rejects("embedded control byte", Mutated("Scene \"Scenes/Main.spiral\"", "Scene \"Scenes/Ma\x01in.spiral\""));

        // ----- path escape attempts inside the manifest -----
        for (const char* escape : { "../outside.spiral", "a/../../outside.spiral", "..", "a\\\\..\\\\outside.spiral", "a/.." })
            Rejects(std::string("Scene escape ") + escape,
                Mutated("Scenes/Main.spiral", escape));
        for (const char* escape : { "../outside", "a/../b", "/abs/receipts", "a//b", "a\\\\b", "C:/x", "a/",
                 "./a", "a/./b", "con", "a:b", "a/b.", " a" })
            Rejects(std::string("FabReceipts escape ") + escape,
                Mutated("Assets/Fab/receipts.spiralfab", escape));
        Rejects("FabReceipts too long",
            Mutated("Assets/Fab/receipts.spiralfab", std::string(1100, 'a')));
        Rejects("Scene too long", Mutated("Scenes/Main.spiral", std::string(5000, 'a')));

        // ----- serialization refuses what parsing refuses, without touching the output -----
        {
            ProjectManifest bad = GoldenManifest();
            bad.FabReceiptsPath = "../escape";
            std::string out = "sentinel-bytes";
            Check(!SerializeProjectManifest(bad, out, error) && out == "sentinel-bytes",
                "serialization rejects a receipts path escape without changing the output");
            bad = GoldenManifest();
            bad.ScenePath.clear();
            Check(!SerializeProjectManifest(bad, out, error) && out == "sentinel-bytes",
                "serialization rejects an empty Scene path");
            bad = GoldenManifest();
            bad.ProjectRevision = ~0ull;
            Check(!SerializeProjectManifest(bad, out, error) && out == "sentinel-bytes",
                "serialization rejects the maximum revision (no successor)");
            bad = GoldenManifest();
            bad.ColorPipelineSettings.CameraISO = 0.0;
            Check(!SerializeProjectManifest(bad, out, error) && out == "sentinel-bytes",
                "serialization rejects invalid color settings");
        }

        // ----- exact size boundary -----
        {
            std::string atLimit = valid;
            atLimit.append(static_cast<size_t>(kMaximumProjectManifestBytes) - atLimit.size(), '\n');
            ProjectManifest atLimitParsed;
            Check(DeserializeProjectManifest(atLimit, atLimitParsed, error) && atLimitParsed == golden,
                "a manifest of exactly the size limit is accepted");
            Rejects("one byte over the size limit", atLimit + " ");
        }

        // ----- file Load/Store, bounded read, transactional failure -----
        const TempRoot root;
        Check(root.IsReady(), "temporary fixture root exists");
        if (!root.IsReady())
            return false;
        const std::filesystem::path file = root.Path() / "round.spiralproject";
        bool durable = false;
        Check(StoreProjectManifest(file, golden, error, &durable) && ReadAll(file) == kGoldenV7,
            "Store writes the golden bytes");
        ProjectManifest loaded;
        Check(LoadProjectManifest(file, loaded, error) && loaded == golden, "Load reads what Store wrote");
        Check(ListNames(root.Path()) == std::vector<std::string> { "round.spiralproject" },
            "Store leaves no temporary file behind");

        std::string oversized = valid;
        oversized.append(static_cast<size_t>(kMaximumProjectManifestBytes) - oversized.size() + 1, '\n');
        const std::filesystem::path bigFile = root.Path() / "big.spiralproject";
        ProjectManifest bigTarget = sentinel;
        Check(WriteAll(bigFile, oversized) && !LoadProjectManifest(bigFile, bigTarget, error) && bigTarget == sentinel,
            "an oversized manifest file is rejected without parsing");
        std::string bounded;
        Check(!ReadProjectManifestBytes(bigFile, bounded, error) && bounded.empty(),
            "the bounded byte reader rejects an oversized file");
        ProjectManifest missingTarget = sentinel;
        Check(!LoadProjectManifest(root.Path() / "absent.spiralproject", missingTarget, error)
                && missingTarget == sentinel,
            "a missing manifest is rejected transactionally");

        const std::filesystem::path invalidStore = root.Path() / "invalid.spiralproject";
        ProjectManifest invalidManifest = golden;
        invalidManifest.FabReceiptsPath = "/absolute";
        Check(!StoreProjectManifest(invalidStore, invalidManifest, error) && !std::filesystem::exists(invalidStore),
            "Store of an invalid manifest creates no file");
        return passed;
    }

    bool TestProjectPersistenceWritersAreCrashSafe()
    {
        bool passed = true;
        const auto Check = [&passed](bool condition, std::string_view message)
        {
            if (!condition)
            {
                std::cerr << "Project persistence test failed: " << message << '\n';
                passed = false;
            }
        };

        const TempRoot root;
        Check(root.IsReady(), "temporary fixture root exists");
        if (!root.IsReady())
            return false;

        std::string error;
        const ProjectManifest manifestA = GoldenManifest();
        ProjectManifest manifestB = GoldenManifest();
        manifestB.ProjectRevision = 4;
        manifestB.ScenePath = "Scenes/Other.spiral";

        MaterialAsset materialA;
        materialA.Name = "A";
        materialA.BaseColor = { 0.25f, 0.5f, 0.75f };
        MaterialAsset materialB = materialA;
        materialB.Name = "B";
        materialB.Metallic = 0.5f;

        Scene sceneA("Scene A");
        sceneA.CreateEntity("EntityA");
        Scene sceneB("Scene B");
        sceneB.CreateEntity("EntityB");
        sceneB.CreateEntity("EntityC");

        // ----- target the rename cannot replace: a non-empty directory -----
        {
            const std::filesystem::path blocked = root.Path() / "blocked";
            std::filesystem::create_directories(blocked / "keep");
            const std::filesystem::path manifestTarget = blocked / "m.spiralproject";
            const std::filesystem::path sceneTarget = blocked / "s.spiral";
            const std::filesystem::path materialTarget = blocked / "m.spiralmat";
            for (const std::filesystem::path& target : { manifestTarget, sceneTarget, materialTarget })
                std::filesystem::create_directories(target / "payload");

            Check(!StoreProjectManifest(manifestTarget, manifestA, error), "manifest store fails when the target cannot be replaced");
            Check(!sceneA.SaveToFile(sceneTarget), "scene save fails when the target cannot be replaced");
            Check(!materialA.SaveToFile(materialTarget), "material save fails when the target cannot be replaced");
            Check(ListNames(blocked) == std::vector<std::string> { "keep", "m.spiralmat", "m.spiralproject", "s.spiral" },
                "failed replacement leaves no temporary file in the directory");
            Check(std::filesystem::exists(manifestTarget / "payload") && std::filesystem::exists(sceneTarget / "payload")
                    && std::filesystem::exists(materialTarget / "payload"),
                "failed replacement leaves the blocking directories intact");
        }

#if defined(__linux__)
        // ----- unwritable directory: the previous file survives byte for byte -----
        {
            const std::filesystem::path locked = root.Path() / "locked";
            std::filesystem::create_directories(locked);
            const std::filesystem::path manifestTarget = locked / "m.spiralproject";
            const std::filesystem::path sceneTarget = locked / "s.spiral";
            const std::filesystem::path materialTarget = locked / "m.spiralmat";
            Check(StoreProjectManifest(manifestTarget, manifestA, error) && sceneA.SaveToFile(sceneTarget)
                    && materialA.SaveToFile(materialTarget),
                "initial files are written");
            const std::string manifestBefore = ReadAll(manifestTarget);
            const std::string sceneBefore = ReadAll(sceneTarget);
            const std::string materialBefore = ReadAll(materialTarget);
            std::error_code permissionError;
            std::filesystem::permissions(locked, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                std::filesystem::perm_options::replace, permissionError);
            if (access(locked.c_str(), W_OK) == 0)
            {
                std::cerr << "Project persistence test note: the unwritable-directory case is skipped because "
                             "this process can write regardless of permissions\n";
            }
            else
            {
                Check(!StoreProjectManifest(manifestTarget, manifestB, error), "manifest store fails in an unwritable directory");
                Check(!sceneB.SaveToFile(sceneTarget), "scene save fails in an unwritable directory");
                Check(!materialB.SaveToFile(materialTarget), "material save fails in an unwritable directory");
                Check(ReadAll(manifestTarget) == manifestBefore && ReadAll(sceneTarget) == sceneBefore
                        && ReadAll(materialTarget) == materialBefore,
                    "previous manifest, scene and material survive an unwritable directory byte for byte");
            }
            std::filesystem::permissions(locked, std::filesystem::perms::owner_all,
                std::filesystem::perm_options::add, permissionError);
            Check(ListNames(locked) == std::vector<std::string> { "m.spiralmat", "m.spiralproject", "s.spiral" },
                "no temporary files remain beside the previous versions");
        }

        // ----- real partial write: EFBIG after some bytes reached the temporary file -----
        {
            const std::filesystem::path partial = root.Path() / "partial";
            std::filesystem::create_directories(partial);
            const std::filesystem::path manifestTarget = partial / "m.spiralproject";
            const std::filesystem::path sceneTarget = partial / "s.spiral";
            const std::filesystem::path materialTarget = partial / "m.spiralmat";
            Check(StoreProjectManifest(manifestTarget, manifestA, error) && sceneA.SaveToFile(sceneTarget)
                    && materialA.SaveToFile(materialTarget),
                "baseline files are written before the partial-write injection");
            const std::string manifestBefore = ReadAll(manifestTarget);
            const std::string sceneBefore = ReadAll(sceneTarget);
            const std::string materialBefore = ReadAll(materialTarget);
            // A value smaller than every candidate file, larger than zero so a partial write happens.
            ScopedFileSizeLimit limit(64);
            Check(limit.IsApplied(), "the file size limit could be applied");
            if (limit.IsApplied())
            {
                Check(!StoreProjectManifest(manifestTarget, manifestB, error), "manifest store fails on a short write");
                Check(!sceneB.SaveToFile(sceneTarget), "scene save fails on a short write");
                Check(!materialB.SaveToFile(materialTarget), "material save fails on a short write");
            }
            Check(ReadAll(manifestTarget) == manifestBefore && ReadAll(sceneTarget) == sceneBefore
                    && ReadAll(materialTarget) == materialBefore,
                "a partial write leaves the previous manifest, scene and material untouched");
            Check(ListNames(partial) == std::vector<std::string> { "m.spiralmat", "m.spiralproject", "s.spiral" },
                "a partial write removes its temporary file");
        }
#endif

        // ----- success path: new bytes, parseable, no leftovers -----
        {
            const std::filesystem::path good = root.Path() / "good" / "nested";
            const std::filesystem::path manifestTarget = good / "m.spiralproject";
            const std::filesystem::path sceneTarget = good / "s.spiral";
            const std::filesystem::path materialTarget = good / "m.spiralmat";
            Check(StoreProjectManifest(manifestTarget, manifestA, error) && StoreProjectManifest(manifestTarget, manifestB, error),
                "manifest store creates parents and replaces");
            ProjectManifest loaded;
            Check(LoadProjectManifest(manifestTarget, loaded, error) && loaded == manifestB, "the replaced manifest is the new one");
            Check(sceneA.SaveToFile(sceneTarget) && sceneB.SaveToFile(sceneTarget), "scene save creates parents and replaces");
            Scene loadedScene;
            Check(Scene::LoadFromFile(sceneTarget, loadedScene) && loadedScene.GetName() == "Scene B",
                "the replaced scene is the new one");
            Check(materialA.SaveToFile(materialTarget) && materialB.SaveToFile(materialTarget), "material save replaces");
            MaterialAsset loadedMaterial;
            Check(MaterialAsset::LoadFromFile(materialTarget, loadedMaterial) && loadedMaterial.Name == "B"
                    && loadedMaterial.Metallic == 0.5f,
                "the replaced material is the new one");
            Check(ListNames(good) == std::vector<std::string> { "m.spiralmat", "m.spiralproject", "s.spiral" },
                "successful saves leave no temporary files");
        }

        // ----- readers never observe a torn file while a writer alternates versions -----
        {
            const std::filesystem::path race = root.Path() / "race";
            std::filesystem::create_directories(race);
            const std::filesystem::path manifestTarget = race / "m.spiralproject";
            const std::filesystem::path materialTarget = race / "m.spiralmat";
            Check(StoreProjectManifest(manifestTarget, manifestA, error) && materialA.SaveToFile(materialTarget),
                "race baseline is written");
            std::atomic<bool> done { false };
            std::atomic<u64> failedReads { 0 };
            std::atomic<u64> reads { 0 };
            std::thread reader([&]()
            {
                while (!done.load(std::memory_order_acquire))
                {
                    ProjectManifest manifest;
                    std::string readError;
                    MaterialAsset material;
                    if (!LoadProjectManifest(manifestTarget, manifest, readError)
                        || !(manifest == manifestA || manifest == manifestB)
                        || !MaterialAsset::LoadFromFile(materialTarget, material)
                        || (material.Name != "A" && material.Name != "B"))
                        failedReads.fetch_add(1, std::memory_order_relaxed);
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            });
            bool writesSucceeded = true;
            for (u32 iteration = 0; iteration < 400 && writesSucceeded; ++iteration)
            {
                std::string writeError;
                writesSucceeded = StoreProjectManifest(manifestTarget, iteration % 2 ? manifestA : manifestB, writeError)
                    && (iteration % 2 ? materialA : materialB).SaveToFile(materialTarget);
            }
            done.store(true, std::memory_order_release);
            reader.join();
            Check(writesSucceeded, "400 alternating atomic replacements succeeded");
            Check(reads.load() > 0 && failedReads.load() == 0,
                "a concurrent reader never observed a missing, torn, or unknown manifest or material");
        }

        // ----- no-replace directory publication -----
        {
            const std::filesystem::path base = root.Path() / "publish";
            std::filesystem::create_directories(base / "staged" / "meshes");
            WriteAll(base / "staged" / "meshes" / "a.bin", "payload");
            const std::filesystem::path finalDirectory = base / "project" / "fab" / "gen1";
            const DirectoryPublishStatus published = PublishDirectoryNoReplace(base / "staged", finalDirectory, error);
            Check(published == DirectoryPublishStatus::Published
                    || published == DirectoryPublishStatus::PublishedDurabilityUnconfirmed,
                "a staged directory is published and missing parents are created");
            Check(ReadAll(finalDirectory / "meshes" / "a.bin") == "payload" && !std::filesystem::exists(base / "staged"),
                "the published directory holds the payload and the staged name is gone");

            std::filesystem::create_directories(base / "second");
            WriteAll(base / "second" / "b.bin", "other");
            Check(PublishDirectoryNoReplace(base / "second", finalDirectory, error) == DirectoryPublishStatus::AlreadyExists
                    && ReadAll(finalDirectory / "meshes" / "a.bin") == "payload" && ReadAll(base / "second" / "b.bin") == "other"
                    && !std::filesystem::exists(finalDirectory / "b.bin"),
                "publishing onto an existing non-empty directory fails closed and changes neither side");

            std::filesystem::create_directories(base / "emptyTarget");
            Check(PublishDirectoryNoReplace(base / "second", base / "emptyTarget", error) == DirectoryPublishStatus::AlreadyExists
                    && std::filesystem::exists(base / "second" / "b.bin") && ListNames(base / "emptyTarget").empty(),
                "publishing onto an existing EMPTY directory is refused, never replaced");

            WriteAll(base / "fileTarget", "file");
            Check(PublishDirectoryNoReplace(base / "second", base / "fileTarget", error) == DirectoryPublishStatus::AlreadyExists
                    && ReadAll(base / "fileTarget") == "file",
                "publishing onto an existing file is refused");

            Check(PublishDirectoryNoReplace(base / "missing", base / "elsewhere", error) == DirectoryPublishStatus::Failed
                    && !std::filesystem::exists(base / "elsewhere"),
                "a missing staged directory fails");
            WriteAll(base / "plainfile", "x");
            Check(PublishDirectoryNoReplace(base / "plainfile", base / "elsewhere2", error) == DirectoryPublishStatus::Failed
                    && std::filesystem::exists(base / "plainfile") && !std::filesystem::exists(base / "elsewhere2"),
                "a regular file is never published as a directory");
            std::error_code linkError;
            std::filesystem::create_directory_symlink(finalDirectory, base / "linkdir", linkError);
            if (!linkError)
                Check(PublishDirectoryNoReplace(base / "linkdir", base / "elsewhere3", error) == DirectoryPublishStatus::Failed
                        && !std::filesystem::exists(base / "elsewhere3"),
                    "a symbolic link is never published as a directory");
            Check(PublishDirectoryNoReplace({}, base / "x", error) == DirectoryPublishStatus::Failed
                    && PublishDirectoryNoReplace(base / "second", {}, error) == DirectoryPublishStatus::Failed,
                "empty publication paths are rejected");
        }
        return passed;
    }
}
