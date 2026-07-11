#include "Engine/Jobs/JobSystem.h"

#include "Engine/Core/Log.h"

#include <algorithm>
#include <exception>

namespace Engine
{
    JobSystem& JobSystem::Get()
    {
        static JobSystem instance;
        return instance;
    }

    JobSystem::~JobSystem()
    {
        Shutdown();
    }

    void JobSystem::Initialize(u32 workerCount)
    {
        std::scoped_lock lock(m_Mutex);
        if (m_Running)
            return;

        const u32 hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
        if (workerCount == 0)
            workerCount = hardwareThreads > 1 ? hardwareThreads - 1 : 1;

        m_Running = true;
        m_AcceptingJobs = true;
        m_Workers.reserve(workerCount);

        for (u32 i = 0; i < workerCount; ++i)
            m_Workers.emplace_back([this, i]() { WorkerLoop(i); });

        Log::Info("Job system initialized with ", workerCount, " worker(s)");
    }

    void JobSystem::Shutdown()
    {
        {
            std::scoped_lock lock(m_Mutex);
            if (!m_Running)
                return;

            m_AcceptingJobs = false;
            m_Running = false;
        }

        m_WorkAvailable.notify_all();

        for (std::thread& worker : m_Workers)
        {
            if (worker.joinable())
                worker.join();
        }

        m_Workers.clear();

        std::scoped_lock lock(m_Mutex);
        while (!m_Jobs.empty())
            m_Jobs.pop();
        m_ActiveJobs = 0;
        Log::Info("Job system shutdown");
    }

    JobHandle JobSystem::Submit(std::function<void()> function, std::string name)
    {
        if (!function)
            return {};

        JobHandle handle;
        bool runInline = false;
        {
            std::scoped_lock lock(m_Mutex);
            if (!m_AcceptingJobs)
            {
                runInline = true;
            }
            else
            {
                Job job;
                job.Handle = { m_NextJobId++ };
                job.Function = std::move(function);
                job.Name = std::move(name);
                handle = job.Handle;
                m_Jobs.push(std::move(job));
            }
        }

        if (runInline)
            function();
        else
            m_WorkAvailable.notify_one();

        return handle;
    }

    void JobSystem::WaitIdle()
    {
        std::unique_lock lock(m_Mutex);
        m_Idle.wait(lock, [this]() { return m_Jobs.empty() && m_ActiveJobs == 0; });
    }

    bool JobSystem::IsRunning() const
    {
        std::scoped_lock lock(m_Mutex);
        return m_Running;
    }

    u32 JobSystem::GetWorkerCount() const
    {
        std::scoped_lock lock(m_Mutex);
        return static_cast<u32>(m_Workers.size());
    }

    void JobSystem::WorkerLoop(u32 workerIndex)
    {
        (void)workerIndex;

        while (true)
        {
            Job job;

            {
                std::unique_lock lock(m_Mutex);
                m_WorkAvailable.wait(lock, [this]() { return !m_Running || !m_Jobs.empty(); });

                if (!m_Running && m_Jobs.empty())
                    break;

                job = std::move(m_Jobs.front());
                m_Jobs.pop();
                ++m_ActiveJobs;
            }

            try
            {
                job.Function();
            }
            catch (const std::exception& exception)
            {
                Log::Error("Job '", job.Name.empty() ? "unnamed" : job.Name, "' failed: ", exception.what());
            }
            catch (...)
            {
                Log::Error("Job '", job.Name.empty() ? "unnamed" : job.Name, "' failed with an unknown exception");
            }

            {
                std::scoped_lock lock(m_Mutex);
                --m_ActiveJobs;
                if (m_Jobs.empty() && m_ActiveJobs == 0)
                    m_Idle.notify_all();
            }
        }
    }
}
