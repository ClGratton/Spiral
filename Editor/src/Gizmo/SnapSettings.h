#pragma once

#include "GizmoTypes.h"

namespace Gizmo
{
    inline constexpr double kMinimumTranslateStep = 0.0001;
    inline constexpr double kMaximumTranslateStep = 10000.0;
    inline constexpr double kMinimumRotateStepDegrees = 0.01;
    inline constexpr double kMaximumRotateStepDegrees = 180.0;
    inline constexpr double kMinimumScaleStep = 0.001;
    inline constexpr double kMaximumScaleStep = 10.0;

    // Snapping is off by default. The steps are viewport-authoring state, not
    // project data.
    struct SnapSettings
    {
        bool Enabled = false;
        double TranslateStep = 1.0;
        double RotateStepDegrees = 15.0;
        double ScaleStep = 0.1;

        bool operator==(const SnapSettings&) const = default;
    };

    enum class SnapSettingsError : u8
    {
        None,
        TranslateStep,
        RotateStep,
        ScaleStep
    };

    // Reports the first step that is non-finite or outside its documented range.
    SnapSettingsError ValidateSnapSettings(const SnapSettings& settings);
    const char* DescribeSnapSettingsError(SnapSettingsError error);

    // Replaces each invalid step with its default (fail closed for persisted
    // or typed input) and keeps the toggle.
    SnapSettings SanitizeSnapSettings(const SnapSettings& settings);

    // Holding Ctrl while dragging inverts the toggle: on -> suspended, off -> enabled.
    bool IsSnapActive(const SnapSettings& settings, bool ctrlHeld);
}
