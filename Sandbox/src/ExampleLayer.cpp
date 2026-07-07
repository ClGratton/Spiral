#include "ExampleLayer.h"

ExampleLayer::ExampleLayer()
    : Engine::Layer("ExampleLayer")
{
}

void ExampleLayer::OnAttach()
{
    Engine::Log::Info("Sandbox example layer attached");
}

void ExampleLayer::OnDetach()
{
    Engine::Log::Info("Sandbox example layer detached");
}

void ExampleLayer::OnUpdate(Engine::Timestep timestep)
{
    ++m_FrameCounter;
    Engine::Log::Info("Sandbox frame ", m_FrameCounter, " dt=", timestep.GetMilliseconds(), "ms");
}
