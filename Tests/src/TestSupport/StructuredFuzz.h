#pragma once

#include "Engine/Renderer/PortableShaderContract.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
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

    inline std::string MakeSceneText(std::span<const std::uint8_t> input)
    {
        const auto byte = [&](size_t index, std::uint8_t fallback)
        {
            return index < input.size() ? input[index] : fallback;
        };
        const int sector = static_cast<int>(byte(2, 0)) - 128;
        const double local = (static_cast<int>(byte(3, 128)) - 128) * 0.25;
        std::ostringstream output;
        output << "SpiralScene 4\nName \"Structured Fuzz\"\n\n"
            << "[WorldGrid]\nVersion 1\nSectorExtent 4096\nOriginHysteresis 1024\nOriginMode ExactCamera\n\n"
            << "[MainCamera]\nPrimary true\nVerticalFovDegrees 45\nNearClip 0.1\nFarClip 1000\n"
            << "BackgroundColor 0.1 0.2 0.3\n\n[Entities]\nNextEntityId 2\nMainCameraEntity 1\n"
            << "Entity 1 \"Camera\"\nTransform 1 " << sector << " 0 0 " << local
            << " 0 0 0 0 0 1 1 1\nCamera 1 true 45 0.1 1000 0.1 0.2 0.3\n";
        return output.str();
    }

    inline StructuredFuzzResult ExerciseScene(std::span<const std::uint8_t> input)
    {
        const std::uint8_t mode = input.size() > 1 ? static_cast<std::uint8_t>(input[1] % 6) : 0;
        std::string candidate = MakeSceneText(input);
        bool mustAccept = mode == 0;
        if (mode == 1)
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
        if (loaded)
        {
            Engine::Scene reloaded;
            if (!output.SaveToFile(roundTrip) || !Engine::Scene::LoadFromFile(roundTrip, reloaded)
                || reloaded.GetName() != output.GetName() || reloaded.GetEntities().size() != output.GetEntities().size())
                return { false, "accepted Scene input failed canonical save/reload" };
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
