#include "Engine/Jobs/JobSystem.h"

#include "Engine/Core/Log.h"

#include <algorithm>
#include <exception>
#include <stdexcept>

namespace Engine
{
    namespace
    {
        thread_local JobSystem* t_WorkerJobSystem = nullptr;
        thread_local u32 t_WorkerIndex = kInvalidJobWorkerIndex;
    }

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
        std::scoped_lock lifecycleLock(m_LifecycleMutex);
        std::unique_lock lock(m_Mutex);
        if (m_Running)
            return;

        const u32 hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
        if (workerCount == 0)
            workerCount = hardwareThreads > 1 ? hardwareThreads - 1 : 1;

        try
        {
            m_Running = true;
            m_AcceptingJobs = true;
            m_Statistics = {};
            m_Workers.reserve(workerCount);

            for (u32 i = 0; i < workerCount; ++i)
                m_Workers.push_back(std::make_unique<Worker>());
            for (u32 i = 0; i < workerCount; ++i)
                m_Workers[i]->Thread = std::thread([this, i]() { WorkerLoop(i); });
        }
        catch (...)
        {
            m_AcceptingJobs = false;
            m_Running = false;
            lock.unlock();
            m_WorkAvailable.notify_all();
            for (const std::unique_ptr<Worker>& worker : m_Workers)
            {
                if (worker->Thread.joinable())
                    worker->Thread.join();
            }
            lock.lock();
            m_Workers.clear();
            m_QueuedJobs = 0;
            m_ActiveJobs = 0;
            throw;
        }

        lock.unlock();
        Log::Info("Job system initialized with ", workerCount, " worker(s)");
    }

    void JobSystem::Shutdown()
    {
        if (GetCurrentWorkerIndex() != kInvalidJobWorkerIndex)
            throw std::logic_error("JobSystem::Shutdown cannot be called from a worker thread");

        std::scoped_lock lifecycleLock(m_LifecycleMutex);
        {
            std::scoped_lock lock(m_Mutex);
            if (!m_Running)
                return;

            m_AcceptingJobs = false;
            m_Running = false;
        }

        m_WorkAvailable.notify_all();

        for (const std::unique_ptr<Worker>& worker : m_Workers)
        {
            if (worker->Thread.joinable())
                worker->Thread.join();
        }

        {
            std::scoped_lock lock(m_Mutex);
            while (!m_InjectedJobs.empty())
                m_InjectedJobs.pop();
            m_QueuedJobs = 0;
            m_ActiveJobs = 0;
            m_Workers.clear();
        }
        Log::Info("Job system shutdown");
    }

    JobHandle JobSystem::Submit(std::function<void()> function, std::string name)
    {
        if (!function)
            return {};

        JobHandle handle;
        bool runInline = false;
        u32 localWorker = kInvalidJobWorkerIndex;
        Job job;
        {
            std::scoped_lock lock(m_Mutex);
            if (!m_AcceptingJobs)
            {
                runInline = true;
            }
            else
            {
                job.Handle = { m_NextJobId++ };
                job.Function = std::move(function);
                job.Name = std::move(name);
                handle = job.Handle;
                if (t_WorkerJobSystem == this)
                    localWorker = t_WorkerIndex;
                else
                {
                    m_InjectedJobs.push(std::move(job));
                    ++m_QueuedJobs;
                    ++m_Statistics.SubmittedJobs;
                }
            }
        }

        if (runInline)
            function();
        else
        {
            if (localWorker != kInvalidJobWorkerIndex)
            {
                Worker& worker = *m_Workers[localWorker];
                std::unique_lock workerLock(worker.Mutex);
                worker.Jobs.push_back(std::move(job));
                std::scoped_lock lock(m_Mutex);
                ++m_QueuedJobs;
                ++m_Statistics.SubmittedJobs;
            }
            m_WorkAvailable.notify_one();
        }

        return handle;
    }

    void JobSystem::WaitIdle()
    {
        if (GetCurrentWorkerIndex() != kInvalidJobWorkerIndex)
            throw std::logic_error("JobSystem::WaitIdle cannot be called from a worker thread");

        std::unique_lock lock(m_Mutex);
        m_Idle.wait(lock, [this]() { return m_QueuedJobs == 0 && m_ActiveJobs == 0; });
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

    u32 JobSystem::GetCurrentWorkerIndex() const
    {
        return t_WorkerJobSystem == this ? t_WorkerIndex : kInvalidJobWorkerIndex;
    }

    JobSystemStatistics JobSystem::GetStatistics() const
    {
        std::scoped_lock lock(m_Mutex);
        return m_Statistics;
    }

    bool JobSystem::TryAcquireJob(u32 workerIndex, Job& outJob)
    {
        {
            Worker& worker = *m_Workers[workerIndex];
            std::unique_lock workerLock(worker.Mutex);
            if (!worker.Jobs.empty())
            {
                outJob = std::move(worker.Jobs.back());
                worker.Jobs.pop_back();
                std::scoped_lock lock(m_Mutex);
                --m_QueuedJobs;
                ++m_ActiveJobs;
                return true;
            }
        }

        {
            std::scoped_lock lock(m_Mutex);
            if (!m_InjectedJobs.empty())
            {
                outJob = std::move(m_InjectedJobs.front());
                m_InjectedJobs.pop();
                --m_QueuedJobs;
                ++m_ActiveJobs;
                return true;
            }
        }

        for (u32 offset = 1; offset < m_Workers.size(); ++offset)
        {
            const u32 victimIndex = (workerIndex + offset) % static_cast<u32>(m_Workers.size());
            Worker& victim = *m_Workers[victimIndex];
            std::unique_lock victimLock(victim.Mutex);
            if (victim.Jobs.empty())
                continue;

            outJob = std::move(victim.Jobs.front());
            victim.Jobs.pop_front();
            std::scoped_lock lock(m_Mutex);
            --m_QueuedJobs;
            ++m_ActiveJobs;
            ++m_Statistics.StolenJobs;
            return true;
        }

        return false;
    }

    void JobSystem::WorkerLoop(u32 workerIndex)
    {
        t_WorkerJobSystem = this;
        t_WorkerIndex = workerIndex;

        while (true)
        {
            Job job;
            if (!TryAcquireJob(workerIndex, job))
            {
                std::unique_lock lock(m_Mutex);
                m_WorkAvailable.wait(lock, [this]() { return !m_Running || m_QueuedJobs != 0; });

                if (!m_Running && m_QueuedJobs == 0)
                    break;
                continue;
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
                ++m_Statistics.CompletedJobs;
                if (m_QueuedJobs == 0 && m_ActiveJobs == 0)
                    m_Idle.notify_all();
            }
        }

        t_WorkerJobSystem = nullptr;
        t_WorkerIndex = kInvalidJobWorkerIndex;
    }
}
