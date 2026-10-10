#pragma once

namespace SpiralTests
{
    // Fast, pure-logic contract tests for the Editor GizmoSession: tool, space and
    // snap state, the registered commands, scripted pointer traces against a fake
    // host over a real Engine::Scene and a real HistoryStore, pointer ownership,
    // navigation yielding, far-sector precision and generated traces.
    // EngineTests.cpp owns registration.
    bool TestGizmoSessionStateSnapSettingsAndCommands();
    bool TestGizmoSessionTranslateIsStartPlusSnappedDelta();
    bool TestGizmoSessionLocalSpaceTranslateSnapsRelativeIncrements();
    bool TestGizmoSessionRotateAndScaleDragsMatchOracles();
    bool TestGizmoSessionOneHistoryEntryPerDragAndExactCancel();
    bool TestGizmoSessionPointerOwnershipAndHitPriority();
    bool TestGizmoSessionToolSwitchingAndDragLock();
    bool TestGizmoSessionYieldsToNavigationAndBlockedInput();
    bool TestGizmoSessionFarSectorPrecisionAndRollover();
    bool TestGizmoSessionDegenerateInputs();
    bool TestGizmoSessionDrawListUsesInjectedPalette();
    bool TestGizmoSessionGeneratedTracesKeepInvariants();
}
