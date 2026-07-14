#pragma once

#include "Engine/Renderer/PortableShaderContract.h"

#include <functional>
#include <memory>
#include <string>

namespace Engine
{
    enum class ShaderPackageExecutionMode
    {
        JobSystem,
        DeterministicInline
    };

    enum class ShaderPackageRequestStatus
    {
        Unknown,
        Pending,
        Success,
        CacheHit,
        Failure,
        Cancelled
    };

    struct ShaderPackageRequestHandle
    {
        u64 Id = 0;
        std::string Key;

        bool IsValid() const { return Id != 0 && !Key.empty(); }
    };

    struct ShaderPackageRequestDiagnostic
    {
        u64 RequestId = 0;
        ShaderPackageRequestStatus Status = ShaderPackageRequestStatus::Unknown;
        std::string Source;
        std::string EntryPoint;
        std::string Targets;
        std::string Backend;
        std::string Key;
        std::string Message;
    };

    struct ShaderPackageRequestResult
    {
        ShaderPackageRequestStatus Status = ShaderPackageRequestStatus::Unknown;
        std::shared_ptr<const PortableShaderPackage> Package;
        ShaderPackageRequestDiagnostic Diagnostic;

        bool IsTerminal() const
        {
            return Status != ShaderPackageRequestStatus::Unknown && Status != ShaderPackageRequestStatus::Pending;
        }

        bool Succeeded() const
        {
            return (Status == ShaderPackageRequestStatus::Success || Status == ShaderPackageRequestStatus::CacheHit)
                && Package && Package->Succeeded();
        }
    };

    class AsyncShaderPackageService
    {
    public:
        using CompileFunction = std::function<PortableShaderPackage(const PortableShaderRequest&)>;

        explicit AsyncShaderPackageService(
            CompileFunction compile,
            ShaderPackageExecutionMode executionMode = ShaderPackageExecutionMode::JobSystem);
        ~AsyncShaderPackageService();

        ShaderPackageRequestHandle Request(const PortableShaderRequest& request);
        ShaderPackageRequestResult Poll(const ShaderPackageRequestHandle& handle) const;
        void Shutdown();

        static const char* ToString(ShaderPackageRequestStatus status);

    private:
        struct State;
        std::shared_ptr<State> m_State;
        CompileFunction m_Compile;
        ShaderPackageExecutionMode m_ExecutionMode = ShaderPackageExecutionMode::JobSystem;
    };
}
