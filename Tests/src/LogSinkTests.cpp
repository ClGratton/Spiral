#include "LogSinkTests.h"

#include "TestSupport/GeneratedTest.h"

#include "Engine/Core/Log.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace SpiralTests
{
    namespace
    {
        using Engine::Log;
        using Level = Engine::Log::Level;

        constexpr std::string_view kLevelNames[] = { "Trace", "Info", "Warn", "Error" };

        struct Checker
        {
            bool Passed = true;

            void operator()(bool condition, std::string_view message)
            {
                if (!condition)
                {
                    std::cerr << "Log sink test failed: " << message << '\n';
                    Passed = false;
                }
            }
        };

        // Redirects the two console streams Log writes to, so the exact bytes can be compared.
        class ConsoleCapture
        {
        public:
            ConsoleCapture()
                : m_OldOut(std::cout.rdbuf(m_Out.rdbuf())), m_OldErr(std::cerr.rdbuf(m_Err.rdbuf()))
            {
            }

            ~ConsoleCapture()
            {
                std::cout.rdbuf(m_OldOut);
                std::cerr.rdbuf(m_OldErr);
            }

            ConsoleCapture(const ConsoleCapture&) = delete;
            ConsoleCapture& operator=(const ConsoleCapture&) = delete;

            std::string Out() const { return m_Out.str(); }
            std::string Err() const { return m_Err.str(); }

        private:
            std::ostringstream m_Out;
            std::ostringstream m_Err;
            std::streambuf* m_OldOut;
            std::streambuf* m_OldErr;
        };

        struct MinimumLevelRestore
        {
            ~MinimumLevelRestore() { Log::SetMinimumLevel(Level::Trace); }
        };

        struct Record
        {
            Level Severity = Level::Trace;
            std::string Message;

            bool operator==(const Record& other) const
            {
                return Severity == other.Severity && Message == other.Message;
            }
        };

        void LogAt(Level level, const std::string& text)
        {
            switch (level)
            {
                case Level::Trace: Log::Trace(text); break;
                case Level::Info: Log::Info(text); break;
                case Level::Warn: Log::Warn(text); break;
                case Level::Error: Log::Error(text); break;
            }
        }

        template<typename... Args>
        void LogAtVariadic(Level level, Args&&... args)
        {
            switch (level)
            {
                case Level::Trace: Log::Trace(std::forward<Args>(args)...); break;
                case Level::Info: Log::Info(std::forward<Args>(args)...); break;
                case Level::Warn: Log::Warn(std::forward<Args>(args)...); break;
                case Level::Error: Log::Error(std::forward<Args>(args)...); break;
            }
        }

        bool WaitFor(const std::atomic<bool>& flag, std::chrono::milliseconds limit = std::chrono::seconds(10))
        {
            const auto deadline = std::chrono::steady_clock::now() + limit;
            while (!flag.load())
            {
                if (std::chrono::steady_clock::now() > deadline)
                    return false;
                std::this_thread::yield();
            }
            return true;
        }

        bool RunProperty(std::string_view name, const Spiral::Tests::Property& property, size_t iterations)
        {
            Spiral::Tests::CampaignOptions options;
            options.Iterations = iterations;
            if (const char* seed = std::getenv("SPIRAL_LOG_SINK_SEED"))
                options.Seed = std::strtoull(seed, nullptr, 10);
            Spiral::Tests::ChoiceTrace replay;
            if (const char* trace = std::getenv("SPIRAL_LOG_SINK_REPLAY");
                trace && Spiral::Tests::ParseTrace(trace, replay))
                options.Replay = replay;

            Spiral::Tests::Counterexample failure;
            if (Spiral::Tests::RunCampaign(options, property, failure))
                return true;

            const std::string minimized = Spiral::Tests::SerializeTrace(failure.MinimizedTrace);
            const std::string rerun = "SPIRAL_LOG_SINK_SEED=" + std::to_string(failure.Seed)
                + " SPIRAL_LOG_SINK_REPLAY=\"" + minimized + "\" EngineTests --test <registered name of "
                + std::string(name) + ">";
            const std::filesystem::path artifact = std::filesystem::temp_directory_path()
                / "spiral-log-sink-counterexample.json";
            std::string artifactError;
            const bool written = Spiral::Tests::WriteCounterexample(artifact, name, failure, rerun, artifactError);
            std::cerr << "Log sink property failed [" << name << "]: " << failure.Message
                << " seed=" << failure.Seed << " iteration=" << failure.Iteration
                << " originalTrace=" << Spiral::Tests::SerializeTrace(failure.OriginalTrace)
                << " minimizedTrace=" << minimized << "\n  rerun: " << rerun << '\n';
            if (written)
                std::cerr << "  counterexample: " << artifact.string() << '\n';
            else
                std::cerr << "  counterexample write failed: " << artifactError << '\n';
            return false;
        }

        struct Operation
        {
            bool SetMinimum = false;
            Level Severity = Level::Info;
            std::string Text;
            bool HasNumber = false;
            long long Number = 0;
        };

        std::string RandomText(Spiral::Tests::ChoiceStream& choices)
        {
            static constexpr std::string_view alphabet = "abcXYZ 019_-[]%\t\xC3\xA9";
            std::string text;
            const size_t length = choices.NextSize(0, 40);
            for (size_t index = 0; index < length; ++index)
                text += alphabet[choices.NextSize(0, alphabet.size() - 1)];
            if (choices.NextSize(0, 15) == 0)
                text += "\nsecond line";
            return text;
        }

        // The oracle: what the console and a sink must observe, derived without calling Log.
        struct Expected
        {
            std::string Out;
            std::string Err;
            std::vector<Record> Delivered;
        };

        Expected Model(const std::vector<Operation>& operations)
        {
            Expected expected;
            Level minimum = Level::Trace;
            for (const Operation& operation : operations)
            {
                if (operation.SetMinimum)
                {
                    minimum = operation.Severity;
                    continue;
                }
                if (static_cast<int>(operation.Severity) < static_cast<int>(minimum))
                    continue;

                std::string message = operation.Text;
                if (operation.HasNumber)
                    message += "|" + std::to_string(operation.Number);
                const std::string line = "[" + std::string(kLevelNames[static_cast<int>(operation.Severity)])
                    + "] " + message + "\n";
                (operation.Severity == Level::Error ? expected.Err : expected.Out) += line;
                expected.Delivered.push_back({ operation.Severity, message });
            }
            return expected;
        }

        void Apply(const Operation& operation)
        {
            if (operation.SetMinimum)
                Log::SetMinimumLevel(operation.Severity);
            else if (operation.HasNumber)
                LogAtVariadic(operation.Severity, operation.Text, "|", operation.Number);
            else
                LogAt(operation.Severity, operation.Text);
        }
    }

    bool TestLogSinkDeliversFilteredLinesInOrderAndLeavesConsoleOutputUnchanged()
    {
        const Spiral::Tests::Property property = [](Spiral::Tests::ChoiceStream& choices, std::string& message)
        {
            std::vector<Operation> operations;
            const size_t count = choices.NextSize(0, 40);
            for (size_t index = 0; index < count; ++index)
            {
                Operation operation;
                operation.SetMinimum = choices.NextSize(0, 4) == 0;
                operation.Severity = static_cast<Level>(choices.NextSize(0, 3));
                operation.Text = RandomText(choices);
                operation.HasNumber = choices.NextBool();
                operation.Number = choices.NextI64(-1000, 1000, { -1, 0, 1 });
                operations.push_back(std::move(operation));
            }
            const Expected expected = Model(operations);

            MinimumLevelRestore restore;
            std::string withSinkOut;
            std::string withSinkErr;
            std::vector<Record> delivered;
            {
                ConsoleCapture capture;
                const Log::SinkId id = Log::AddSink([&](Level level, std::string_view line)
                {
                    delivered.push_back({ level, std::string(line) });
                });
                for (const Operation& operation : operations)
                    Apply(operation);
                const bool removed = Log::RemoveSink(id);
                withSinkOut = capture.Out();
                withSinkErr = capture.Err();
                if (!removed || id == Log::kInvalidSinkId)
                {
                    message = "sink registration did not round trip";
                    return false;
                }
            }
            Log::SetMinimumLevel(Level::Trace);

            std::string plainOut;
            std::string plainErr;
            {
                ConsoleCapture capture;
                for (const Operation& operation : operations)
                    Apply(operation);
                plainOut = capture.Out();
                plainErr = capture.Err();
            }

            if (withSinkOut != expected.Out || withSinkErr != expected.Err)
            {
                message = "console output differs from the independent model";
                return false;
            }
            if (plainOut != withSinkOut || plainErr != withSinkErr)
            {
                message = "registering a sink changed console output";
                return false;
            }
            if (!(delivered == expected.Delivered))
            {
                message = "sink observed lines differ from the filtered model";
                return false;
            }
            return true;
        };

        return RunProperty("Log sink delivers filtered lines in order and leaves console output unchanged",
            property, 200);
    }

    bool TestLogSinkRegistrationRemovalAndShutdownContract()
    {
        Checker check;
        MinimumLevelRestore restore;
        ConsoleCapture capture;

        check(Log::AddSink(Log::Sink {}) == Log::kInvalidSinkId, "an empty callback returns the invalid id");
        check(!Log::RemoveSink(Log::kInvalidSinkId), "the invalid id is never removable");
        check(!Log::RemoveSink(0xFFFFFFFFull), "an unknown id is not removable");

        std::vector<std::string> order;
        const Log::SinkId first = Log::AddSink([&](Level, std::string_view line) { order.push_back("A:" + std::string(line)); });
        const Log::SinkId second = Log::AddSink([&](Level, std::string_view line) { order.push_back("B:" + std::string(line)); });
        const Log::SinkId third = Log::AddSink([&](Level, std::string_view line) { order.push_back("C:" + std::string(line)); });
        check(first != Log::kInvalidSinkId && second != Log::kInvalidSinkId && third != Log::kInvalidSinkId
                && first != second && second != third && first != third,
            "registered sinks receive distinct nonzero ids");

        Log::Info("one");
        check((order == std::vector<std::string> { "A:one", "B:one", "C:one" }),
            "every sink sees each line, in registration order");

        order.clear();
        check(Log::RemoveSink(second), "an existing sink is removed");
        check(!Log::RemoveSink(second), "removal is not repeatable");
        Log::Warn("two");
        check((order == std::vector<std::string> { "A:two", "C:two" }),
            "removing the middle sink keeps the others in order and silences only it");

        order.clear();
        const Log::SinkId replacement = Log::AddSink([&](Level, std::string_view line) { order.push_back("D:" + std::string(line)); });
        check(replacement != Log::kInvalidSinkId && replacement != first && replacement != second && replacement != third,
            "ids are never reused after removal");
        Log::Error("three");
        check((order == std::vector<std::string> { "A:three", "C:three", "D:three" }),
            "a later registration is appended after earlier sinks");

        order.clear();
        Log::SetMinimumLevel(Level::Warn);
        Log::Info("filtered");
        Log::Warn("kept");
        check((order == std::vector<std::string> { "A:kept", "C:kept", "D:kept" }),
            "lines below the minimum level never reach a sink");
        Log::SetMinimumLevel(Level::Trace);

        order.clear();
        Log::Shutdown();
        Log::Info("after shutdown");
        check(order.empty(), "Shutdown removes every registered sink");
        check(!Log::RemoveSink(first) && !Log::RemoveSink(third) && !Log::RemoveSink(replacement),
            "ids removed by Shutdown are no longer valid");
        Log::Init();
        Log::Info("after reinit");
        check(order.empty(), "a sink registered before Shutdown does not survive Init");

        const Log::SinkId afterReinit = Log::AddSink([&](Level, std::string_view line) { order.push_back("E:" + std::string(line)); });
        check(afterReinit != Log::kInvalidSinkId, "registration works again after Init");
        Log::Info("fresh");
        check((order == std::vector<std::string> { "E:fresh" }), "the fresh sink receives lines");
        check(Log::RemoveSink(afterReinit), "the fresh sink is removable");

        return check.Passed;
    }

    bool TestLogSinkReentrancyAndThrowingSinksCannotBreakLogging()
    {
        Checker check;
        MinimumLevelRestore restore;
        ConsoleCapture capture;

        std::vector<Record> secondSeen;
        Log::SinkId lateId = Log::kInvalidSinkId;
        Log::SinkId reentrantRegistration = 12345;
        bool reentrantRemoval = true;
        int reentrantCalls = 0;
        const Log::SinkId reentrant = Log::AddSink([&](Level level, std::string_view line)
        {
            ++reentrantCalls;
            reentrantRegistration = Log::AddSink([](Level, std::string_view) {});
            reentrantRemoval = Log::RemoveSink(lateId);
            if (level == Level::Info)
                Log::Warn("nested from sink: ", line);
        });
        lateId = Log::AddSink([&](Level level, std::string_view line) { secondSeen.push_back({ level, std::string(line) }); });
        check(reentrant != Log::kInvalidSinkId && lateId != Log::kInvalidSinkId, "sinks registered");

        Log::Info("outer");
        check(reentrantCalls == 1, "a line logged from a sink is not delivered back to sinks");
        check(reentrantRegistration == Log::kInvalidSinkId, "AddSink from inside a sink is refused");
        check(!reentrantRemoval, "RemoveSink from inside a sink is refused");
        check((secondSeen == std::vector<Record> { { Level::Info, "outer" } }),
            "other sinks do not see the nested line either");
        check(capture.Out() == "[Info] outer\n[Warn] nested from sink: outer\n",
            "the nested line is still written to the console, after its parent");

        check(Log::RemoveSink(lateId), "refused in-sink removal left the registry intact");
        check(Log::RemoveSink(reentrant), "the reentrant sink is removable from outside");

        int goodCalls = 0;
        const Log::SinkId thrower = Log::AddSink([](Level, std::string_view) { throw std::runtime_error("sink failure"); });
        const Log::SinkId intThrower = Log::AddSink([](Level, std::string_view) { throw 7; });
        const Log::SinkId good = Log::AddSink([&](Level, std::string_view) { ++goodCalls; });
        bool escaped = false;
        try
        {
            Log::Info("one");
            Log::Error("two");
        }
        catch (...)
        {
            escaped = true;
        }
        check(!escaped, "a throwing sink never propagates into the logging call");
        check(goodCalls == 2, "sinks after a throwing sink still receive every line");
        const Log::SinkId afterThrow = Log::AddSink([](Level, std::string_view) {});
        check(afterThrow != Log::kInvalidSinkId, "a swallowed exception does not leave the thread marked as inside a sink");
        check(Log::RemoveSink(afterThrow) && Log::RemoveSink(thrower) && Log::RemoveSink(intThrower) && Log::RemoveSink(good),
            "all sinks removed");

        return check.Passed;
    }

    bool TestLogSinkConcurrentProducersChurnAndRemovalAreSerialized()
    {
        Checker check;
        MinimumLevelRestore restore;

        constexpr int kProducers = 4;
        constexpr int kMessages = 300;

        const auto parse = [](std::string_view line, int& producer, int& sequence)
        {
            // "t<producer> n<sequence>"
            if (line.size() < 5 || line[0] != 't')
                return false;
            const size_t space = line.find(" n");
            if (space == std::string_view::npos)
                return false;
            try
            {
                producer = std::stoi(std::string(line.substr(1, space - 1)));
                sequence = std::stoi(std::string(line.substr(space + 2)));
            }
            catch (...)
            {
                return false;
            }
            return producer >= 0 && producer < kProducers && sequence >= 0 && sequence < kMessages;
        };
        const auto levelFor = [](int sequence) { return static_cast<Level>(1 + sequence % 3); };

        // Phase 1: constant minimum level. A permanent sink must see every line exactly once and in
        // per-producer order; a churning sink must see one contiguous window of the global order and
        // never be called after RemoveSink returned.
        {
            ConsoleCapture capture;
            std::vector<Record> permanent;
            const Log::SinkId permanentId = Log::AddSink([&](Level level, std::string_view line)
            {
                permanent.push_back({ level, std::string(line) });
            });

            std::atomic<bool> producersDone { false };
            std::atomic<int> callsAfterRemoval { 0 };
            std::atomic<int> churnRounds { 0 };
            bool churnContiguous = true;
            bool churnOrdered = true;

            std::thread churn([&]
            {
                while (!producersDone.load())
                {
                    std::vector<Record> window;
                    std::atomic<bool> removed { false };
                    const Log::SinkId id = Log::AddSink([&](Level level, std::string_view line)
                    {
                        if (removed.load())
                            callsAfterRemoval.fetch_add(1);
                        window.push_back({ level, std::string(line) });
                    });
                    std::this_thread::yield();
                    Log::RemoveSink(id);
                    removed.store(true);

                    // Safe without a lock: RemoveSink synchronized with the last callback.
                    int lastSequence[kProducers];
                    for (int& value : lastSequence)
                        value = -1;
                    for (const Record& record : window)
                    {
                        int producer = 0;
                        int sequence = 0;
                        if (!parse(record.Message, producer, sequence))
                        {
                            churnOrdered = false;
                            continue;
                        }
                        if (lastSequence[producer] != -1 && sequence != lastSequence[producer] + 1)
                            churnContiguous = false;
                        if (lastSequence[producer] != -1 && sequence <= lastSequence[producer])
                            churnOrdered = false;
                        lastSequence[producer] = sequence;
                    }
                    churnRounds.fetch_add(1);
                }
            });

            std::vector<std::thread> producers;
            for (int producer = 0; producer < kProducers; ++producer)
            {
                producers.emplace_back([producer, &levelFor]
                {
                    for (int sequence = 0; sequence < kMessages; ++sequence)
                        LogAt(levelFor(sequence), "t" + std::to_string(producer) + " n" + std::to_string(sequence));
                });
            }
            for (std::thread& producer : producers)
                producer.join();
            producersDone.store(true);
            churn.join();
            check(Log::RemoveSink(permanentId), "permanent sink removed");

            check(permanent.size() == static_cast<size_t>(kProducers * kMessages), "the permanent sink saw every line exactly once");
            int next[kProducers] = {};
            bool permanentOrdered = true;
            for (const Record& record : permanent)
            {
                int producer = 0;
                int sequence = 0;
                if (!parse(record.Message, producer, sequence) || sequence != next[producer]
                    || record.Severity != levelFor(sequence))
                {
                    permanentOrdered = false;
                    break;
                }
                ++next[producer];
            }
            check(permanentOrdered, "per-producer order, content and level are preserved through the sink");
            check(callsAfterRemoval.load() == 0, "no sink callback ran after RemoveSink returned");
            check(churnOrdered, "churning sinks saw well formed, ordered lines");
            check(churnContiguous, "a churning sink saw a gap-free window of the serialized order");
            check(churnRounds.load() > 0, "the churn thread ran at least one registration cycle");

            // Console lines must be intact: no interleaved bytes, exact per-stream counts.
            const auto validateStream = [&](const std::string& text, bool errorStream)
            {
                int perProducer[kProducers] = {};
                size_t lines = 0;
                size_t offset = 0;
                bool intact = true;
                while (offset < text.size())
                {
                    const size_t end = text.find('\n', offset);
                    if (end == std::string::npos)
                    {
                        intact = false;
                        break;
                    }
                    const std::string_view line(text.data() + offset, end - offset);
                    offset = end + 1;
                    ++lines;
                    const size_t close = line.find("] ");
                    int producer = 0;
                    int sequence = 0;
                    if (line.empty() || line[0] != '[' || close == std::string_view::npos
                        || !parse(line.substr(close + 2), producer, sequence)
                        || line.substr(1, close - 1) != kLevelNames[static_cast<int>(levelFor(sequence))]
                        || (levelFor(sequence) == Level::Error) != errorStream
                        || sequence < perProducer[producer])
                    {
                        intact = false;
                        break;
                    }
                    perProducer[producer] = sequence;
                }
                size_t expected = 0;
                for (int sequence = 0; sequence < kMessages; ++sequence)
                    expected += (levelFor(sequence) == Level::Error) == errorStream ? 1u : 0u;
                return intact && lines == expected * kProducers;
            };
            check(validateStream(capture.Out(), false), "stdout lines are intact, complete and ordered per producer");
            check(validateStream(capture.Err(), true), "stderr lines are intact, complete and ordered per producer");
        }

        // Phase 2: the minimum level flips concurrently. Delivery may drop lines but must stay
        // well formed and per-producer ordered; the flip itself must not race with logging.
        {
            ConsoleCapture capture;
            std::vector<Record> seen;
            const Log::SinkId id = Log::AddSink([&](Level level, std::string_view line)
            {
                seen.push_back({ level, std::string(line) });
            });
            std::atomic<bool> producersDone { false };
            std::thread toggler([&]
            {
                bool strict = false;
                while (!producersDone.load())
                {
                    Log::SetMinimumLevel(strict ? Level::Warn : Level::Trace);
                    strict = !strict;
                    std::this_thread::yield();
                }
            });
            std::vector<std::thread> producers;
            for (int producer = 0; producer < kProducers; ++producer)
            {
                producers.emplace_back([producer, &levelFor]
                {
                    for (int sequence = 0; sequence < kMessages; ++sequence)
                        LogAt(levelFor(sequence), "t" + std::to_string(producer) + " n" + std::to_string(sequence));
                });
            }
            for (std::thread& producer : producers)
                producer.join();
            producersDone.store(true);
            toggler.join();
            check(Log::RemoveSink(id), "flip-phase sink removed");
            Log::SetMinimumLevel(Level::Trace);

            int last[kProducers];
            for (int& value : last)
                value = -1;
            bool wellFormed = true;
            for (const Record& record : seen)
            {
                int producer = 0;
                int sequence = 0;
                if (!parse(record.Message, producer, sequence) || sequence <= last[producer]
                    || record.Severity != levelFor(sequence))
                {
                    wellFormed = false;
                    break;
                }
                last[producer] = sequence;
            }
            check(wellFormed, "lines delivered while the minimum level flips are well formed and ordered");
            check(!seen.empty(), "some lines were delivered while the minimum level flipped");
        }

        // RemoveSink must wait for a callback that is already running.
        {
            ConsoleCapture capture;
            std::atomic<bool> entered { false };
            std::atomic<bool> release { false };
            std::atomic<bool> callbackFinished { false };
            std::atomic<bool> removeReturned { false };
            std::atomic<bool> removeSawFinishedCallback { false };
            const Log::SinkId id = Log::AddSink([&](Level, std::string_view)
            {
                entered.store(true);
                WaitFor(release);
                callbackFinished.store(true);
            });
            std::thread writer([] { Log::Info("blocked"); });
            check(WaitFor(entered), "the sink callback started");
            std::thread remover([&]
            {
                Log::RemoveSink(id);
                removeSawFinishedCallback.store(callbackFinished.load());
                removeReturned.store(true);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            check(!removeReturned.load(), "RemoveSink did not return while the callback was still running");
            release.store(true);
            writer.join();
            remover.join();
            check(removeReturned.load() && removeSawFinishedCallback.load(),
                "RemoveSink returned only after the running callback finished");
        }

        return check.Passed;
    }
}
