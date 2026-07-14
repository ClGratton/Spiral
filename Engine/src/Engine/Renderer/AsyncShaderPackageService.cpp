#include "Engine/Renderer/AsyncShaderPackageService.h"

#include "Engine/Jobs/JobSystem.h"

#include <mutex>
#include <sstream>
#include <exception>
#include <unordered_map>

namespace Engine
{
    namespace
    {
        std::string TargetList(const PortableShaderRequest& request)
        {
            std::string result;
            for (PortableShaderTarget target : request.Targets)
            {
                if (!result.empty()) result += ',';
                result += target == PortableShaderTarget::Dxil ? "DXIL" : "SPIR-V";
            }
            return result;
        }

        std::string FailureMessage(const PortableShaderPackage& package)
        {
            if (package.Diagnostics.empty())
                return "compiler returned an incomplete shader package";

            std::ostringstream message;
            for (size_t index = 0; index < package.Diagnostics.size(); ++index)
            {
                if (index != 0) message << " | ";
                message << package.Diagnostics[index].Message;
            }
            return message.str();
        }
    }

    struct AsyncShaderPackageService::State
    {
        struct Record
        {
            ShaderPackageRequestResult Result;
        };

        mutable std::mutex Mutex;
        std::unordered_map<u64, Record> Records;
        std::unordered_map<std::string, u64> InFlight;
        std::unordered_map<std::string, std::shared_ptr<const PortableShaderPackage>> Completed;
        u64 NextRequestId = 1;
        bool Accepting = true;
    };

    AsyncShaderPackageService::AsyncShaderPackageService(CompileFunction compile, ShaderPackageExecutionMode executionMode)
        : m_State(std::make_shared<State>())
        , m_Compile(std::move(compile))
        , m_ExecutionMode(executionMode)
    {
    }

    AsyncShaderPackageService::~AsyncShaderPackageService()
    {
        Shutdown();
    }

