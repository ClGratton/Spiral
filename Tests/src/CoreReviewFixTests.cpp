#include "CoreReviewFixTests.h"

#include "Engine/Core/Application.h"
#include "Engine/Core/AtomicFile.h"
#include "Engine/Core/Layer.h"
#include "Engine/Core/LayerStack.h"
#include "Engine/Diagnostics/Profiler.h"
#include "Engine/Jobs/FrameTaskGraph.h"
#include "Engine/Jobs/JobSystem.h"
#include "Engine/Platform/Headless/HeadlessWindow.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(GE_PLATFORM_LINUX) || defined(GE_PLATFORM_MACOS)
    #include <sys/stat.h>
#endif

namespace Engine
{
    // Friend of Application (see Application.h): the only way a test can drive
    // the private main loop of a headless Application.
    struct ApplicationTestAccess
    {
        static void Run(Application& application) { application.Run(); }
    };
}

namespace
{
    using namespace Engine;
    using Clock = std::chrono::steady_clock;

    bool Check(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "Core review-fix test failed: " << message << '\n';
        return condition;
    }

    // Records the phase order of every loop iteration as one letter each: U for
    // OnUpdate (a frame was rendered), I for OnUiRender, B for
    // OnBackgroundUpdate. It scripts the resize events the headless window
    // delivers inside the loop's own poll, which is where GLFW delivers them.
    class PhaseProbeLayer final : public Layer
    {
    public:
        explicit PhaseProbeLayer(std::chrono::milliseconds minimizedFor)
            : Layer("PhaseProbe"), m_MinimizedFor(minimizedFor)
        {
        }

        void OnUpdate(Timestep timestep) override
        {
            (void)timestep;
            Phases.push_back('U');
            if (m_UpdateCount++ == 0)
            {
                // Delivered by the poll inside the next iteration's rendered
                // branch: the window minimizes in the middle of a frame.
                Window().QueueResize(0, 0);
            }
            else if (m_RestoreQueued)
            {
                Application::Get().Close();
            }
        }

        void OnUiRender() override { Phases.push_back('I'); }

        void OnBackgroundUpdate() override
        {
            Phases.push_back('B');
            if (!m_MinimizedAt)
                m_MinimizedAt = Clock::now();
            if (!m_RestoreQueued && Clock::now() - *m_MinimizedAt >= m_MinimizedFor)
            {
                // Delivered by the wait inside the next minimized iteration: the
                // window is restored in the middle of an iteration that rendered
                // no frame.
                Window().QueueResize(1280, 720);
                m_RestoreQueued = true;
            }
        }

        std::string Phases;

    private:
        static HeadlessWindow& Window()
        {
            return static_cast<HeadlessWindow&>(Application::Get().GetWindow());
        }

        std::chrono::milliseconds m_MinimizedFor;
        std::optional<Clock::time_point> m_MinimizedAt;
        size_t m_UpdateCount = 0;
        bool m_RestoreQueued = false;
    };

    class HeadlessTestApplication final : public Application
    {
    public:
        explicit HeadlessTestApplication(ApplicationSpecification specification)
            : Application(std::move(specification))
        {
        }
    };

    ApplicationSpecification MakeHeadlessSpecification(std::string name)
    {
        ApplicationSpecification specification;
        specification.Name = std::move(name);
        specification.Window.Headless = true;
        // Zero means unbounded; the probe layers close the application.
        specification.MaxFrames = 0;
        return specification;
    }

    // The job system the suite shares: make sure it runs for the test and leave
    // it as it was found.
    class ScopedJobSystem
    {
    public:
        explicit ScopedJobSystem(u32 workers)
        {
            JobSystem& jobs = JobSystem::Get();
            m_WasRunning = jobs.IsRunning();
            if (!m_WasRunning)
                jobs.Initialize(workers);
        }

        ~ScopedJobSystem()
        {
            JobSystem& jobs = JobSystem::Get();
            if (m_WasRunning && !jobs.IsRunning())
                jobs.Initialize();
            else if (!m_WasRunning && jobs.IsRunning())
                jobs.Shutdown();
        }

