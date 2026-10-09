#pragma once

namespace SpiralTests
{
    bool TestUiTextureServiceHandleLifecycleAndLimits();
    bool TestUiTextureServiceDirtyRectContentMatchesIndependentOracle();
    bool TestUiTextureServiceDeferredRetirementWaitsForWriteTokensAndPresentation();
    bool TestUiTextureServiceFailuresAreAtomicAndNeverRegisterPartialTextures();
    bool TestUiTextureServiceShutdownDrainThreadRulesAndNoStall();
}
