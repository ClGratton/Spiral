#pragma once

namespace SpiralTests
{
    bool TestShortcutKeyTableMatchesGlfwAndChordsRoundTrip();
    bool TestShortcutChordParsingRejectsMalformedText();
    bool TestShortcutMapMatchesBruteForceOracle();
    bool TestShortcutMapRebindIsAtomic();
    bool TestShortcutCodecRoundTripsAndFailsClosed();
    bool TestCommandRegistryRegistrationRules();
    bool TestCommandRegistryDispatchMatchesModel();
    bool TestCommandRegistryReentrancyAndSources();
    bool TestCommandRegistryTypedArguments();
    bool TestFuzzyMatchGoldenCases();
    bool TestFuzzyMatchAgreesWithBruteForceOracle();
    bool TestFuzzyRankProperties();
    bool TestLogBufferRingBoundaries();
    bool TestLogBufferMatchesNaiveModel();
    bool TestLogBufferConcurrentProducers();
    bool TestTextTruncationNeverSplitsUtf8();
    bool TestNotificationsExpiryUsesInjectedClock();
    bool TestNotificationsMatchModel();
}
