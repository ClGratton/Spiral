#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Spiral::Tests
{
    using ChoiceTrace = std::vector<std::uint64_t>;

    inline std::string SerializeTrace(const ChoiceTrace& trace)
    {
        std::string result;
        for (size_t index = 0; index < trace.size(); ++index)
        {
            if (index != 0) result += ',';
            result += std::to_string(trace[index]);
        }
        return result;
    }

    inline bool ParseTrace(std::string_view text, ChoiceTrace& trace)
    {
        trace.clear();
        if (text.empty()) return false;
        size_t offset = 0;
        while (offset < text.size())
        {
            const size_t comma = text.find(',', offset);
            const size_t end = comma == std::string_view::npos ? text.size() : comma;
            if (end == offset) return false;
            std::uint64_t value = 0;
            const auto [parsedEnd, error] = std::from_chars(text.data() + offset, text.data() + end, value);
            if (error != std::errc {} || parsedEnd != text.data() + end) return false;
            trace.push_back(value);
            if (comma == std::string_view::npos) break;
            offset = comma + 1;
        }
        return !trace.empty();
    }

    class ChoiceStream
    {
    public:
        explicit ChoiceStream(std::uint64_t seed)
            : m_State(seed)
        {
        }

        explicit ChoiceStream(const ChoiceTrace& replay)
            : m_Replay(&replay)
        {
        }

        std::uint64_t Next()
        {
            std::uint64_t value = 0;
            if (m_Replay)
            {
                if (m_ReplayIndex >= m_Replay->size())
                {
                    m_ReplayExhausted = true;
                    return 0;
                }
                value = (*m_Replay)[m_ReplayIndex++];
            }
            else
            {
                m_State += 0x9e3779b97f4a7c15ull;
                std::uint64_t mixed = m_State;
                mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ull;
                mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebull;
                value = mixed ^ (mixed >> 31);
            }
            m_Trace.push_back(value);
            return value;
        }

        bool NextBool() { return (Next() & 1ull) != 0; }

        size_t NextSize(size_t minimum, size_t maximum)
        {
            if (minimum >= maximum) return minimum;
            return minimum + static_cast<size_t>(Next() % (maximum - minimum + 1));
        }

        std::int64_t NextI64(std::int64_t minimum, std::int64_t maximum,
            const std::vector<std::int64_t>& boundaries = {})
        {
            const std::uint64_t choice = Next();
            if (!boundaries.empty() && (choice & 3ull) != 3ull)
                return boundaries[static_cast<size_t>((choice >> 2) % boundaries.size())];

            const auto ordered = [](std::int64_t value)
            {
                return static_cast<std::uint64_t>(value) ^ (1ull << 63);
            };
            const std::uint64_t first = ordered(minimum);
            const std::uint64_t last = ordered(maximum);
            const std::uint64_t span = last - first;
            const std::uint64_t selected = span == std::numeric_limits<std::uint64_t>::max()
                ? Next() : first + (Next() % (span + 1));
            return static_cast<std::int64_t>(selected ^ (1ull << 63));
        }

        const ChoiceTrace& Trace() const { return m_Trace; }
        bool ReplayExhausted() const { return m_ReplayExhausted; }

    private:
        std::uint64_t m_State = 0;
        const ChoiceTrace* m_Replay = nullptr;
        size_t m_ReplayIndex = 0;
        bool m_ReplayExhausted = false;
        ChoiceTrace m_Trace;
    };

    template <typename StillFails>
    ChoiceTrace MinimizeTrace(ChoiceTrace trace, StillFails&& stillFails)
    {
        if (trace.empty() || !stillFails(trace)) return trace;

        for (size_t chunk = trace.size() / 2; chunk > 0; chunk /= 2)
        {
            bool changed = true;
            while (changed)
            {
                changed = false;
                for (size_t offset = 0; offset + chunk <= trace.size(); ++offset)
                {
                    ChoiceTrace candidate = trace;
                    candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(offset),
                        candidate.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
                    if (!candidate.empty() && stillFails(candidate))
                    {
                        trace = std::move(candidate);
                        changed = true;
                        break;
                    }
                }
            }
        }

        for (size_t index = 0; index < trace.size(); ++index)
        {
            const std::uint64_t original = trace[index];
            for (const std::uint64_t candidateValue : { 0ull, 1ull, original / 2ull })
            {
                if (candidateValue == trace[index]) continue;
                ChoiceTrace candidate = trace;
                candidate[index] = candidateValue;
                if (stillFails(candidate)) trace = std::move(candidate);
            }
        }
        return trace;
    }

    struct CampaignOptions
    {
        std::uint64_t Seed = 0x53504952414c2026ull;
        size_t Iterations = 256;
        std::optional<ChoiceTrace> Replay;
    };

    struct Counterexample
    {
        std::uint64_t Seed = 0;
        size_t Iteration = 0;
        std::string Message;
        ChoiceTrace OriginalTrace;
        ChoiceTrace MinimizedTrace;
    };

    using Property = std::function<bool(ChoiceStream&, std::string&)>;

    inline bool RunCampaign(const CampaignOptions& options, const Property& property, Counterexample& failure)
    {
        const size_t iterations = options.Replay ? 1 : options.Iterations;
        for (size_t iteration = 0; iteration < iterations; ++iteration)
        {
            const std::uint64_t iterationSeed = options.Seed + iteration * 0x9e3779b97f4a7c15ull;
            ChoiceStream stream = options.Replay ? ChoiceStream(*options.Replay) : ChoiceStream(iterationSeed);
            std::string message;
            if (property(stream, message) && !stream.ReplayExhausted()) continue;

            failure.Seed = options.Seed;
            failure.Iteration = iteration;
            failure.Message = stream.ReplayExhausted() ? "replay trace exhausted" : std::move(message);
            failure.OriginalTrace = options.Replay ? *options.Replay : stream.Trace();
            const auto stillFails = [&](const ChoiceTrace& candidate)
            {
                ChoiceStream replay(candidate);
                std::string ignored;
                return !property(replay, ignored) && !replay.ReplayExhausted();
            };
            failure.MinimizedTrace = MinimizeTrace(failure.OriginalTrace, stillFails);
            return false;
        }
        return true;
    }

    inline std::string EscapeJson(std::string_view value)
    {
        std::string escaped;
        for (const unsigned char character : value)
        {
            switch (character)
            {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += static_cast<char>(character); break;
            }
        }
        return escaped;
    }

    inline bool WriteCounterexample(const std::filesystem::path& path, std::string_view testName,
        const Counterexample& failure, std::string_view rerunCommand, std::string& error)
    {
        std::error_code filesystemError;
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path(), filesystemError);
            if (filesystemError)
            {
                error = "could not create counterexample directory: " + filesystemError.message();
                return false;
            }
        }
        const std::filesystem::path temporary = path.string() + ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            error = "could not open counterexample temporary file";
            return false;
        }
        output << "{\n"
            << "  \"schema\":1,\n"
            << "  \"test\":\"" << EscapeJson(testName) << "\",\n"
            << "  \"seed\":" << failure.Seed << ",\n"
            << "  \"iteration\":" << failure.Iteration << ",\n"
            << "  \"message\":\"" << EscapeJson(failure.Message) << "\",\n"
            << "  \"originalTrace\":\"" << SerializeTrace(failure.OriginalTrace) << "\",\n"
            << "  \"minimizedTrace\":\"" << SerializeTrace(failure.MinimizedTrace) << "\",\n"
            << "  \"rerun\":\"" << EscapeJson(rerunCommand) << "\"\n"
            << "}\n";
        output.close();
        if (!output)
        {
            error = "could not write counterexample temporary file";
            std::filesystem::remove(temporary, filesystemError);
            return false;
        }
        if (std::filesystem::exists(path, filesystemError)) std::filesystem::remove(path, filesystemError);
        filesystemError.clear();
        std::filesystem::rename(temporary, path, filesystemError);
        if (filesystemError)
        {
            error = "could not publish counterexample: " + filesystemError.message();
            std::filesystem::remove(temporary, filesystemError);
            return false;
        }
        return true;
    }
}
