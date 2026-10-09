#include "SnapSettings.h"

#include <cmath>

namespace Gizmo
{
    namespace
    {
        bool InRange(double value, double minimum, double maximum)
        {
            return std::isfinite(value) && value >= minimum && value <= maximum;
        }
    }

    SnapSettingsError ValidateSnapSettings(const SnapSettings& settings)
    {
        if (!InRange(settings.TranslateStep, kMinimumTranslateStep, kMaximumTranslateStep))
            return SnapSettingsError::TranslateStep;
        if (!InRange(settings.RotateStepDegrees, kMinimumRotateStepDegrees, kMaximumRotateStepDegrees))
            return SnapSettingsError::RotateStep;
        if (!InRange(settings.ScaleStep, kMinimumScaleStep, kMaximumScaleStep))
            return SnapSettingsError::ScaleStep;
        return SnapSettingsError::None;
    }

    const char* DescribeSnapSettingsError(SnapSettingsError error)
    {
        switch (error)
        {
        case SnapSettingsError::None: return "valid";
        case SnapSettingsError::TranslateStep: return "translate step must be between 0.0001 and 10000";
        case SnapSettingsError::RotateStep: return "rotate step must be between 0.01 and 180 degrees";
        case SnapSettingsError::ScaleStep: return "scale step must be between 0.001 and 10";
        }
        return "unknown";
    }

    SnapSettings SanitizeSnapSettings(const SnapSettings& settings)
    {
        const SnapSettings defaults;
        SnapSettings result = settings;
        if (!InRange(result.TranslateStep, kMinimumTranslateStep, kMaximumTranslateStep))
            result.TranslateStep = defaults.TranslateStep;
        if (!InRange(result.RotateStepDegrees, kMinimumRotateStepDegrees, kMaximumRotateStepDegrees))
            result.RotateStepDegrees = defaults.RotateStepDegrees;
        if (!InRange(result.ScaleStep, kMinimumScaleStep, kMaximumScaleStep))
            result.ScaleStep = defaults.ScaleStep;
        return result;
    }

    bool IsSnapActive(const SnapSettings& settings, bool ctrlHeld)
    {
        return settings.Enabled != ctrlHeld;
    }
}
