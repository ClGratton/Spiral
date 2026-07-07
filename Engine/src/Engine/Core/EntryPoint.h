#pragma once

#include "Engine/Core/Application.h"
#include "Engine/Core/Log.h"
#include "Engine/Diagnostics/CrashHandler.h"
#include "Engine/Jobs/JobSystem.h"

#include <exception>

extern Engine::Application* Engine::CreateApplication(ApplicationCommandLineArgs args);

int main(int argc, char** argv)
{
    Engine::Log::Init();
    Engine::CrashHandler::Install();
    Engine::JobSystem::Get().Initialize();

    int exitCode = 0;
    Engine::Application* application = nullptr;

    try
    {
        application = Engine::CreateApplication({ argc, argv });
        Engine::CrashHandler::SetApplicationName(application->GetSpecification().Name);
        application->Run();
    }
    catch (const std::exception& exception)
    {
        Engine::Log::Error("Unhandled exception: ", exception.what());
        const auto reportPath = Engine::CrashHandler::WriteReport("Unhandled C++ exception", exception.what());
        Engine::Log::Error("Crash report: ", reportPath.string());
        exitCode = 1;
    }
    catch (...)
    {
        Engine::Log::Error("Unhandled unknown exception");
        const auto reportPath = Engine::CrashHandler::WriteReport("Unhandled unknown exception");
        Engine::Log::Error("Crash report: ", reportPath.string());
        exitCode = 1;
    }

    delete application;
    Engine::JobSystem::Get().Shutdown();
    Engine::Log::Shutdown();
    return exitCode;
}
