#pragma once

#include "Engine/Renderer/Renderer.h"

namespace Engine
{
    class RenderBackend
    {
    public:
        virtual ~RenderBackend() = default;

        virtual const char* GetName() const = 0;
        virtual bool Initialize() = 0;
        virtual void Shutdown() = 0;
        virtual void BeginFrame(const ClearColor& clearColor) = 0;
        virtual void EndFrame() = 0;
    };
}
