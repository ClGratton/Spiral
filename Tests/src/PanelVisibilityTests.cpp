#include "PanelVisibilityTests.h"

#include "PanelVisibility.h"
#include "TestSupport/GeneratedTest.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace SpiralEditor::Layout;
    using Spiral::Tests::ChoiceStream;

    // Failure hypotheses, oracles, and non-claims:
    // - The codec must be strict: a file the Editor did not write must never
    //   half-apply. The oracle is a hand-written accepted/rejected corpus plus a
    //   structure-aware mutation property (delete, insert, flip, truncate,
    //   duplicate a line of a valid file) whose invariant is "rejected, or the
    //   parsed table re-formats and re-parses to itself".
    // - Tier: fast, pure, no filesystem. Replay with SPIRAL_PANELS_SEED /
    //   SPIRAL_PANELS_REPLAY. Not claimed: the Editor's atomic file write, the
    //   ImGui close button, or dock restoration of a reopened panel.

    const std::vector<std::string> kKnown = { "scene.hierarchy", "scene.inspector", "scene.viewport",
        "content.browser", "diagnostics.console", "diagnostics.profiler" };

    struct Checker
    {
        const char* Suite;
        bool Ok = true;

        void Expect(bool condition, const std::string& message)
        {
            if (!condition)
            {
                std::cerr << "Panel visibility test failed [" << Suite << "]: " << message << '\n';
                Ok = false;
            }
        }
    };

    bool Same(const std::vector<PanelVisibility>& left, const std::vector<PanelVisibility>& right)
    {
        if (left.size() != right.size())
            return false;
        for (size_t index = 0; index < left.size(); ++index)
        {
            if (left[index].Id != right[index].Id || left[index].Visible != right[index].Visible)
                return false;
        }
        return true;
    }
}

namespace SpiralTests
{
    bool TestPanelVisibilityRoundTripsEveryCombinationAndMatchesHandWrittenText()
    {
        Checker check { "round trip" };

        // Hand-written canonical text for one table.
        const std::vector<PanelVisibility> sample = { { "scene.hierarchy", true }, { "scene.inspector", false },
            { "scene.viewport", true }, { "content.browser", false }, { "diagnostics.console", true },
            { "diagnostics.profiler", false } };
        const std::string expected =
            "SpiralEditorPanels 1\n"
            "Panel scene.hierarchy 1\n"
            "Panel scene.inspector 0\n"
            "Panel scene.viewport 1\n"
            "Panel content.browser 0\n"
            "Panel diagnostics.console 1\n"
            "Panel diagnostics.profiler 0\n";
        check.Expect(FormatPanelVisibility(sample) == expected, "canonical text is byte exact");

        // Every one of the 2^6 visibility combinations survives a round trip.
        for (unsigned mask = 0; mask < (1u << kKnown.size()); ++mask)
        {
            std::vector<PanelVisibility> table;
            for (size_t index = 0; index < kKnown.size(); ++index)
                table.push_back({ kKnown[index], (mask & (1u << index)) != 0 });
            std::vector<PanelVisibility> parsed;
            std::string error;
            check.Expect(ParsePanelVisibility(FormatPanelVisibility(table), kKnown, parsed, error) && Same(parsed, table),
                "combination " + std::to_string(mask) + " round-trips");
        }

        // Header only: valid, everything keeps its default (visible).
        std::vector<PanelVisibility> parsed;
        std::string error;
        check.Expect(ParsePanelVisibility("SpiralEditorPanels 1\n", kKnown, parsed, error) && parsed.size() == kKnown.size()
                && std::all_of(parsed.begin(), parsed.end(), [](const PanelVisibility& p) { return p.Visible; }),
            "a header-only file means all defaults");
        // A panel missing from the file stays visible; order in the file does not matter.
        check.Expect(ParsePanelVisibility("SpiralEditorPanels 1\nPanel scene.viewport 0\nPanel scene.hierarchy 0\n", kKnown, parsed, error)
                && parsed.size() == kKnown.size() && parsed[0].Id == "scene.hierarchy" && !parsed[0].Visible
                && parsed[2].Id == "scene.viewport" && !parsed[2].Visible && parsed[1].Visible,
            "missing panels default to visible and file order is irrelevant");
        return check.Ok;
    }

