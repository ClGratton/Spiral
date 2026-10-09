#pragma once

#include "GeneratedTest.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace Spiral::Tests
{
    // Uniform double in [0, 1) with 53 random bits.
    inline double UnitDouble(ChoiceStream& stream)
    {
        return static_cast<double>(stream.Next() >> 11) * (1.0 / 9007199254740992.0);
    }

    inline double RangeDouble(ChoiceStream& stream, double minimum, double maximum)
    {
        return minimum + (maximum - minimum) * UnitDouble(stream);
    }

    // Runs a generated property. The seed and a replay trace can be overridden
    // through <envPrefix>_SEED and <envPrefix>_REPLAY; a failure prints the
    // seed, both traces, the rerun line and writes a counterexample JSON under
    // the system temp directory.
    inline bool RunNamedProperty(
        std::string_view suite,
        std::string_view name,
        const std::string& envPrefix,
        size_t iterations,
        const Property& property)
    {
        CampaignOptions options;
        options.Iterations = iterations;
        if (const char* seed = std::getenv((envPrefix + "_SEED").c_str()))
            options.Seed = std::strtoull(seed, nullptr, 10);
        ChoiceTrace replay;
        if (const char* trace = std::getenv((envPrefix + "_REPLAY").c_str()); trace && ParseTrace(trace, replay))
            options.Replay = replay;

        Counterexample failure;
        if (RunCampaign(options, property, failure))
            return true;

        const std::string minimized = SerializeTrace(failure.MinimizedTrace);
        const std::string rerun = envPrefix + "_SEED=" + std::to_string(failure.Seed)
            + " " + envPrefix + "_REPLAY=\"" + minimized + "\" EngineTests --test <registered name of "
            + std::string(name) + ">";
        const std::filesystem::path artifact = std::filesystem::temp_directory_path()
            / ("spiral-" + std::string(suite) + "-counterexample-" + std::string(name) + ".json");
        std::string artifactError;
        const bool written = WriteCounterexample(artifact, name, failure, rerun, artifactError);
        std::cerr << suite << " property failed [" << name << "]: " << failure.Message
            << " seed=" << failure.Seed << " iteration=" << failure.Iteration
            << " originalTrace=" << SerializeTrace(failure.OriginalTrace)
            << " minimizedTrace=" << minimized << "\n  rerun: " << rerun << '\n';
        if (written)
            std::cerr << "  counterexample: " << artifact.string() << '\n';
        else
            std::cerr << "  counterexample write failed: " << artifactError << '\n';
        return false;
    }
}