    private:
        bool m_WasRunning = false;
    };

    class OnDetachRaisesFlagLayer final : public Layer
    {
    public:
        explicit OnDetachRaisesFlagLayer(std::shared_ptr<std::atomic<bool>> cancel)
            : Layer("OnDetachRaisesFlag"), m_Cancel(std::move(cancel))
        {
        }

        void OnDetach() override { m_Cancel->store(true, std::memory_order_release); }

    private:
        std::shared_ptr<std::atomic<bool>> m_Cancel;
    };

    class ThrowingAttachLayer final : public Layer
    {
    public:
        explicit ThrowingAttachLayer(int& detachCount)
            : Layer("ThrowingAttach"), m_DetachCount(detachCount)
        {
        }

        void OnAttach() override { throw std::runtime_error("attach failed"); }
        void OnDetach() override { ++m_DetachCount; }

    private:
        int& m_DetachCount;
    };

    class NamedLayer final : public Layer
    {
    public:
        explicit NamedLayer(std::string name)
            : Layer(std::move(name))
        {
        }
    };

    std::string LayerOrder(LayerStack& layers)
    {
        std::string order;
        for (const Scope<Layer>& layer : layers)
            order += layer->GetName() + ";";
        return order;
    }

    // Occupies workers until released, with a safety timeout so a failed
    // assertion cannot hang the suite.
    struct WorkerBlockers
    {
        std::atomic<bool> Release { false };
        std::atomic<u32> Started { 0 };
    };
}

namespace SpiralTests
{
    // Behavior: a minimized window runs no UI phase (layers' OnUiRender draws
    // ImGui and is only legal inside an open UI frame), runs the background hook
    // instead, makes one decision per iteration even when a resize is delivered
    // by the loop's own poll in either direction, and does not spin: the loop
    // waits for events with a bounded timeout.
    // Oracle: the exact per-iteration phase string of a scripted minimize and
    // restore, and a wall-clock bound on the iterations a 150 ms minimize takes.
    bool TestApplicationMinimizeLatchesOneUiDecisionAndBacksOff()
    {
        ScopedJobSystem jobs(1);
        auto application = std::make_unique<HeadlessTestApplication>(MakeHeadlessSpecification("MinimizeProbe"));
        PhaseProbeLayer* probe = static_cast<PhaseProbeLayer*>(
            application->PushLayer(CreateScope<PhaseProbeLayer>(std::chrono::milliseconds(150))));

        const auto start = Clock::now();
        ApplicationTestAccess::Run(*application);
        const double elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        const std::string phases = probe->Phases;
        application.reset();

        // Iteration 0 renders with its UI. Iteration 1 minimizes inside its own
        // poll: it rendered a frame but the window is no longer drawable, so the
        // UI phase must not run. Every minimized iteration is background only,
        // including the one whose wait delivers the restore (no frame was
        // rendered, so no UI frame). The next iteration renders and ends the run.
        const bool shape = phases.size() >= 8 && phases.rfind("UIUB", 0) == 0
            && phases.compare(phases.size() - 2, 2, "UI") == 0;
        bool middleIsBackground = shape;
        for (size_t index = 4; shape && index + 2 < phases.size(); ++index)
            middleIsBackground = middleIsBackground && phases[index] == 'B';
        const size_t backgroundIterations = static_cast<size_t>(std::count(phases.begin(), phases.end(), 'B'));
        const bool uiOnlyWhenRendered = std::count(phases.begin(), phases.end(), 'I') == 2
            && std::count(phases.begin(), phases.end(), 'U') == 3;

        return Check(shape && middleIsBackground,
                "minimized iterations run only the background hook (phases: " + phases + ")")
            && Check(uiOnlyWhenRendered, "OnUiRender runs only in the two iterations that rendered a UI frame")
            // A 150 ms minimize with a 16 ms bounded wait is about ten iterations.
            // A spinning loop runs millions; allow generous scheduling slack.
            && Check(backgroundIterations >= 3 && backgroundIterations <= 60,
                "the minimized loop waits instead of spinning (" + std::to_string(backgroundIterations)
                    + " iterations in " + std::to_string(elapsedMs) + " ms)")
            && Check(elapsedMs >= 100.0, "the minimized interval was actually waited out");
    }

