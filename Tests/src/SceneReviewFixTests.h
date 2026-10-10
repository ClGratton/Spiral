#pragma once

namespace SpiralTests
{
    bool TestSceneLoadRejectsUnrenderableTransformsCamerasAndWrappedIds();
    bool TestSceneSaveRefusesEverythingTheLoaderRejectsAndKeepsTheLastGoodFile();
    bool TestCameraProjectionValidationReachesEveryProducer();
    bool TestSceneMainCameraAbsenceSurvivesSaveAndReload();
    bool TestSceneEntityIdCounterSaturatesInsteadOfWrapping();
    bool TestSceneLoadAndLookupScaleLinearly();
    bool TestSceneGeneratedPopulationsSaveOnlyWhatLoadsBack();
}
