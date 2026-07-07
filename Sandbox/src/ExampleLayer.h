#pragma once

#include <Engine.h>

class ExampleLayer final : public Engine::Layer
{
public:
    ExampleLayer();

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(Engine::Timestep timestep) override;

private:
    unsigned int m_FrameCounter = 0;
};
