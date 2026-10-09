#pragma once

namespace SpiralTests
{
    bool TestFabZipStagingSinkWritesOwnerOnlyExactTrees();
    bool TestFabImportControllerImportsFolderZipAndGlbPackages();
    bool TestFabImportControllerClassifiesReimportReplacementAndConflict();
    bool TestFabImportControllerRejectsHostilePackagesWithoutChangingTheProject();
    bool TestFabImportControllerValidatesAndBindsProvenance();
    bool TestFabImportControllerCancelsAtEveryHookAndInjectsCommitFailures();
    bool TestFabImportControllerRejectsStaleBasesAndAssignmentMismatches();
    bool TestFabImportControllerStateMachineMatchesModel();
}
