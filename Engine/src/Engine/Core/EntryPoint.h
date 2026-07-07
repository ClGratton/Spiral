#pragma once

#include "Engine/Core/Application.h"
#include "Engine/Core/Log.h"
#include "Engine/Jobs/JobSystem.h"

#include <exception>

extern Engine::Application* Engine::CreateApplication(ApplicationCommandLineArgs args);

int main(int argc, char** argv)
{
    Engine::Log::Init();
    Engine::JobSystem::Get().Initialize();

    int exitCode = 0;
    Engine::Application* application = nullptr;

    try
    {
        application = Engine::CreateApplication({ argc, argv });
        application->Run();
    }
    catch (const std::exception& exception)
    {
        Engine::Log::Error("Unhandled exception: ", exception.what());
        exitCode = 1;
    }
    catch (...)
    {
        Engine::Log::Error("Unhandled unknown exception");
        exitCode = 1;
    }

    delete application;
    Engine::JobSystem::Get().Shutdown();
    Engine::Log::Shutdown();
    return exitCode;
}
