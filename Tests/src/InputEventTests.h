#pragma once

namespace SpiralTests
{
    // Fast, GLFW-free contract tests. EngineTests.cpp owns registration.
    bool TestInputModifierTranslation();
    bool TestInputEventTranslationAndDispatch();
    bool TestInputEventConsumerCompatibility();
}
