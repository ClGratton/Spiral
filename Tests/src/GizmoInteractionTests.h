#pragma once

namespace SpiralTests
{
    // Fast, pure-logic contract tests for the Editor gizmo projection, handle
    // picking, drag solvers and interaction state machine. EngineTests.cpp
    // owns registration.
    bool TestGizmoProjectionRoundTripsAndConstantSize();
    bool TestGizmoViewValidationAndAtomicity();
    bool TestGizmoHandleGeometryAndHitTesting();
    bool TestGizmoSolversRecoverTrueDisplacements();
    bool TestGizmoDragRoundTripsThroughApply();
    bool TestGizmoDrawListContracts();
    bool TestGizmoStateMachineScriptedGestures();
    bool TestGizmoStateMachineModelProperty();
}
