#pragma once

namespace SpiralTests
{
    // Fast, pure tests for detachable OS-window panels. EngineTests.cpp owns
    // registration. Nothing here opens a window or touches a GPU; the native
    // behaviour is covered by the --ui-viewport-smoke marker in Scripts/TestVulkan.sh.
    bool TestUiViewportPresentModeSelection();
    bool TestUiViewportCapabilityDecision();
    bool TestPresentationSerialLedgerHandCases();
    bool TestPresentationSerialLedgerMatchesReferenceModel();
}
