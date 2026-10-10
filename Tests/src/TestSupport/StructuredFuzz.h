#pragma once

#include "Engine/Renderer/PortableShaderContract.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Spiral::Tests
{
    struct StructuredFuzzResult
    {
        bool Passed = true;
        std::string Message;
    };

    inline std::filesystem::path StructuredFuzzTemporaryPath(std::string_view name)
    {
        return std::filesystem::temp_directory_path() / ("spiral-structured-fuzz-" + std::string(name));
    }

    inline bool WriteBytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return !!output;
    }

    inline std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    }

    inline std::vector<std::uint8_t> ParseCorpusCase(std::string_view text)
    {
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
            text.remove_suffix(1);
        std::vector<std::uint8_t> bytes;
        size_t offset = 0;
        while (offset < text.size())
        {
            const size_t comma = text.find(',', offset);
            const size_t end = comma == std::string_view::npos ? text.size() : comma;
            std::uint32_t value = 0;
            const auto [parsedEnd, error] = std::from_chars(text.data() + offset, text.data() + end, value);
            if (end == offset || error != std::errc {} || parsedEnd != text.data() + end || value > 255)
                return {};
            bytes.push_back(static_cast<std::uint8_t>(value));
            if (comma == std::string_view::npos) break;
            offset = comma + 1;
        }
        return bytes;
    }

    // Fields of the generated scene that the hostile modes replace. Every default
    // reproduces the original generated text byte for byte, so the stored corpus
    // keeps exercising the same inputs.
    struct SceneTextFields
    {
        std::string EntityId = "1";
        std::string TransformId = "1";
        std::string Rotation = "0 0 0";
        std::string Scale = "1 1 1";
        std::string Projection = "45 0.1 1000";
        std::string Background = "0.1 0.2 0.3";
        std::string NextEntityId = "2";
        std::string MainCameraEntity = "1";
        std::string EntityName = "Camera";
        std::string ExtraRecords;
        bool CameraBeforeTransform = false;
    };

    inline std::string MakeSceneText(std::span<const std::uint8_t> input, const SceneTextFields& fields = {})
    {
        const auto byte = [&](size_t index, std::uint8_t fallback)
        {
            return index < input.size() ? input[index] : fallback;
        };
        const int sector = static_cast<int>(byte(2, 0)) - 128;
        const double local = (static_cast<int>(byte(3, 128)) - 128) * 0.25;
        std::ostringstream transform;
        transform << "Transform " << fields.TransformId << ' ' << sector << " 0 0 " << local
            << " 0 0 " << fields.Rotation << ' ' << fields.Scale << '\n';
        const std::string camera = "Camera " + fields.EntityId + " true " + fields.Projection + ' ' + fields.Background + '\n';
        std::ostringstream output;
        output << "SpiralScene 5\nName \"Structured Fuzz\"\n\n"
            << "[WorldGrid]\nVersion 1\nSectorExtent 4096\nOriginHysteresis 1024\nOriginMode ExactCamera\n\n"
            << "[MainCamera]\nPrimary true\nVerticalFovDegrees 45\nNearClip 0.1\nFarClip 1000\n"
            << "BackgroundColor 0.1 0.2 0.3\n\n[Entities]\nNextEntityId " << fields.NextEntityId
            << "\nMainCameraEntity " << fields.MainCameraEntity << "\n"
            << "Entity " << fields.EntityId << " \"" << fields.EntityName << "\"\n"
            << (fields.CameraBeforeTransform ? camera + transform.str() : transform.str() + camera)
            << fields.ExtraRecords;
        return output.str();
    }

    // The variants the hostile and stress modes pick from, indexed by a corpus
    // byte. Rejected variants reproduce defects a load-time validator must keep
    // out; accepted ones stress escapes, extreme ids and an absent main camera.
    inline std::string MakeVariantSceneText(std::span<const std::uint8_t> input, std::uint8_t mode, bool& outMustReject)
    {
        const std::uint8_t token = input.size() > 4 ? input[4] : 0;
        const auto choose = [&](std::initializer_list<const char*> values)
        {
            return std::string(*(values.begin() + token % values.size()));
        };
        SceneTextFields fields;
        outMustReject = true;
        switch (mode)
        {
            case 6:
            {
                // The hostile scale lands on a plain second entity (so the camera's own
                // unit-scale rule cannot mask a missing transform check), except the
                // non-unit variant that targets the camera itself.
                const std::string scale = choose({ "0 1 1", "1 -1 1", "1 1 1e-39", "nan 1 1", "5 5 5", "1 inf 1", "0 0 0" });
                if (scale == "5 5 5")
                    fields.Scale = scale;
                else
                {
                    fields.NextEntityId = "3";
                    fields.ExtraRecords = "Entity 2 \"Plain\"\nTransform 2 0 0 0 0 0 0 0 0 0 " + scale + "\n";
                }
                break;
            }
            case 7:
                fields.Projection = choose({ "0 0.1 1000", "180 0.1 1000", "-5 0.1 1000", "nan 0.1 1000",
                    "45 0 1000", "45 -1 1000", "45 10 10", "45 10 5", "45 0.1 inf" });
                break;
            case 8:
            {
                // Plain unsigned decimals only: "-1" would wrap through unsigned stream extraction.
                const std::string id = choose({ "-1", "4294967296", "0x1", "+1", "1.5", "99999999999" });
                switch ((input.size() > 5 ? input[5] : 0) % 4)
                {
                    case 0: fields.TransformId = id; break;
                    case 1: fields.NextEntityId = id; break;
                    case 2: fields.EntityId = fields.TransformId = id; break;
                    default: fields.MainCameraEntity = id; break;
                }
                break;
            }
            case 9: fields.CameraBeforeTransform = true; fields.Scale = choose({ "5 5 5", "1 1 2", "0.5 1 1" }); break;
            case 10: fields.Rotation = choose({ "nan 0 0", "inf 0 0", "0 -inf 0" }); break;
            case 11:
            {
                // A plain second entity carries the stress values (the camera must
                // keep unit scale): escapes in the name, extreme but valid scale and
                // rotation.
                outMustReject = false;
                const std::string scale = choose({ "0.5 2 1e-30", "100 100 100", "1.17549435e-38 1 1" });
                const std::string rotation = choose({ "1 2 3", "720 -720 360", "1e30 0 0" });
                const std::string name = choose({ "a\\\"b\\\\c d", "  spaced  ", "tab\there", "[bracket]", "" });
                fields.NextEntityId = "3";
                fields.ExtraRecords = "Entity 2 \"" + name + "\"\nTransform 2 0 0 0 0 0 0 " + rotation + ' ' + scale + "\n";
                break;
            }
            case 12:
                outMustReject = false;
                fields.EntityId = fields.TransformId = "4294967295";
                fields.NextEntityId = fields.MainCameraEntity = "4294967295";
                break;
            case 13: outMustReject = false; fields.MainCameraEntity = "0"; break;
            default: break;
        }
        return MakeSceneText(input, fields);
    }

    // The invariants every accepted scene must satisfy, evaluated from the outside
    // without the Scene validators: finite rotation, normal-range positive scale
    // with a finite reciprocal, unit scale and a renderable projection on cameras.
    inline bool EntityHoldsSceneInvariants(const Engine::SceneEntity& entity)
    {
        const Engine::Math::Vec3& scale = entity.Transform.Scale;
        const Engine::Math::Vec3& rotation = entity.Transform.RotationDegrees;
        constexpr float smallest = std::numeric_limits<float>::min();
        if (!std::isfinite(rotation.X) || !std::isfinite(rotation.Y) || !std::isfinite(rotation.Z)
            || !(scale.X >= smallest) || !(scale.Y >= smallest) || !(scale.Z >= smallest)
            || !std::isfinite(1.0f / scale.X) || !std::isfinite(1.0f / scale.Y) || !std::isfinite(1.0f / scale.Z))
            return false;
        if (entity.Camera)
        {
            const Engine::CameraProjection& projection = entity.Camera->Projection;
            if (scale.X != 1.0f || scale.Y != 1.0f || scale.Z != 1.0f
                || !(projection.VerticalFovDegrees > 0.0f) || !(projection.VerticalFovDegrees < 180.0f)
                || !(projection.NearClip > 0.0f) || !(projection.FarClip > projection.NearClip)
                || !std::isfinite(projection.FarClip))
                return false;
        }
        return true;
    }

    inline bool SameSceneEntities(const Engine::Scene& a, const Engine::Scene& b)
    {
        if (a.GetEntities().size() != b.GetEntities().size() || !(a.GetMainCameraEntity() == b.GetMainCameraEntity()))
            return false;
        for (size_t index = 0; index < a.GetEntities().size(); ++index)
        {
            const Engine::SceneEntity& x = a.GetEntities()[index];
            const Engine::SceneEntity& y = b.GetEntities()[index];
            const Engine::Math::SectorLocalPosition& px = x.Transform.GetPosition();
            const Engine::Math::SectorLocalPosition& py = y.Transform.GetPosition();
            if (!(x.EntityHandle == y.EntityHandle) || x.Name != y.Name
                || !(px.Sector == py.Sector) || px.Local.X != py.Local.X || px.Local.Y != py.Local.Y || px.Local.Z != py.Local.Z
                || x.Transform.RotationDegrees.X != y.Transform.RotationDegrees.X
                || x.Transform.RotationDegrees.Y != y.Transform.RotationDegrees.Y
                || x.Transform.RotationDegrees.Z != y.Transform.RotationDegrees.Z
                || x.Transform.Scale.X != y.Transform.Scale.X || x.Transform.Scale.Y != y.Transform.Scale.Y
                || x.Transform.Scale.Z != y.Transform.Scale.Z
                || x.Camera.has_value() != y.Camera.has_value())
                return false;
            if (x.Camera
                && (x.Camera->Projection.VerticalFovDegrees != y.Camera->Projection.VerticalFovDegrees
                    || x.Camera->Projection.NearClip != y.Camera->Projection.NearClip
                    || x.Camera->Projection.FarClip != y.Camera->Projection.FarClip
                    || x.Camera->Primary != y.Camera->Primary))
                return false;
        }
        return true;
    }

    inline StructuredFuzzResult ExerciseScene(std::span<const std::uint8_t> input)
    {
        const std::uint8_t mode = input.size() > 1 ? static_cast<std::uint8_t>(input[1] % 14) : 0;
        std::string candidate = MakeSceneText(input);
        bool mustAccept = mode == 0;
        bool mustReject = false;
        if (mode >= 6)
        {
            candidate = MakeVariantSceneText(input, mode, mustReject);
            mustAccept = !mustReject;
        }
        else if (mode == 1)
            candidate.replace(12, 1, "9");
        else if (mode == 2)
            candidate.resize(std::max<size_t>(1, candidate.size() / 2));
        else if (mode == 3)
            candidate += "Entity 1 \"Duplicate\"\n";
        else if (mode == 4 && !candidate.empty())
            candidate[(input.size() > 2 ? input[2] : 0) % candidate.size()] ^= 0x20;
        else if (mode == 5)
        {
            const std::string other = MakeSceneText({});
            candidate = candidate.substr(0, candidate.size() / 2) + other.substr(other.size() / 2);
        }

        const std::filesystem::path source = StructuredFuzzTemporaryPath("scene.spiral");
        const std::filesystem::path roundTrip = StructuredFuzzTemporaryPath("scene-roundtrip.spiral");
        const std::vector<std::uint8_t> bytes(candidate.begin(), candidate.end());
        if (!WriteBytes(source, bytes)) return { false, "could not write generated Scene candidate" };

        Engine::Scene output("FuzzSentinel");
        output.CreateEntity("KeepSentinel");
        const size_t sentinelCount = output.GetEntities().size();
        const bool loaded = Engine::Scene::LoadFromFile(source, output);
        if (!loaded && (output.GetName() != "FuzzSentinel" || output.GetEntities().size() != sentinelCount))
            return { false, "rejected Scene input mutated the caller-owned destination" };
        if (mustAccept && !loaded)
            return { false, "valid generated Scene input was rejected" };
        if (mustReject && loaded)
            return { false, "hostile generated Scene input was accepted (an invariant is not enforced at load)" };
        if (loaded && mode == 13 && output.GetMainCameraEntity().IsValid())
            return { false, "an explicit 'no main camera' was replaced by a promoted camera on load" };
        if (loaded)
        {
            for (const Engine::SceneEntity& entity : output.GetEntities())
                if (!EntityHoldsSceneInvariants(entity))
                    return { false, "an accepted Scene holds an entity that violates the transform or camera invariants" };
            Engine::Scene reloaded;
            if (!output.SaveToFile(roundTrip) || !Engine::Scene::LoadFromFile(roundTrip, reloaded)
                || reloaded.GetName() != output.GetName() || reloaded.GetEntities().size() != output.GetEntities().size())
                return { false, "accepted Scene input failed canonical save/reload" };
            if (!SameSceneEntities(output, reloaded))
                return { false, "accepted Scene input did not round-trip entity for entity" };
        }
        std::error_code error;
        std::filesystem::remove(source, error);
        std::filesystem::remove(roundTrip, error);
        return {};
    }

    inline Engine::PortableShaderRequest MakeFuzzShaderRequest()
    {
        Engine::PortableShaderRequest request;
        request.SourceName = "structured-fuzz.slang";
        request.Source = "float4 main() : SV_Target { return 1; }";
        request.EntryPoint = "main";
        request.Stage = Engine::RHI::ShaderStage::Pixel;
        request.Targets = { Engine::PortableShaderTarget::Spirv };
        request.CompilerIdentity = "StructuredFuzz";
        request.CompilerVersion = "1";
        request.CompilerPackageHash = "test-only";
        return request;
    }

    inline StructuredFuzzResult ExercisePortableShader(std::span<const std::uint8_t> input)
    {
        const Engine::PortableShaderRequest request = MakeFuzzShaderRequest();
        Engine::PortableShaderPackage package;
        package.Key = Engine::PortableShaderContract::CacheKey(request);
        package.Spirv = { 3, 2, 23, 7 };
        const std::filesystem::path baseline = StructuredFuzzTemporaryPath("shader-baseline.shaderpkg");
        const std::filesystem::path candidatePath = StructuredFuzzTemporaryPath("shader-candidate.shaderpkg");
        if (!Engine::PortableShaderContract::StoreAtomic(baseline, package))
            return { false, "could not create a valid portable-shader seed package" };
        std::vector<std::uint8_t> candidate = ReadBytes(baseline);
        const std::uint8_t mode = input.size() > 1 ? static_cast<std::uint8_t>(input[1] % 6) : 0;
        const bool mustAccept = mode == 0;
        if (mode == 1 && candidate.size() > 5)
            candidate[5] = 99;
        else if (mode == 2 && !candidate.empty())
            candidate.resize(std::max<size_t>(1, candidate.size() / 2));
        else if (mode == 3 && !candidate.empty())
            candidate[0] ^= 0x7f;
        else if (mode == 4 && !candidate.empty())
            candidate[(input.size() > 2 ? input[2] : 0) % candidate.size()] ^= 1;
        else if (mode == 5 && candidate.size() > 2)
        {
            std::vector<std::uint8_t> other(candidate.rbegin(), candidate.rend());
            std::copy(other.begin() + static_cast<std::ptrdiff_t>(other.size() / 2), other.end(),
                candidate.begin() + static_cast<std::ptrdiff_t>(candidate.size() / 2));
        }
        if (!WriteBytes(candidatePath, candidate)) return { false, "could not write portable-shader candidate" };

        Engine::PortableShaderPackage sentinel;
        sentinel.Version = 77;
        sentinel.Key = "FuzzSentinel";
        sentinel.Spirv = { 91 };
        Engine::PortableShaderPackage output = sentinel;
        const bool loaded = Engine::PortableShaderContract::Load(candidatePath, request, output);
        if (!loaded && output != sentinel)
            return { false, "rejected portable-shader input mutated the caller-owned destination" };
        std::string validationError;
        if (mustAccept && !loaded)
            return { false, "valid generated portable-shader package was rejected" };
        if (loaded && !Engine::PortableShaderContract::ValidatePackage(request, output, validationError))
            return { false, "accepted portable-shader package failed semantic validation" };
        std::error_code error;
        std::filesystem::remove(baseline, error);
        std::filesystem::remove(candidatePath, error);
        return {};
    }

    inline StructuredFuzzResult ExerciseStructuredInput(std::span<const std::uint8_t> input)
    {
        if (input.empty()) return {};
        return (input[0] & 1) == 0 ? ExerciseScene(input) : ExercisePortableShader(input);
    }

    inline bool WriteFuzzFailure(const std::filesystem::path& directory, std::span<const std::uint8_t> input,
        std::string_view message, std::string_view rerun, std::string& error)
    {
        std::error_code filesystemError;
        std::filesystem::create_directories(directory, filesystemError);
        if (filesystemError) { error = filesystemError.message(); return false; }
        const std::filesystem::path inputPath = directory / "structured-fuzz-failure.input";
        if (!WriteBytes(inputPath, input)) { error = "could not write failure input"; return false; }
        std::ofstream manifest(directory / "structured-fuzz-failure.txt", std::ios::trunc);
        manifest << "StructuredFuzzFailureV1\nmessage=" << message << "\nrerun=" << rerun << '\n';
        if (!manifest) { error = "could not write failure manifest"; return false; }
        return true;
    }

    inline bool ReplayStructuredCorpus(const std::filesystem::path& corpusDirectory,
        const std::filesystem::path& failureDirectory, std::string& error)
    {
        if (!std::filesystem::is_directory(corpusDirectory))
        {
            error = "structured fuzz corpus directory is missing: " + corpusDirectory.string();
            return false;
        }
        std::vector<std::filesystem::path> cases;
        for (const auto& entry : std::filesystem::directory_iterator(corpusDirectory))
            if (entry.is_regular_file() && entry.path().extension() == ".case") cases.push_back(entry.path());
        std::sort(cases.begin(), cases.end());
        if (cases.empty()) { error = "structured fuzz corpus is empty"; return false; }
        for (const std::filesystem::path& path : cases)
        {
            std::ifstream input(path);
            const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            const std::vector<std::uint8_t> bytes = ParseCorpusCase(text);
            if (bytes.empty()) { error = "invalid structured corpus case: " + path.string(); return false; }
            const StructuredFuzzResult result = ExerciseStructuredInput(bytes);
            if (!result.Passed)
            {
                const std::string rerun = "EngineFuzzTests --replay \"" + path.string() + "\"";
                std::string artifactError;
                WriteFuzzFailure(failureDirectory, bytes, result.Message, rerun, artifactError);
                error = result.Message + (artifactError.empty() ? "" : "; artifact: " + artifactError);
                return false;
            }
        }
        return true;
    }
}