    // Behavior: process teardown destroys the Application (detaching layers,
    // which raise their jobs' cancellation) before the job system joins its
    // workers, so a long cancellable job is cancelled instead of being awaited.
    // Oracle: a job that only finishes on cancel or after a long safety timeout;
    // teardown must return in a small fraction of that timeout and the job must
    // have observed the cancel.
    bool TestApplicationShutdownCancelsLayerJobsBeforeJoiningWorkers()
    {
        ScopedJobSystem jobs(2);
        auto application = std::make_unique<HeadlessTestApplication>(MakeHeadlessSpecification("ShutdownOrderProbe"));
        const auto cancel = std::make_shared<std::atomic<bool>>(false);
        application->PushLayer(CreateScope<OnDetachRaisesFlagLayer>(cancel));

        const auto started = std::make_shared<std::atomic<bool>>(false);
        const auto outcome = std::make_shared<std::atomic<int>>(0);
        JobSystem::Get().Submit([cancel, started, outcome]
        {
            started->store(true, std::memory_order_release);
            const auto deadline = Clock::now() + std::chrono::seconds(8);
            while (!cancel->load(std::memory_order_acquire) && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            outcome->store(cancel->load(std::memory_order_acquire) ? 1 : 2, std::memory_order_release);
        }, "CancellableLongJob");
        for (int attempt = 0; attempt < 2000 && !started->load(std::memory_order_acquire); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!Check(started->load(), "the long job started"))
        {
            cancel->store(true);
            return false;
        }

        const auto begin = Clock::now();
        DestroyApplicationThenShutdownJobs(application.release());
        const double elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();

        return Check(outcome->load() == 1, "the job observed the layer's cancellation instead of timing out")
            && Check(elapsedMs < 3000.0, "teardown did not wait for the uncancelled job (" + std::to_string(elapsedMs) + " ms)")
            && Check(!JobSystem::Get().IsRunning(), "the job system is shut down after teardown");
    }

    // Behavior: a layer whose OnAttach throws is not registered and is never
    // detached; the stack keeps its other layers and its layer/overlay split.
    // Oracle: detach counter, layer count and iteration order.
    bool TestLayerStackRollsBackALayerWhoseAttachThrows()
    {
        int detachCount = 0;
        LayerStack layers;
        layers.PushLayer(CreateScope<NamedLayer>("A"));
        layers.PushOverlay(CreateScope<NamedLayer>("Overlay"));

        bool threwLayer = false;
        try
        {
            layers.PushLayer(CreateScope<ThrowingAttachLayer>(detachCount));
        }
        catch (const std::runtime_error&)
        {
            threwLayer = true;
        }
        bool threwOverlay = false;
        try
        {
            layers.PushOverlay(CreateScope<ThrowingAttachLayer>(detachCount));
        }
        catch (const std::runtime_error&)
        {
            threwOverlay = true;
        }
        // The insertion index must have been restored: a new layer still lands
        // before the overlay.
        layers.PushLayer(CreateScope<NamedLayer>("B"));
        const std::string order = LayerOrder(layers);
        layers.Clear();

        return Check(threwLayer && threwOverlay, "OnAttach failures propagate")
            && Check(detachCount == 0, "a layer whose attach failed is never detached")
            && Check(order == "A;B;Overlay;", "the stack keeps its layers and layer/overlay boundary (" + order + ")");
    }

    // Behavior (POSIX): replacing a file keeps its permission bits, a new file is
    // private, a symlinked destination directory is followed, and a very long
    // target name still gets a temporary name inside NAME_MAX.
    bool TestAtomicFilePreservesModeFollowsSymlinkedParentAndBoundsTemporaryNames()
    {
#if defined(GE_PLATFORM_LINUX) || defined(GE_PLATFORM_MACOS)
        std::error_code error;
        const std::filesystem::path root = std::filesystem::temp_directory_path()
            / ("CoreReviewFixAtomic-" + std::to_string(static_cast<u64>(Clock::now().time_since_epoch().count())));
        std::filesystem::create_directories(root / "real", error);
        if (!Check(!error, "fixture directory"))
            return false;

        const auto readFile = [](const std::filesystem::path& path)
        {
            std::ifstream input(path, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        };
        const auto modeOf = [](const std::filesystem::path& path)
        {
            struct stat info {};
            return ::stat(path.c_str(), &info) == 0 ? static_cast<int>(info.st_mode & 07777) : -1;
        };

        bool ok = true;
        std::string writeError;

        // Mode preservation.
        const std::filesystem::path shared = root / "shared.txt";
        { std::ofstream(shared) << "old"; }
        ::chmod(shared.c_str(), 0664);
        ok &= Check(WriteFileAtomically(shared, "new contents", writeError), "replace an existing file: " + writeError);
        ok &= Check(readFile(shared) == "new contents", "the replacement content is published");
        ok &= Check(modeOf(shared) == 0664, "the replacement keeps the target's mode (got " + std::to_string(modeOf(shared)) + ")");

        const std::filesystem::path fresh = root / "fresh.txt";
        ok &= Check(WriteFileAtomically(fresh, "x", writeError), "create a new file: " + writeError);
        ok &= Check(modeOf(fresh) == 0600, "a new file is owner-only");

        // Symlinked destination directory.
        std::filesystem::create_directory_symlink(root / "real", root / "link", error);
        ok &= Check(!error, "fixture symlink");
        ok &= Check(WriteFileAtomically(root / "link" / "through.txt", "linked", writeError),
            "a symlinked destination directory is followed: " + writeError);
        ok &= Check(readFile(root / "real" / "through.txt") == "linked", "the file lands in the link target");

        // Long target name: the temporary name must stay under NAME_MAX.
        const std::filesystem::path longName = root / (std::string(240, 'n') + ".txt");
        ok &= Check(WriteFileAtomically(longName, "long", writeError), "a 244-byte file name is writable: " + writeError);
        ok &= Check(readFile(longName) == "long", "the long-named file has its content");

        size_t leftovers = 0;
        for (const auto& entry : std::filesystem::directory_iterator(root))
        {
            const std::string name = entry.path().filename().string();
            leftovers += name.find(".tmp.") != std::string::npos ? 1 : 0;
        }
        ok &= Check(leftovers == 0, "no temporary file is left behind");

        std::filesystem::remove_all(root, error);
        return ok;
#else
        return true;
#endif
    }

    // Behavior: a worker-lane frame task completes even when every worker is
    // occupied by long background jobs (the calling thread runs it), and the
    // task still runs on a worker when workers are free.
    // Oracle: blockers that only end on release or a 5 s safety timeout versus
    // a sub-second bound on Execute; the profile hook reports which thread ran
    // the task.
    bool TestFrameTaskGraphRunsWorkerTasksWhenEveryWorkerIsBusy()
    {
        ScopedJobSystem jobs(2);
        JobSystem& system = JobSystem::Get();
        const u32 workers = system.GetWorkerCount();
        auto blockers = std::make_shared<WorkerBlockers>();
        for (u32 index = 0; index < workers; ++index)
        {
            system.Submit([blockers]
            {
                blockers->Started.fetch_add(1, std::memory_order_acq_rel);
                const auto deadline = Clock::now() + std::chrono::seconds(5);
                while (!blockers->Release.load(std::memory_order_acquire) && Clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }, "Blocker");
        }
        for (int attempt = 0; attempt < 3000 && blockers->Started.load(std::memory_order_acquire) < workers; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!Check(blockers->Started.load() == workers, "every worker is occupied"))
        {
            blockers->Release.store(true);
            system.WaitIdle();
            return false;
        }

        const std::thread::id caller = std::this_thread::get_id();
        std::atomic<bool> workerTaskRan { false };
        std::atomic<bool> workerTaskOnCaller { false };
        FrameTaskGraph busyGraph;
        FrameTaskDescription workerTask;
        workerTask.Name = "BusyWorkerTask";
        workerTask.Lane = FrameTaskLane::Worker;
        workerTask.Execute = [&]
        {
            workerTaskOnCaller.store(std::this_thread::get_id() == caller);
            workerTaskRan.store(true);
        };
        const FrameTaskId first = busyGraph.AddTask(std::move(workerTask));
        FrameTaskDescription followUp;
        followUp.Name = "AfterWorkerTask";
        followUp.Lane = FrameTaskLane::CallingThread;
        followUp.Dependencies = { first };
        followUp.Execute = [] {};
        busyGraph.AddTask(std::move(followUp));

        const auto begin = Clock::now();
        const FrameTaskGraphResult busyResult = busyGraph.Execute(system);
        const double busyMs = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();

        blockers->Release.store(true, std::memory_order_release);
        system.WaitIdle();

        bool ok = Check(busyResult.Succeeded() && workerTaskRan.load(), "the graph completes while every worker is busy")
            && Check(busyMs < 1500.0, "Execute did not queue behind the blockers (" + std::to_string(busyMs) + " ms)")
            && Check(workerTaskOnCaller.load(), "the starved worker task ran on the calling thread");

        // With free workers the task still goes to a worker.
        size_t onWorker = 0;
        constexpr size_t iterations = 20;
        for (size_t iteration = 0; iteration < iterations; ++iteration)
        {
            FrameTaskGraph freeGraph;
            FrameTaskDescription task;
            task.Name = "FreeWorkerTask";
            task.Lane = FrameTaskLane::Worker;
            task.Execute = [] {};
            freeGraph.AddTask(std::move(task));
            FrameTaskExecutionOptions options;
            options.ProfileHook = [&](const FrameTaskProfileEvent& event)
            {
                if (event.Phase == FrameTaskProfilePhase::End && event.WorkerIndex != kInvalidJobWorkerIndex)
                    ++onWorker;
            };
            ok &= Check(freeGraph.Execute(system, options).Succeeded(), "free-worker graph succeeds");
        }
        ok &= Check(onWorker == iterations, "with idle workers every worker-lane task runs on a worker ("
            + std::to_string(onWorker) + "/" + std::to_string(iterations) + ")");
        return ok;
    }

    // Behavior: WaitIdle returns only after the job and everything it captured
    // has been destroyed.
    // Oracle: a capture whose destructor takes 30 ms and then raises a flag.
    bool TestJobSystemWaitIdleOutlastsJobCaptureDestruction()
    {
        struct SlowDestroy
        {
            explicit SlowDestroy(std::atomic<bool>& flag) : Flag(flag) {}
            ~SlowDestroy()
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                Flag.store(true, std::memory_order_release);
            }
            std::atomic<bool>& Flag;
        };

        ScopedJobSystem jobs(1);
        bool ok = true;
        for (int round = 0; round < 3; ++round)
        {
            std::atomic<bool> destroyed { false };
            std::atomic<bool> ran { false };
            {
                auto capture = std::make_shared<SlowDestroy>(destroyed);
                JobSystem::Get().Submit([capture, &ran] { ran.store(true); }, "CaptureDestruction");
            }
            JobSystem::Get().WaitIdle();
            ok &= Check(ran.load() && destroyed.load(std::memory_order_acquire),
                "WaitIdle returned before the job's captured state was destroyed");
        }
        return ok;
    }

    // Behavior: several profile scopes (and a function scope) can share one
    // block, and the scope name may be a temporary string. Pre-fix this did not
    // compile because ## pasted the token __LINE__ instead of its value.
    bool TestProfileScopesCanShareABlock()
    {
        GE_PROFILE_SCOPE("first");
        GE_PROFILE_SCOPE(std::string("Job:") + "second");
        GE_PROFILE_FUNCTION();
        return true;
    }
}