    bool TestPanelVisibilityRejectsMalformedFilesWithoutChangingTheOutput()
    {
        Checker check { "rejection corpus" };
        const std::vector<PanelVisibility> sentinel = { { "sentinel", false } };

        struct Case
        {
            const char* Name;
            std::string Text;
        };
        const std::vector<Case> corpus = {
            { "empty", "" },
            { "header without newline", "SpiralEditorPanels 1" },
            { "wrong magic", "SpiralEditorSettings 1\n" },
            { "future version", "SpiralEditorPanels 2\n" },
            { "version zero", "SpiralEditorPanels 0\n" },
            { "version with padding", "SpiralEditorPanels 01\n" },
            { "extra token in header", "SpiralEditorPanels 1 x\n" },
            { "crlf", "SpiralEditorPanels 1\r\nPanel scene.viewport 1\r\n" },
            { "missing final newline", "SpiralEditorPanels 1\nPanel scene.viewport 1" },
            { "unknown id", "SpiralEditorPanels 1\nPanel scene.minimap 1\n" },
            { "duplicate id", "SpiralEditorPanels 1\nPanel scene.viewport 1\nPanel scene.viewport 0\n" },
            { "flag 2", "SpiralEditorPanels 1\nPanel scene.viewport 2\n" },
            { "flag word", "SpiralEditorPanels 1\nPanel scene.viewport true\n" },
            { "flag missing", "SpiralEditorPanels 1\nPanel scene.viewport\n" },
            { "flag missing with space", "SpiralEditorPanels 1\nPanel scene.viewport \n" },
            { "extra token", "SpiralEditorPanels 1\nPanel scene.viewport 1 extra\n" },
            { "blank line", "SpiralEditorPanels 1\n\nPanel scene.viewport 1\n" },
            { "tab separator", "SpiralEditorPanels 1\nPanel\tscene.viewport 1\n" },
            { "leading space", "SpiralEditorPanels 1\n Panel scene.viewport 1\n" },
            { "wrong keyword", "SpiralEditorPanels 1\nWindow scene.viewport 1\n" },
            { "empty id", "SpiralEditorPanels 1\nPanel  1\n" },
            { "id with space", "SpiralEditorPanels 1\nPanel scene viewport 1\n" },
            { "embedded nul", std::string("SpiralEditorPanels 1\nPanel scene.viewport") + '\0' + " 1\n" },
        };
        for (const Case& item : corpus)
        {
            std::vector<PanelVisibility> out = sentinel;
            std::string error;
            check.Expect(!ParsePanelVisibility(item.Text, kKnown, out, error), std::string("rejects: ") + item.Name);
            check.Expect(Same(out, sentinel) && !error.empty(), std::string("leaves the output untouched and names a reason: ") + item.Name);
        }

        // Oversize.
        std::string huge = "SpiralEditorPanels 1\n";
        while (huge.size() <= kMaximumPanelVisibilityBytes)
            huge += "Panel scene.viewport 1\n";
        std::vector<PanelVisibility> out = sentinel;
        std::string error;
        check.Expect(!ParsePanelVisibility(huge, kKnown, out, error) && error == "oversized_file", "oversized file rejected");
        // The size limit is inclusive: exactly the maximum is judged by content, not size.
        return check.Ok;
    }

    bool TestPanelVisibilityMutatedFilesAreRejectedOrSelfConsistent()
    {
        Checker check { "mutation property" };
        size_t accepted = 0;
        size_t rejected = 0;

        Spiral::Tests::CampaignOptions options;
        options.Iterations = 600;
        if (const char* seed = std::getenv("SPIRAL_PANELS_SEED"))
            options.Seed = std::strtoull(seed, nullptr, 10);
        Spiral::Tests::ChoiceTrace replay;
        if (const char* trace = std::getenv("SPIRAL_PANELS_REPLAY"); trace && Spiral::Tests::ParseTrace(trace, replay))
            options.Replay = replay;

        const Spiral::Tests::Property property = [&](ChoiceStream& stream, std::string& message)
        {
            std::vector<PanelVisibility> table;
            for (const std::string& id : kKnown)
            {
                if (stream.NextBool() || table.size() < 2) // keep the file non-trivial
                    table.push_back({ id, stream.NextBool() });
            }
            std::string text = FormatPanelVisibility(table);
            const size_t mutations = stream.NextSize(0, 3);
            for (size_t mutation = 0; mutation < mutations && !text.empty(); ++mutation)
            {
                const size_t position = stream.NextSize(0, text.size() - 1);
                switch (stream.NextSize(0, 4))
                {
                case 0: text.erase(position, 1); break;
                case 1: text.insert(position, 1, static_cast<char>(stream.NextSize(0, 255))); break;
                case 2: text[position] = static_cast<char>(stream.NextSize(0, 255)); break;
                case 3: text.resize(position); break;
                default:
                {
                    const size_t lineStart = text.rfind('\n', position == 0 ? 0 : position - 1);
                    const size_t from = lineStart == std::string::npos ? 0 : lineStart + 1;
                    const size_t to = text.find('\n', from);
                    if (to != std::string::npos)
                        text.insert(to + 1, text.substr(from, to - from + 1));
                    break;
                }
                }
            }

            const std::vector<PanelVisibility> sentinel = { { "sentinel", false } };
            std::vector<PanelVisibility> out = sentinel;
            std::string error;
            if (!ParsePanelVisibility(text, kKnown, out, error))
            {
                ++rejected;
                if (!Same(out, sentinel) || error.empty())
                {
                    message = "a rejected file changed the output or gave no reason";
                    return false;
                }
                return true;
            }
            ++accepted;
            // Self consistency: ids in known order, and the canonical text of the result parses to itself.
            std::vector<PanelVisibility> reparsed;
            if (out.size() != kKnown.size() || !ParsePanelVisibility(FormatPanelVisibility(out), kKnown, reparsed, error)
                || !Same(reparsed, out))
            {
                message = "an accepted file is not self consistent";
                return false;
            }
            for (size_t index = 0; index < kKnown.size(); ++index)
            {
                if (out[index].Id != kKnown[index])
                {
                    message = "accepted table is not in known-id order";
                    return false;
                }
            }
            return true;
        };

        Spiral::Tests::Counterexample failure;
        if (!Spiral::Tests::RunCampaign(options, property, failure))
        {
            const std::filesystem::path artifact = std::filesystem::temp_directory_path() / "spiral-panels-counterexample.json";
            std::string artifactError;
            Spiral::Tests::WriteCounterexample(artifact, "panel-visibility-mutation", failure,
                "SPIRAL_PANELS_SEED=" + std::to_string(failure.Seed) + " SPIRAL_PANELS_REPLAY=\""
                    + Spiral::Tests::SerializeTrace(failure.MinimizedTrace) + "\"", artifactError);
            std::cerr << "Panel visibility property failed: " << failure.Message << " seed=" << failure.Seed
                      << " counterexample=" << artifact.string() << '\n';
            check.Ok = false;
        }
        check.Expect(accepted > 50 && rejected > 50,
            "the campaign exercises both outcomes (accepted " + std::to_string(accepted) + ", rejected " + std::to_string(rejected) + ")");
        return check.Ok;
    }
}
