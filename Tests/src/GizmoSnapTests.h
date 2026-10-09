#pragma once

namespace SpiralTests
{
    // Fast, pure-logic contract tests for the Editor gizmo snapping, rotation
    // and transform-application core. EngineTests.cpp owns registration.
    bool TestGizmoSnapSettingsValidationAndCtrlInversion();
    bool TestGizmoOracleInt128MatchesNativeArithmetic();
    bool TestGizmoSnapRoundingMatchesExactOracle();
    bool TestGizmoSnapTranslationMatchesInt128Oracle();
    bool TestGizmoSnapTranslationBoundariesAndFailureAtomicity();
    bool TestGizmoEulerRecompositionContinuityAndGimbal();
    bool TestGizmoApplyOperationsMatchOracles();
}