    ShaderPackageRequestHandle AsyncShaderPackageService::Request(const PortableShaderRequest& request)
    {
        const std::string key = PortableShaderContract::CacheKey(request);
        ShaderPackageRequestHandle handle;
        {
            std::scoped_lock lock(m_State->Mutex);
            if (!m_State->Accepting || !m_Compile)
                return {};

            const auto inFlight = m_State->InFlight.find(key);
            if (inFlight != m_State->InFlight.end())
                return { inFlight->second, key };

            handle = { m_State->NextRequestId++, key };
            State::Record record;
            record.Result.Status = ShaderPackageRequestStatus::Pending;
            record.Result.Diagnostic = {
                handle.Id,
                ShaderPackageRequestStatus::Pending,
                request.SourceName,
                request.EntryPoint,
                TargetList(request),
                request.CompilerIdentity.empty() ? "Slang" : request.CompilerIdentity,
                key,
                "shader package compilation is pending"
            };

            const auto completed = m_State->Completed.find(key);
            if (completed != m_State->Completed.end())
            {
                record.Result.Status = ShaderPackageRequestStatus::CacheHit;
                record.Result.Package = completed->second;
                record.Result.Diagnostic.Status = ShaderPackageRequestStatus::CacheHit;
                record.Result.Diagnostic.Message = "validated shader package was reused from the service cache";
                m_State->Records.emplace(handle.Id, std::move(record));
                return handle;
            }

            m_State->Records.emplace(handle.Id, std::move(record));
            m_State->InFlight.emplace(key, handle.Id);
        }

        const std::shared_ptr<State> state = m_State;
        const CompileFunction compile = m_Compile;
        const auto work = [state, compile, request, handle]()
        {
            PortableShaderPackage compiled;
            compiled.Key = handle.Key;
            std::shared_ptr<const PortableShaderPackage> published;
            std::string terminalMessage;
            bool succeeded = false;
            try
            {
                compiled = compile(request);
                succeeded = compiled.Succeeded();
                std::string validationError;
                if (succeeded && compiled.Key != handle.Key)
                {
                    terminalMessage = "compiler returned a shader package with the wrong canonical key";
                    succeeded = false;
                }
                if (succeeded && compiled.Conventions != request.Conventions)
                {
                    terminalMessage = "compiler returned a shader package with mismatched backend conventions";
                    succeeded = false;
                }
                if (succeeded && !PortableShaderContract::Validate(
                        request,
                        compiled.Reflection,
                        compiled.VertexInputs,
                        validationError))
                {
                    terminalMessage = "compiler returned an invalid shader interface: " + validationError;
                    succeeded = false;
                }
                if (succeeded)
                {
                    published = std::make_shared<const PortableShaderPackage>(std::move(compiled));
                    terminalMessage = "validated shader package was published atomically";
                }
                else
                {
                    if (terminalMessage.empty())
                        terminalMessage = FailureMessage(compiled);
                }
            }
            catch (const std::exception& exception)
            {
                terminalMessage = std::string("shader compiler work threw an exception: ") + exception.what();
                succeeded = false;
            }
            catch (...)
            {
                terminalMessage = "shader compiler work threw an unknown exception";
                succeeded = false;
            }

            std::scoped_lock lock(state->Mutex);
            const auto record = state->Records.find(handle.Id);
            if (!state->Accepting || record == state->Records.end()
                || record->second.Result.Status != ShaderPackageRequestStatus::Pending)
                return;

            state->InFlight.erase(handle.Key);
            if (succeeded)
            {
                try
                {
                    state->Completed[handle.Key] = published;
                }
                catch (const std::exception& exception)
                {
                    terminalMessage = std::string("shader package publication threw an exception: ") + exception.what();
                    succeeded = false;
                }
                catch (...)
                {
                    terminalMessage = "shader package publication threw an unknown exception";
                    succeeded = false;
                }
            }
            record->second.Result.Status = succeeded
                ? ShaderPackageRequestStatus::Success
                : ShaderPackageRequestStatus::Failure;
            record->second.Result.Diagnostic.Status = record->second.Result.Status;
            if (succeeded)
            {
                record->second.Result.Package = published;
            }
            record->second.Result.Diagnostic.Message.swap(terminalMessage);
        };

        if (m_ExecutionMode == ShaderPackageExecutionMode::DeterministicInline)
        {
            work();
        }
        else if (JobSystem::Get().IsRunning())
        {
            JobSystem::Get().Submit(work, "PortableShaderCompile:" + request.EntryPoint);
        }
        else
        {
            std::scoped_lock lock(m_State->Mutex);
            m_State->InFlight.erase(key);
            State::Record& record = m_State->Records[handle.Id];
            record.Result.Status = ShaderPackageRequestStatus::Failure;
            record.Result.Diagnostic.Status = ShaderPackageRequestStatus::Failure;
            record.Result.Diagnostic.Message = "job system is not running; asynchronous shader compilation was not started";
        }
        return handle;
    }

    ShaderPackageRequestResult AsyncShaderPackageService::Poll(const ShaderPackageRequestHandle& handle) const
    {
        if (!handle.IsValid() || !m_State)
            return {};
        std::scoped_lock lock(m_State->Mutex);
        const auto record = m_State->Records.find(handle.Id);
        return record == m_State->Records.end() ? ShaderPackageRequestResult {} : record->second.Result;
    }

    void AsyncShaderPackageService::Shutdown()
    {
        if (!m_State)
            return;
        std::scoped_lock lock(m_State->Mutex);
        if (!m_State->Accepting)
            return;
        m_State->Accepting = false;
        m_State->InFlight.clear();
        for (auto& [id, record] : m_State->Records)
        {
            if (record.Result.Status != ShaderPackageRequestStatus::Pending)
                continue;
            record.Result.Status = ShaderPackageRequestStatus::Cancelled;
            record.Result.Diagnostic.Status = ShaderPackageRequestStatus::Cancelled;
            record.Result.Diagnostic.Message = "shader package request was cancelled during service shutdown";
        }
    }

    const char* AsyncShaderPackageService::ToString(ShaderPackageRequestStatus status)
    {
        switch (status)
        {
        case ShaderPackageRequestStatus::Unknown: return "unknown";
        case ShaderPackageRequestStatus::Pending: return "pending";
        case ShaderPackageRequestStatus::Success: return "success";
        case ShaderPackageRequestStatus::CacheHit: return "cache-hit";
        case ShaderPackageRequestStatus::Failure: return "failure";
        case ShaderPackageRequestStatus::Cancelled: return "cancelled";
        }
        return "unknown";
    }
}
