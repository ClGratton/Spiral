#pragma once

namespace SpiralTests
{
    // Tests for the pure Editor picking and framing math (Editor/src/Viewport/PickingMath).
    // EngineTests.cpp owns registration.
    bool TestPickingRayConventionsMatchRendererMatrices();
    bool TestPickingViewportMappingHandComputedAndRoundTrip();
    bool TestPickingOrientedBoxHandComputedCases();
    bool TestPickingOrientedBoxMatchesBruteForceAndContainsInteriorRays();
    bool TestPickingTriangleRefinementMatchesPlaneOracleAndBudgetPolicy();
    bool TestPickingNearestHitTieBreakConcaveMissAndSectorPrecision();
    bool TestPickingFramingFitsSphereAndKeepsViewDirection();
    bool TestPickingLatencyForLargeMeshes();
}
