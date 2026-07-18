#include "Engine/Core/Log.h"
#include "TestSupport/StructuredFuzz.h"

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace
{
    int ExerciseOrReport(std::span<const std::uint8_t> input, const std::filesystem::path& failureDirectory,
        std::string_view rerun)
    {
        const Spiral::Tests::StructuredFuzzResult result = Spiral::Tests::ExerciseStructuredInput(input);
        if (result.Passed) return 0;
        std::string error;
        Spiral::Tests::WriteFuzzFailure(failureDirectory, input, result.Message, rerun, error);
        std::cerr << "Structured fuzz invariant failed: " << result.Message << '\n';
        std::cerr << "Rerun: " << rerun << '\n';
        if (!error.empty()) std::cerr << "Failure artifact error: " << error << '\n';
        return 1;
    }
}

#ifdef GE_LIBFUZZER
extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    Engine::Log::Init();
    std::atexit([] { Engine::Log::Shutdown(); });
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, size_t size)
{
    if (ExerciseOrReport({ data, size }, "output/fuzz-failures",
        "EngineFuzzTests output/fuzz-failures/structured-fuzz-failure.input") != 0)
        std::abort();
    return 0;
}
#else
int main(int argc, char** argv)
{
    Engine::Log::Init();
    std::filesystem::path corpus = "Tests/Corpus/Fuzz";
    std::filesystem::path failureDirectory = "output/fuzz-failures";
    std::filesystem::path replay;
    std::uint64_t seed = 0x53504952414c2026ull;
    size_t iterations = 256;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        const auto value = [&]() -> std::string_view
        {
            if (++index >= argc) return {};
            return argv[index];
        };
        if (argument == "--corpus") corpus = value();
        else if (argument == "--failure-dir") failureDirectory = value();
        else if (argument == "--replay") replay = value();
        else if (argument == "--seed")
        {
            const std::string_view text = value();
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seed);
            if (error != std::errc {} || end != text.data() + text.size()) return 2;
        }
        else if (argument == "--iterations")
        {
            const std::string_view text = value();
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), iterations);
            if (error != std::errc {} || end != text.data() + text.size() || iterations == 0) return 2;
        }
        else return 2;
    }

    std::string error;
    if (!replay.empty())
    {
        std::vector<std::uint8_t> bytes;
        if (replay.extension() == ".case")
        {
            std::ifstream input(replay);
            const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            bytes = Spiral::Tests::ParseCorpusCase(text);
        }
        else
            bytes = Spiral::Tests::ReadBytes(replay);
        if (bytes.empty() || ExerciseOrReport(bytes, failureDirectory,
            "EngineFuzzTests --replay \"" + replay.string() + "\"") != 0) return 1;
    }
    else if (!Spiral::Tests::ReplayStructuredCorpus(corpus, failureDirectory, error))
    {
        std::cerr << error << '\n';
        return 1;
    }

    std::uint64_t state = seed;
    for (size_t iteration = 0; iteration < iterations; ++iteration)
    {
        std::vector<std::uint8_t> bytes(8);
        for (std::uint8_t& byte : bytes)
        {
            state += 0x9e3779b97f4a7c15ull;
            std::uint64_t mixed = state;
            mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ull;
            mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebull;
            byte = static_cast<std::uint8_t>((mixed ^ (mixed >> 31)) & 0xff);
        }
        if (ExerciseOrReport(bytes, failureDirectory,
            "EngineFuzzTests --replay \"" + (failureDirectory / "structured-fuzz-failure.input").string() + "\"") != 0)
            return 1;
    }
    Engine::Log::Shutdown();
    std::cout << "EngineFuzzSummaryV1 corpus=pass iterations=" << iterations << " seed=" << seed << " result=pass\n";
    return 0;
}
#endif
