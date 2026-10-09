#pragma once

namespace SpiralTests
{
    // Fast, pure-math contract tests. EngineTests.cpp owns registration.
    bool TestMathRayPlaneAndLineHandCases();
    bool TestMathRayAabbHandCasesAndEdges();
    bool TestMathRayAabbMatchesTriangleOracle();
    bool TestMathRaySegmentDistanceMatchesOracle();
}
