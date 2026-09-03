#include "Engine/Renderer/Renderer.h"

namespace Engine
{
    namespace
    {
        RendererColorPipelineSettings s_ColorPipelineSettings;
    }

    bool Renderer::SetColorPipelineSettings(const RendererColorPipelineSettings& settings)
    {
        if (!IsValidRendererColorPipelineSettings(settings))
            return false;
        s_ColorPipelineSettings = settings;
        return true;
    }

    RendererColorPipelineSettings Renderer::GetColorPipelineSettings()
    {
        return s_ColorPipelineSettings;
    }
}
