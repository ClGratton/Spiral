#pragma once

namespace SpiralTests
{
    // Fast contract tests for the Engine::Log sink hook. EngineTests.cpp owns registration.
    bool TestLogSinkDeliversFilteredLinesInOrderAndLeavesConsoleOutputUnchanged();
    bool TestLogSinkRegistrationRemovalAndShutdownContract();
    bool TestLogSinkReentrancyAndThrowingSinksCannotBreakLogging();
    bool TestLogSinkConcurrentProducersChurnAndRemovalAreSerialized();
}
