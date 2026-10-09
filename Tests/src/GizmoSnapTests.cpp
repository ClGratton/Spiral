#include "GizmoSnapTests.h"

#include "TestSupport/Int128.h"
#include "TestSupport/PropertyRunner.h"

#include "EulerRotation.h"
#include "GizmoApply.h"
#include "SnapMath.h"
#include "SnapSettings.h"

#include "Engine/Math/DVec3Ops.h"
#include "Engine/Math/Math.h"
#include "Engine/Math/WorldGrid.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace SpiralTests
{
    namespace
    {
        using namespace Gizmo;
        using Engine::i64;
        using Engine::Math::DVec3;
        using Engine::Math::SectorLocalPosition;
        using Engine::Math::Vec3;
        using Engine::Math::WorldGridPolicy;
        using Spiral::Tests::FloorDiv;
        using Spiral::Tests::Int128;
        using i128 = Int128;

        // Failure hypotheses, oracles, and non-claims for the whole file:
        // - Position snapping is checked against an exact __int128 model: a
        //   step is decomposed into S * 2^-Q, a local into n * 2^-20, the
        //   sector extent into an integer, and the lattice index, tie rule,
        //   sector carry and clamp are decided in integers, never through
        //   floating division. The model implements the documented rule set
        //   independently (aligned lattices carry at +E/2, others clamp).
        // - Euler extraction is checked by recomposing with the engine's own
        //   float Math::RotationYawPitchRoll and comparing matrices, never Euler
        //   triples; rotation application against a Rodrigues vector model.
        // - Properties run 500 iterations from a replayable seed
        //   (SPIRAL_GIZMO_SNAP_SEED / SPIRAL_GIZMO_SNAP_REPLAY).
        // - Tier: fast, in-process. Not claimed: ImGui, input delivery, a
        //   renderer, or snapping to mesh surfaces, vertices or bounds.

        struct Checker
        {
            const char* Suite;
            bool Ok = true;

            void Expect(bool condition, const std::string& message)
            {
                if (!condition)
                {
                    std::cerr << "Gizmo snap test failed [" << Suite << "]: " << message << '\n';
                    Ok = false;
                }
            }

            void ExpectNear(double actual, double expected, double tolerance, const std::string& message)
            {
                Expect(std::abs(actual - expected) <= tolerance,
                    message + " expected " + std::to_string(expected) + " got " + std::to_string(actual));
            }
        };

        bool RunProperty(std::string_view name, const Spiral::Tests::Property& property)
        {
            return Spiral::Tests::RunNamedProperty("gizmo-snap", name, "SPIRAL_GIZMO_SNAP", 500, property);
        }

        // ---- exact integer model ---------------------------------------------

        struct ExactStep
        {
            i128 S = 1;
            int Q = 0;
        };

        // step == S * 2^-Q exactly.
        ExactStep DecomposeStep(double step)
        {
            int exponent = 0;
            const double mantissa = std::frexp(step, &exponent);
            return { i128(static_cast<i64>(std::ldexp(mantissa, 53))), 53 - exponent };
        }

        // Round-half-up (TowardPositive) or half-away-from-zero index of
        // valueUnits / S, where valueUnits is in units of 2^-Q.
        i128 OracleIndex(i128 valueUnits, i128 S, bool awayFromZero)
        {
            if (awayFromZero && valueUnits < 0)
                return -FloorDiv(2 * (-valueUnits) + S, 2 * S);
            return FloorDiv(2 * valueUnits + S, 2 * S);
        }

        // Index of an arbitrary double (|value| >= 2^-30 or zero) in units of the step, exactly.
        i128 OracleIndexOfDouble(double value, const ExactStep& step, bool awayFromZero)
        {
            if (value == 0.0)
                return 0;

            int exponent = 0;
            const double mantissa = std::frexp(value, &exponent);
            const i128 valueMantissa(static_cast<i64>(std::ldexp(mantissa, 53)));
            const int valueQ = 53 - exponent;
            const int shift = std::max(step.Q, valueQ);
            return OracleIndex(valueMantissa << (shift - valueQ), step.S << (shift - step.Q), awayFromZero);
        }

        // 2^-20 units to 2^-Q units; Q >= 20 for every step in range.
        i128 ToUnits(i128 n20, int q)
        {
            return n20 * (i128(1) << (q - 20));
        }

        long double UnitsToLongDouble(i128 units, int q)
        {
            return std::ldexp(Spiral::Tests::ToLongDouble(units), -q);
        }

        bool WithinUlps(double actual, long double expected, double ulps)
        {
            const double reference = static_cast<double>(expected);
            const double spacing = std::nextafter(std::abs(reference), std::numeric_limits<double>::infinity()) - std::abs(reference);
            return std::abs(static_cast<long double>(actual) - expected) <= ulps * spacing + 1e-300L;
        }

        struct AxisExpectation
        {
            i128 Sector = 0;
            long double Local = 0.0L;
        };

        // Documented rules in integers. localUnits and extentUnits are in 2^-Q units.
        AxisExpectation OracleSnapAxis(i64 sector, i128 localUnits, const ExactStep& step, i128 extentUnits)
        {
            const bool aligned = Spiral::Tests::IsDivisible(extentUnits, step.S);
            i128 k = OracleIndex(localUnits, step.S, false);
            AxisExpectation result;
            result.Sector = sector;
            if (aligned && 2 * k * step.S >= extentUnits)
            {
                const i128 value = k * step.S;
                const i128 carry = FloorDiv(2 * value + extentUnits, 2 * extentUnits);
                result.Sector = i128(sector) + carry;
                result.Local = UnitsToLongDouble(value - carry * extentUnits, step.Q);
                return result;
            }

            while (2 * k * step.S >= extentUnits)
                k = k - i128(1);
            while (2 * k * step.S < -extentUnits)
                k = k + i128(1);
            result.Local = UnitsToLongDouble(k * step.S, step.Q);
            return result;
        }

        WorldGridPolicy PolicyFor(double extent)
        {
            WorldGridPolicy policy;
            policy.SectorExtent = extent;
            policy.OriginHysteresis = 0.0;
            return policy;
        }

        constexpr double kSteps[] = {
            1.0, 0.5, 0.25, 0.1, 0.05, 0.01, 0.001, 2.0, 5.0, 10.0, 3.0, 0.0001, 7.0, 1000.0,
            4096.0, 8192.0, 0.015625, 0.0009765625, 0.3, 123.456,
            // 2048 / n rounds so that n * step is within half an ulp of E/2 for E = 4096.
            2048.0 / 3.0, 2048.0 / 7.0, 2048.0 / 9.0, 2048.0 / 15.0, 2048.0 / 59.0
        };
        constexpr double kExtents[] = { 4096.0, 1000.0, 100.0, 64.0, 8192.0 };

        // Exact power-of-two steps >= 2^-18 can produce exact half-step ties.
        bool IsPowerOfTwoStep(double step)
        {
            int exponent = 0;
            return std::frexp(step, &exponent) == 0.5 && step >= std::ldexp(1.0, -18);
        }

        // ---- Euler helpers ---------------------------------------------------

        Rotation3 FromEngine(const Engine::Math::Mat4& matrix)
        {
            Rotation3 result;
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 3; ++column)
                    result.M[row][column] = matrix.Values[row * 4 + column];
            }

            return result;
        }

        Engine::Math::Mat4 EngineRotation(const Vec3& degrees)
        {
            return Engine::Math::RotationYawPitchRoll(
                Engine::Math::DegreesToRadians(degrees.Y),
                Engine::Math::DegreesToRadians(degrees.X),
                Engine::Math::DegreesToRadians(degrees.Z));
        }

        double MaxMatrixDifference(const Rotation3& a, const Rotation3& b)
        {
            double worst = 0.0;
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 3; ++column)
                    worst = std::max(worst, std::abs(a.M[row][column] - b.M[row][column]));
            }

            return worst;
        }

        // Independent Rodrigues rotation of a vector, no matrices.
        DVec3 RodriguesRotate(const DVec3& v, const DVec3& axis, double radians)
        {
            using namespace Engine::Math;
            return v * std::cos(radians) + Cross(axis, v) * std::sin(radians)
                + axis * (Dot(axis, v) * (1.0 - std::cos(radians)));
        }

        bool SameBits(const Vec3& a, const Vec3& b)
        {
            return std::memcmp(&a, &b, sizeof(Vec3)) == 0;
        }
    }

    bool TestGizmoSnapSettingsValidationAndCtrlInversion()
    {
        Checker check { "settings" };
        const SnapSettings defaults;
        check.Expect(!defaults.Enabled, "snapping is off by default");
        check.Expect(defaults.TranslateStep == 1.0 && defaults.RotateStepDegrees == 15.0 && defaults.ScaleStep == 0.1,
            "default steps are 1.0 unit, 15 degrees and 0.1");
        check.Expect(ValidateSnapSettings(defaults) == SnapSettingsError::None, "defaults are valid");

        struct Case
        {
            double Translate;
            double Rotate;
            double Scale;
            SnapSettingsError Expected;
        };
        const double nan = std::nan("");
        const double inf = std::numeric_limits<double>::infinity();
        const Case cases[] = {
            { kMinimumTranslateStep, 15.0, 0.1, SnapSettingsError::None },
            { kMaximumTranslateStep, 15.0, 0.1, SnapSettingsError::None },
            { 1.0, kMinimumRotateStepDegrees, 0.1, SnapSettingsError::None },
            { 1.0, kMaximumRotateStepDegrees, 0.1, SnapSettingsError::None },
            { 1.0, 15.0, kMinimumScaleStep, SnapSettingsError::None },
            { 1.0, 15.0, kMaximumScaleStep, SnapSettingsError::None },
            { std::nextafter(kMinimumTranslateStep, 0.0), 15.0, 0.1, SnapSettingsError::TranslateStep },
            { std::nextafter(kMaximumTranslateStep, inf), 15.0, 0.1, SnapSettingsError::TranslateStep },
            { 0.0, 15.0, 0.1, SnapSettingsError::TranslateStep },
            { -1.0, 15.0, 0.1, SnapSettingsError::TranslateStep },
            { nan, 15.0, 0.1, SnapSettingsError::TranslateStep },
            { 1.0, std::nextafter(kMinimumRotateStepDegrees, 0.0), 0.1, SnapSettingsError::RotateStep },
            { 1.0, std::nextafter(kMaximumRotateStepDegrees, inf), 0.1, SnapSettingsError::RotateStep },
            { 1.0, inf, 0.1, SnapSettingsError::RotateStep },
            { 1.0, 15.0, std::nextafter(kMinimumScaleStep, 0.0), SnapSettingsError::ScaleStep },
            { 1.0, 15.0, std::nextafter(kMaximumScaleStep, inf), SnapSettingsError::ScaleStep },
            { 1.0, 15.0, nan, SnapSettingsError::ScaleStep },
            { nan, nan, nan, SnapSettingsError::TranslateStep }
        };
        for (const Case& item : cases)
        {
            SnapSettings settings;
            settings.TranslateStep = item.Translate;
            settings.RotateStepDegrees = item.Rotate;
            settings.ScaleStep = item.Scale;
            check.Expect(ValidateSnapSettings(settings) == item.Expected,
                "validation of " + std::to_string(item.Translate) + "/" + std::to_string(item.Rotate) + "/" + std::to_string(item.Scale));
            const SnapSettings sanitized = SanitizeSnapSettings(settings);
            check.Expect(ValidateSnapSettings(sanitized) == SnapSettingsError::None, "sanitized settings are valid");
            if (item.Expected == SnapSettingsError::None)
                check.Expect(sanitized == settings, "valid settings are kept");
            check.Expect(std::string(DescribeSnapSettingsError(item.Expected)).size() > 0, "every error has text");
        }

        SnapSettings custom;
        custom.Enabled = true;
        custom.TranslateStep = 0.25;
        custom.RotateStepDegrees = std::nan("");
        const SnapSettings repaired = SanitizeSnapSettings(custom);
        check.Expect(repaired.Enabled && repaired.TranslateStep == 0.25 && repaired.RotateStepDegrees == 15.0,
            "sanitize keeps the toggle and valid steps and defaults only the invalid one");

        for (bool enabled : { false, true })
        {
            for (bool ctrl : { false, true })
            {
                SnapSettings settings;
                settings.Enabled = enabled;
                check.Expect(IsSnapActive(settings, ctrl) == (enabled != ctrl),
                    "Ctrl inverts the toggle: enabled " + std::to_string(enabled) + " ctrl " + std::to_string(ctrl));
            }
        }

        return check.Ok;
    }

    bool TestGizmoOracleInt128MatchesNativeArithmetic()
    {
        Checker check { "int128" };
        using Spiral::Tests::ToLongDouble;

        check.Expect(Int128(-1) < Int128(0) && Int128(5) > Int128(-5) && -Int128(7) == Int128(-7), "signed ordering and negation");
        check.Expect((i128(1) << 100) > (i128(1) << 99) && ((i128(1) << 100) >> 100) == i128(1), "wide shifts");
        check.Expect(FloorDiv(Int128(-7), Int128(2)) == Int128(-4) && FloorDiv(Int128(7), Int128(2)) == Int128(3)
                && FloorDiv(Int128(-8), Int128(2)) == Int128(-4) && FloorDiv(Int128(0), Int128(5)) == Int128(0),
            "floor division rounds toward negative infinity");
        check.Expect(Spiral::Tests::IsDivisible(i128(1000) << 40, i128(125)) && !Spiral::Tests::IsDivisible(i128(1001), i128(125)), "divisibility");

        const bool ok = RunProperty("TestGizmoOracleInt128MatchesNativeArithmetic",
            [](Spiral::Tests::ChoiceStream& stream, std::string& message)
            {
                const i64 a = stream.NextI64(std::numeric_limits<i64>::min(), std::numeric_limits<i64>::max(), { 0, 1, -1, std::numeric_limits<i64>::max(), std::numeric_limits<i64>::min() });
                const i64 b = stream.NextI64(std::numeric_limits<i64>::min(), std::numeric_limits<i64>::max(), { 0, 1, -1, 3, 1ll << 32 });
                const i64 c = stream.NextI64(1, std::numeric_limits<i64>::max(), { 1, 2, 3, 1ll << 52, 4096 });
                const int shift = static_cast<int>(stream.NextSize(0, 60));
                const i128 product = i128(a) * i128(b);
                const i128 sum = product + (i128(a) << shift) - i128(b);
                const i128 quotient = FloorDiv(sum, i128(c));
                // Division identity, portable: q * c <= sum < q * c + c.
                if (!(quotient * i128(c) <= sum && sum < quotient * i128(c) + i128(c)))
                {
                    message = "floor division identity fails";
                    return false;
                }

#if defined(__SIZEOF_INT128__)
                __extension__ typedef __int128 Native;
                const Native nativeProduct = static_cast<Native>(a) * static_cast<Native>(b);
                const Native nativeSum = nativeProduct + (static_cast<Native>(a) << shift) - static_cast<Native>(b);
                Native nativeQuotient = nativeSum / static_cast<Native>(c);
                if (nativeSum % static_cast<Native>(c) != 0 && nativeSum < 0)
                    --nativeQuotient;
                const auto same = [](const i128& value, Native native)
                {
                    return value.Lo == static_cast<std::uint64_t>(native) && value.Hi == static_cast<std::uint64_t>(native >> 64);
                };
                if (!same(product, nativeProduct) || !same(sum, nativeSum) || !same(quotient, nativeQuotient))
                {
                    message = "portable Int128 differs from native __int128 for " + std::to_string(a) + ", " + std::to_string(b) + ", " + std::to_string(c);
                    return false;
                }

                const long double expectedLong = static_cast<long double>(nativeSum);
                if (std::abs(ToLongDouble(sum) - expectedLong) > std::abs(expectedLong) * 1e-18L)
                {
                    message = "long double conversion differs";
                    return false;
                }
#endif
                return true;
            });
        check.Expect(ok, "generated Int128 property");
        return check.Ok;
    }

    bool TestGizmoSnapRoundingMatchesExactOracle()
    {
        Checker check { "rounding" };

        struct Tie
        {
            double Value;
            double Step;
            double Up;
            double Away;
        };
        const Tie ties[] = {
            { 0.5, 1.0, 1.0, 1.0 }, { -0.5, 1.0, 0.0, -1.0 }, { 1.5, 1.0, 2.0, 2.0 }, { -1.5, 1.0, -1.0, -2.0 },
            { 2.5, 1.0, 3.0, 3.0 }, { -2.5, 1.0, -2.0, -3.0 }, { 7.5, 15.0, 1.0, 1.0 }, { -7.5, 15.0, 0.0, -1.0 },
            { 0.0, 1.0, 0.0, 0.0 }, { 0.49999999999999994, 1.0, 0.0, 0.0 }, { -0.49999999999999994, 1.0, 0.0, 0.0 },
            { 0.125, 0.25, 1.0, 1.0 }, { -0.125, 0.25, 0.0, -1.0 }
        };
        for (const Tie& tie : ties)
        {
            double index = 123.0;
            check.Expect(TryRoundToStepIndex(tie.Value, tie.Step, TieRule::TowardPositive, index) && index == tie.Up,
                "toward +inf tie " + std::to_string(tie.Value) + "/" + std::to_string(tie.Step));
            check.Expect(TryRoundToStepIndex(tie.Value, tie.Step, TieRule::AwayFromZero, index) && index == tie.Away,
                "away from zero tie " + std::to_string(tie.Value) + "/" + std::to_string(tie.Step));
        }

        // 0.15 / 0.1 rounds to 1.4999999999999998 in doubles but the doubles are
        // really 0.1499999999999999944 and 0.1000000000000000055: the exact
        // residual decides, and the exact answer is index 1.
        double index = 0.0;
        check.Expect(TryRoundToStepIndex(0.15, 0.1, TieRule::TowardPositive, index) && index == 1.0, "0.15 / 0.1 uses the exact residual");
        check.Expect(TryRoundToStepIndex(0.35, 0.1, TieRule::TowardPositive, index) && index == 3.0, "0.35 / 0.1: the double 0.35 is below the exact tie");

        const double invalid[] = { std::nan(""), std::numeric_limits<double>::infinity() };
        for (double value : invalid)
        {
            check.Expect(!TryRoundToStepIndex(value, 1.0, TieRule::TowardPositive, index), "non-finite value rejected");
            check.Expect(!TryRoundToStepIndex(1.0, value, TieRule::TowardPositive, index), "non-finite step rejected");
        }

        check.Expect(!TryRoundToStepIndex(1.0, 0.0, TieRule::TowardPositive, index), "zero step rejected");
        check.Expect(!TryRoundToStepIndex(1.0, -1.0, TieRule::AwayFromZero, index), "negative step rejected");
        check.Expect(!TryRoundToStepIndex(1e300, 1.0, TieRule::TowardPositive, index), "index beyond 2^52 rejected");
        check.Expect(TryRoundToStepIndex(4503599627370495.0, 1.0, TieRule::TowardPositive, index) && index == 4503599627370495.0, "largest exact index accepted");
        check.Expect(!TryRoundToStepIndex(4503599627370496.0, 1.0, TieRule::TowardPositive, index), "2^52 rejected");

        double snapped = 5.0;
        check.Expect(TrySnapRelative(-0.2, 1.0, snapped) && snapped == 0.0 && !std::signbit(snapped), "relative snap never returns -0");
        check.Expect(TrySnapAngleDegrees(22.5, 15.0, snapped) && snapped == 30.0, "22.5 degrees ties away to 30");
        check.Expect(TrySnapAngleDegrees(-22.5, 15.0, snapped) && snapped == -30.0, "-22.5 degrees ties away to -30");
        check.Expect(TrySnapAngleDegrees(22.4999, 15.0, snapped) && snapped == 15.0, "just under the tie stays");
        check.Expect(TrySnapAngleDegrees(-1000.0, 1.0, snapped) && snapped == -1000.0, "unit step is identity on integers");
        check.Expect(TrySnapScaleFactor(1.26, 0.1, snapped) && std::abs(snapped - 1.3) < 1e-12, "1.26 snaps to 1.3");
        check.Expect(TrySnapScaleFactor(1.0, 0.1, snapped) && snapped == 1.0, "factor 1 is a fixed point");
        check.Expect(TrySnapScaleFactor(0.74, 0.1, snapped) && std::abs(snapped - 0.7) < 1e-12, "0.74 snaps to 0.7");
        check.Expect(!TrySnapScaleFactor(std::nan(""), 0.1, snapped), "NaN factor rejected");

        const bool ok = RunProperty("TestGizmoSnapRoundingMatchesExactOracle",
            [](Spiral::Tests::ChoiceStream& stream, std::string& message)
            {
                const double step = kSteps[stream.NextSize(0, std::size(kSteps) - 1)];
                const ExactStep exact = DecomposeStep(step);
                const i64 n = stream.NextI64(-(static_cast<i64>(1) << 31), static_cast<i64>(1) << 31,
                    { 0, 1, -1, (static_cast<i64>(1) << 20), -(static_cast<i64>(1) << 20), (static_cast<i64>(1) << 19) });
                double value = std::ldexp(static_cast<double>(n), -20);
                const u64 mode = stream.Next() % 4;
                if (mode == 1 && IsPowerOfTwoStep(step))
                {
                    value = (static_cast<double>(stream.NextI64(-1000, 1000)) + 0.5) * step;
                }
                else if (mode == 2)
                {
                    // Near-ties for decimal steps: the double (k + 0.5) * step lands on either side of the exact tie.
                    value = (static_cast<double>(stream.NextI64(-3000, 3000)) + 0.5) * step;
                }
                else if (mode == 3)
                {
                    value = Spiral::Tests::RangeDouble(stream, -2048.0, 2048.0);
                }

                if (value != 0.0 && std::abs(value) < std::ldexp(1.0, -30))
                    value = 0.0;

                for (bool away : { false, true })
                {
                    double actual = 0.0;
                    if (!TryRoundToStepIndex(value, step, away ? TieRule::AwayFromZero : TieRule::TowardPositive, actual))
                    {
                        message = "rejected value " + std::to_string(value) + " step " + std::to_string(step);
                        return false;
                    }

                    const i128 expected = OracleIndexOfDouble(value, exact, away);
                    if (actual != static_cast<double>(Spiral::Tests::ToLongDouble(expected)))
                    {
                        message = std::string(away ? "away" : "up") + " index " + std::to_string(actual) + " expected "
                            + std::to_string(static_cast<double>(Spiral::Tests::ToLongDouble(expected))) + " for value " + std::to_string(value)
                            + " step " + std::to_string(step);
                        return false;
                    }
                }

                double relative = 0.0;
                if (!TrySnapRelative(value, step, relative))
                {
                    message = "TrySnapRelative rejected a valid value";
                    return false;
                }

                const long double product = Spiral::Tests::ToLongDouble(OracleIndexOfDouble(value, exact, true)) * UnitsToLongDouble(exact.S, exact.Q);
                if (!WithinUlps(relative, product, 1.0) || std::signbit(relative) != (relative < 0.0))
                {
                    message = "relative snap " + std::to_string(relative) + " is not k * step for value " + std::to_string(value);
                    return false;
                }

                return true;
            });
        check.Expect(ok, "generated rounding property");
        return check.Ok;
    }

    bool TestGizmoSnapTranslationMatchesInt128Oracle()
    {
        Checker check { "translation-oracle" };

        check.Expect(IsSectorLatticeAligned(1.0, PolicyFor(4096.0)) && IsSectorLatticeAligned(0.25, PolicyFor(4096.0))
                && IsSectorLatticeAligned(4096.0, PolicyFor(4096.0)) && IsSectorLatticeAligned(8.0, PolicyFor(1000.0)),
            "power-of-two and dividing steps are aligned");
        check.Expect(!IsSectorLatticeAligned(0.1, PolicyFor(4096.0)) && !IsSectorLatticeAligned(10.0, PolicyFor(4096.0))
                && !IsSectorLatticeAligned(3.0, PolicyFor(1000.0)) && !IsSectorLatticeAligned(8192.0, PolicyFor(4096.0))
                && !IsSectorLatticeAligned(0.1, PolicyFor(1000.0)),
            "decimal and non-dividing steps are not aligned");
        check.Expect(!IsSectorLatticeAligned(0.0, PolicyFor(4096.0)) && !IsSectorLatticeAligned(1.0, WorldGridPolicy { 99, 4096.0, 0.0, Engine::Math::WorldOriginMode::ExactCamera }),
            "invalid step or policy is not aligned");

        const bool ok = RunProperty("TestGizmoSnapTranslationMatchesInt128Oracle",
            [](Spiral::Tests::ChoiceStream& stream, std::string& message)
            {
                const double extent = kExtents[stream.NextSize(0, std::size(kExtents) - 1)];
                const WorldGridPolicy policy = PolicyFor(extent);
                const double step = kSteps[stream.NextSize(0, std::size(kSteps) - 1)];
                const ExactStep exact = DecomposeStep(step);
                const i128 extentUnits = i128(static_cast<i64>(extent)) << exact.Q;
                const i64 halfUnits20 = static_cast<i64>(extent * 0.5 * 1048576.0);

                SectorLocalPosition position;
                i128 localUnits[3] = {};
                i64* sectors[3] = { &position.Sector.X, &position.Sector.Y, &position.Sector.Z };
                double* locals[3] = { &position.Local.X, &position.Local.Y, &position.Local.Z };
                const std::vector<i64> sectorBoundaries = {
                    0, 1, -1, 1000000, -1000000, static_cast<i64>(1) << 60, -(static_cast<i64>(1) << 60),
                    std::numeric_limits<i64>::max() - 5, std::numeric_limits<i64>::min() + 5
                };
                for (int axis = 0; axis < 3; ++axis)
                {
                    *sectors[axis] = stream.NextI64(-2000000, 2000000, sectorBoundaries);
                    i64 n = stream.NextI64(-halfUnits20, halfUnits20 - 1,
                        { -halfUnits20, halfUnits20 - 1, 0, 1, -1, halfUnits20 - 2, -halfUnits20 + 1, halfUnits20 / 2 });
                    if (IsPowerOfTwoStep(step) && stream.NextBool())
                    {
                        // Exact half-step tie (an odd number of half steps) inside the sector.
                        const i64 stepUnits = static_cast<i64>(std::ldexp(step, 20));
                        const i64 halfSteps = 2 * (halfUnits20 - 1) / stepUnits;
                        if (halfSteps >= 1)
                        {
                            const i64 k = stream.NextI64(-(halfSteps + 1) / 2, (halfSteps - 1) / 2);
                            n = (2 * k + 1) * stepUnits / 2;
                        }
                    }

                    *locals[axis] = std::ldexp(static_cast<double>(n), -20);
                    localUnits[axis] = ToUnits(n, exact.Q);
                }

                if (!Engine::Math::IsCanonical(position, policy))
                {
                    message = "generator produced a non-canonical input";
                    return false;
                }

                const AxisMask mask { stream.NextBool(), stream.NextBool(), stream.NextBool() };
                const bool selected[3] = { mask.X, mask.Y, mask.Z };
                std::array<AxisExpectation, 3> expected;
                bool overflow = false;
                for (int axis = 0; axis < 3; ++axis)
                {
                    expected[axis] = OracleSnapAxis(*sectors[axis], localUnits[axis], exact, extentUnits);
                    overflow = overflow || (selected[axis]
                        && (!Spiral::Tests::FitsInt64(expected[axis].Sector)));
                }

                const SectorLocalPosition before = position;
                SectorLocalPosition snapped = position;
                snapped.Sector.X = 123456789;
                const bool succeeded = TrySnapTranslation(position, mask, step, policy, snapped);
                if (overflow)
                {
                    if (succeeded)
                    {
                        message = "snapping a position whose carry overflows the sector index must fail";
                        return false;
                    }

                    if (snapped.Sector.X != 123456789)
                    {
                        message = "failed snap modified its output";
                        return false;
                    }

                    return true;
                }

                if (!succeeded)
                {
                    message = "valid snap rejected for step " + std::to_string(step) + " extent " + std::to_string(extent);
                    return false;
                }

                if (!Engine::Math::IsCanonical(snapped, policy))
                {
                    message = "snapped position is not canonical";
                    return false;
                }

                const i64 resultSectors[3] = { snapped.Sector.X, snapped.Sector.Y, snapped.Sector.Z };
                const double resultLocals[3] = { snapped.Local.X, snapped.Local.Y, snapped.Local.Z };
                for (int axis = 0; axis < 3; ++axis)
                {
                    if (!selected[axis])
                    {
                        if (resultSectors[axis] != *sectors[axis] || std::memcmp(&resultLocals[axis], locals[axis], sizeof(double)) != 0)
                        {
                            message = "unconstrained axis " + std::to_string(axis) + " changed";
                            return false;
                        }

                        continue;
                    }

                    if (i128(resultSectors[axis]) != expected[axis].Sector || !WithinUlps(resultLocals[axis], expected[axis].Local, 2.0))
                    {
                        message = "axis " + std::to_string(axis) + " sector " + std::to_string(resultSectors[axis])
                            + " local " + std::to_string(resultLocals[axis]) + " expected sector "
                            + std::to_string(Spiral::Tests::ToInt64(expected[axis].Sector)) + " local " + std::to_string(static_cast<double>(expected[axis].Local))
                            + " (step " + std::to_string(step) + ", extent " + std::to_string(extent) + ", input local "
                            + std::to_string(*locals[axis]) + ")";
                        return false;
                    }
                }

                // Idempotence.
                SectorLocalPosition again;
                if (!TrySnapTranslation(snapped, mask, step, policy, again)
                    || again.Sector != snapped.Sector
                    || again.Local.X != snapped.Local.X || again.Local.Y != snapped.Local.Y || again.Local.Z != snapped.Local.Z)
                {
                    message = "snapping is not idempotent for step " + std::to_string(step) + " extent " + std::to_string(extent);
                    return false;
                }

                // The input is never modified.
                if (std::memcmp(&before, &position, sizeof(before)) != 0)
                {
                    message = "snap modified its input";
                    return false;
                }

                return true;
            });
        check.Expect(ok, "generated translation snap property");
        return check.Ok;
    }

    bool TestGizmoSnapTranslationBoundariesAndFailureAtomicity()
    {
        Checker check { "translation-boundaries" };
        const WorldGridPolicy policy = PolicyFor(4096.0);
        const double half = 2048.0;
        const AxisMask all { true, true, true };
        const AxisMask xOnly { true, false, false };

        const auto snapX = [&](i64 sector, double local, double step, AxisMask mask, SectorLocalPosition& out)
        {
            const SectorLocalPosition position { { sector, 7, -9 }, { local, 0.3, -0.7 } };
            return TrySnapTranslation(position, mask, step, policy, out);
        };

        SectorLocalPosition out;
        // E/2 - epsilon rounds up to E/2 and carries to the next sector at -E/2.
        check.Expect(snapX(4, half - 1e-9, 1.0, xOnly, out) && out.Sector.X == 5 && out.Local.X == -half, "E/2 - epsilon carries with step 1");
        check.Expect(snapX(4, half - 0.5001, 1.0, xOnly, out) && out.Sector.X == 4 && out.Local.X == half - 1.0, "just under the half-step tie stays in the sector");
        check.Expect(snapX(4, half - 0.5, 1.0, xOnly, out) && out.Sector.X == 5 && out.Local.X == -half, "exact tie below E/2 rounds up and carries");
        check.Expect(snapX(4, half - 0.4999, 1.0, xOnly, out) && out.Sector.X == 5 && out.Local.X == -half, "just over the half-step tie carries");
        check.Expect(snapX(4, -half, 1.0, xOnly, out) && out.Sector.X == 4 && out.Local.X == -half, "-E/2 is a lattice point and stays");
        check.Expect(snapX(4, -half + 0.4999, 1.0, xOnly, out) && out.Sector.X == 4 && out.Local.X == -half, "just above -E/2 snaps to -E/2");
        check.Expect(snapX(4, -half + 0.5, 1.0, xOnly, out) && out.Sector.X == 4 && out.Local.X == -half + 1.0, "tie above -E/2 rounds up");
        check.Expect(snapX(-4, -half + 0.0, 2.0, xOnly, out) && out.Sector.X == -4 && out.Local.X == -half, "negative sector at -E/2");

        // Odd ratio: E/2 is a half-step tie, so it rounds up and carries.
        {
            // An odd E / step ratio puts E/2 on a half-step tie that never occurs for a canonical local.
            const WorldGridPolicy odd = PolicyFor(1000.0);
            const SectorLocalPosition high { { 2, 0, 0 }, { 499.999, 0.0, 0.0 } };
            const SectorLocalPosition low { { 2, 0, 0 }, { -500.0, 0.0, 0.0 } };
            check.Expect(TrySnapTranslation(high, xOnly, 8.0, odd, out) && out.Sector.X == 2 && out.Local.X == 496.0, "odd ratio: no carry at the top");
            check.Expect(TrySnapTranslation(low, xOnly, 8.0, odd, out) && out.Sector.X == 2 && out.Local.X == -496.0, "odd ratio: -E/2 ties up to -496");
        }

        // Step not dividing E: no carry, the last in-sector lattice point is used.
        check.Expect(snapX(0, half - 1e-9, 3.0, xOnly, out) && out.Sector.X == 0 && out.Local.X == 3.0 * 682.0 && out.Local.X < half,
            "step 3: nearest lattice point below E/2 (2046)");
        check.Expect(snapX(0, -half, 3.0, xOnly, out) && out.Sector.X == 0 && out.Local.X == -3.0 * 682.0,
            "step 3: -E/2 clamps up to -2046");
        check.Expect(snapX(0, half - 1e-9, 0.1, xOnly, out) && out.Sector.X == 0 && out.Local.X < half && out.Local.X > half - 0.2,
            "decimal step stays inside the sector and is canonical");

        // Constrained axes only: others are bit-identical.
        const SectorLocalPosition position { { 3, 4, 5 }, { 1.4, -2.6, 3.5 } };
        SectorLocalPosition snapped;
        check.Expect(TrySnapTranslation(position, { false, true, false }, 1.0, policy, snapped), "Y-only snap");
        check.Expect(snapped.Sector == position.Sector && snapped.Local.X == 1.4 && snapped.Local.Y == -3.0 && snapped.Local.Z == 3.5,
            "only Y is snapped (-2.6 -> -3)");
        check.Expect(TrySnapTranslation(position, {}, 1.0, policy, snapped)
                && snapped.Local.X == 1.4 && snapped.Local.Y == -2.6 && snapped.Local.Z == 3.5,
            "an empty mask is the identity");
        check.Expect(TrySnapTranslation(position, all, 1.0, policy, snapped) && snapped.Local.X == 1.0 && snapped.Local.Y == -3.0 && snapped.Local.Z == 4.0,
            "all axes snap (3.5 ties up to 4)");

        // Failure atomicity.
        const SectorLocalPosition sentinel { { 11, 12, 13 }, { 0.5, 0.25, 0.125 } };
        SectorLocalPosition untouched = sentinel;
        const SectorLocalPosition edge { { std::numeric_limits<i64>::max(), 0, 0 }, { half - 0.25, 0.0, 0.0 } };
        check.Expect(!TrySnapTranslation(edge, xOnly, 1.0, policy, untouched) && std::memcmp(&untouched, &sentinel, sizeof(sentinel)) == 0,
            "carry beyond INT64_MAX fails and leaves the output alone");
        check.Expect(TrySnapTranslation(edge, { false, true, true }, 1.0, policy, untouched) && untouched.Sector.X == std::numeric_limits<i64>::max(),
            "the overflow axis is only an error when it is constrained");
        untouched = sentinel;
        const SectorLocalPosition lowEdge { { std::numeric_limits<i64>::min(), 0, 0 }, { -half, 0.0, 0.0 } };
        check.Expect(TrySnapTranslation(lowEdge, xOnly, 1.0, policy, untouched) && untouched.Sector.X == std::numeric_limits<i64>::min() && untouched.Local.X == -half,
            "INT64_MIN at -E/2 needs no carry");
        untouched = sentinel;
        const SectorLocalPosition nonCanonical { { 0, 0, 0 }, { half, 0.0, 0.0 } };
        check.Expect(!TrySnapTranslation(nonCanonical, xOnly, 1.0, policy, untouched) && std::memcmp(&untouched, &sentinel, sizeof(sentinel)) == 0,
            "a non-canonical input (local == E/2) is rejected");
        check.Expect(!TrySnapTranslation({ { 0, 0, 0 }, { std::nan(""), 0.0, 0.0 } }, xOnly, 1.0, policy, untouched), "NaN local rejected");
        check.Expect(!TrySnapTranslation(position, all, 0.0, policy, untouched), "zero step rejected");
        check.Expect(!TrySnapTranslation(position, all, -1.0, policy, untouched), "negative step rejected");
        check.Expect(!TrySnapTranslation(position, all, std::nan(""), policy, untouched), "NaN step rejected");
        WorldGridPolicy broken = policy;
        broken.Version = 99;
        check.Expect(!TrySnapTranslation(position, all, 1.0, broken, untouched), "invalid policy rejected");
        check.Expect(std::memcmp(&untouched, &sentinel, sizeof(sentinel)) == 0, "no failed call changed the output");

        // Translation invariance on an aligned lattice: snap(p + j*step) = snap(p) + j*step in one sector.
        for (double local : { -1500.25, -3.5, 0.0, 0.49, 0.5, 777.123, 1500.75 })
        {
            for (double j : { -7.0, -1.0, 1.0, 12.0 })
            {
                SectorLocalPosition a;
                SectorLocalPosition b;
                const double step = 0.25;
                const bool first = snapX(2, local, step, xOnly, a);
                const bool second = snapX(2, local + j * step, step, xOnly, b);
                check.Expect(first && second && b.Sector.X == a.Sector.X && b.Local.X == a.Local.X + j * step,
                    "translation invariance at " + std::to_string(local) + " + " + std::to_string(j));
            }
        }

        return check.Ok;
    }

    bool TestGizmoEulerRecompositionContinuityAndGimbal()
    {
        Checker check { "euler" };

        // RotationFromEulerDegrees is the upper 3x3 of the engine's float matrix.
        check.ExpectNear(MaxMatrixDifference(RotationFromEulerDegrees({ 20.0f, 35.0f, -70.0f }), FromEngine(EngineRotation({ 20.0f, 35.0f, -70.0f }))),
            0.0, 1e-6, "double rotation matches the engine matrix");

        // Gimbal table around pitch +-90: recompose with the engine matrix.
        for (float pitch : { 89.9f, -89.9f, 90.0f, -90.0f, 90.1f, -90.1f, 89.9999f })
        {
            for (float yaw : { -170.0f, -30.0f, 0.0f, 45.0f, 170.0f })
            {
                for (float roll : { -120.0f, 0.0f, 33.0f, 179.0f })
                {
                    const Vec3 original { pitch, yaw, roll };
                    const Rotation3 matrix = FromEngine(EngineRotation(original));
                    const Vec3 extracted = EulerDegreesFromRotation(matrix, original);
                    const double error = MaxMatrixDifference(FromEngine(EngineRotation(extracted)), matrix);
                    check.Expect(error < 1e-5, "recomposition error " + std::to_string(error) + " at pitch " + std::to_string(pitch)
                            + " yaw " + std::to_string(yaw) + " roll " + std::to_string(roll));
                    const bool gimbal = std::abs(std::abs(pitch) - 90.0f) < 0.0002f || std::abs(pitch) == 90.0f;
                    if (gimbal)
                        check.Expect(std::abs(extracted.Z - original.Z) < 1e-3f, "roll is kept at the gimbal");
                }
            }
        }

        // Exact gimbal rule with a different hint roll: yaw absorbs the remainder.
        {
            const Rotation3 matrix = FromEngine(EngineRotation({ 90.0f, 30.0f, 40.0f }));
            const Vec3 extracted = EulerDegreesFromRotation(matrix, { 90.0f, 0.0f, 10.0f });
            check.ExpectNear(extracted.X, 90.0, 1e-3, "gimbal pitch");
            check.ExpectNear(extracted.Z, 10.0, 1e-3, "gimbal roll comes from the hint");
            check.ExpectNear(extracted.Y, 60.0, 1e-3, "gimbal yaw absorbs the remainder (30 + 40 - 10)");
            const Rotation3 negative = FromEngine(EngineRotation({ -90.0f, 30.0f, 40.0f }));
            const Vec3 negativeExtracted = EulerDegreesFromRotation(negative, { -90.0f, 0.0f, 10.0f });
            check.ExpectNear(negativeExtracted.Y, 0.0 + 30.0 - 40.0 + 10.0, 1e-3, "negative gimbal yaw absorbs the remainder (30 - 40 + 10)");
        }

        // Non-finite hint falls back to zero.
        {
            const Rotation3 matrix = FromEngine(EngineRotation({ 10.0f, 20.0f, 30.0f }));
            const Vec3 extracted = EulerDegreesFromRotation(matrix, { std::nanf(""), std::nanf(""), std::nanf("") });
            check.ExpectNear(MaxMatrixDifference(FromEngine(EngineRotation(extracted)), matrix), 0.0, 1e-5, "NaN hint still recomposes");
        }

        // Continuity: rotating +20 degrees about world Y from yaw 170 gives yaw 190, not -170.
        {
            GizmoTransform start;
            start.RotationDegrees = { 0.0f, 170.0f, 0.0f };
            GizmoTransform rotated;
            double applied = 0.0;
            check.Expect(ApplyRotation(start, { 0.0, 1.0, 0.0 }, 20.0 * 3.14159265358979323846 / 180.0, false, {}, rotated, applied), "rotation applies");
            check.ExpectNear(rotated.RotationDegrees.Y, 190.0, 1e-3, "yaw continues past 180 instead of wrapping");
            check.ExpectNear(rotated.RotationDegrees.X, 0.0, 1e-3, "pitch stays");
            check.ExpectNear(applied, 20.0, 1e-9, "applied degrees");
        }

        const bool ok = RunProperty("TestGizmoEulerRecompositionContinuityAndGimbal",
            [](Spiral::Tests::ChoiceStream& stream, std::string& message)
            {
                using Spiral::Tests::RangeDouble;
                const Vec3 original {
                    static_cast<float>(RangeDouble(stream, -180.0, 180.0)),
                    static_cast<float>(RangeDouble(stream, -180.0, 180.0)),
                    static_cast<float>(RangeDouble(stream, -180.0, 180.0))
                };
                const Vec3 hint {
                    static_cast<float>(RangeDouble(stream, -720.0, 720.0)),
                    static_cast<float>(RangeDouble(stream, -720.0, 720.0)),
                    static_cast<float>(RangeDouble(stream, -720.0, 720.0))
                };
                const Rotation3 matrix = FromEngine(EngineRotation(original));
                const Vec3 fromHint = EulerDegreesFromRotation(matrix, hint);
                const double error = MaxMatrixDifference(FromEngine(EngineRotation(fromHint)), matrix);
                if (error > 1e-5)
                {
                    message = "recomposition error " + std::to_string(error);
                    return false;
                }

                // With the original as the hint the nearest branch is the original itself.
                const bool nearGimbal = std::abs(std::abs(original.X) - 90.0f) < 0.05f;
                if (!nearGimbal && std::abs(original.X) < 180.0f)
                {
                    const Vec3 same = EulerDegreesFromRotation(matrix, original);
                    const auto close = [](float a, float b) { return std::abs(a - b) < 2e-3f; };
                    if (!close(same.X, original.X) || !close(same.Y, original.Y) || !close(same.Z, original.Z))
                    {
                        message = "hint equal to the input did not return it: " + std::to_string(same.X) + "," + std::to_string(same.Y) + ","
                            + std::to_string(same.Z) + " vs " + std::to_string(original.X) + "," + std::to_string(original.Y) + "," + std::to_string(original.Z);
                        return false;
                    }
                }

                // Unwrapping: every returned angle is within 180 degrees of the hint.
                const Vec3 unwrapped = EulerDegreesFromRotation(matrix, hint);
                if (std::abs(unwrapped.Y - hint.Y) > 360.0f + 1e-3f || std::abs(unwrapped.Z - hint.Z) > 360.0f + 1e-3f)
                {
                    message = "result is not near the hint";
                    return false;
                }

                return true;
            });
        check.Expect(ok, "generated Euler recomposition property");
        return check.Ok;
    }

    bool TestGizmoApplyOperationsMatchOracles()
    {
        Checker check { "apply" };
        const WorldGridPolicy policy = PolicyFor(4096.0);
        const SnapSettings off;
        SnapSettings on;
        on.Enabled = true;

        // Basis: world is the identity, local follows the engine matrix rows, scale is always local.
        {
            DVec3 basis[3];
            BuildToolBasis(TransformTool::Translate, TransformSpace::World, { 10.0f, 20.0f, 30.0f }, basis);
            check.Expect(basis[0].X == 1.0 && basis[1].Y == 1.0 && basis[2].Z == 1.0 && basis[0].Y == 0.0, "world basis is the identity");
            const Vec3 degrees { 10.0f, 20.0f, 30.0f };
            const Engine::Math::Mat4 engine = EngineRotation(degrees);
            for (TransformTool tool : { TransformTool::Translate, TransformTool::Rotate, TransformTool::Scale })
            {
                BuildToolBasis(tool, TransformSpace::Local, degrees, basis);
                for (int row = 0; row < 3; ++row)
                {
                    check.ExpectNear(basis[row].X, engine.Values[row * 4 + 0], 1e-6, "local basis row X");
                    check.ExpectNear(basis[row].Y, engine.Values[row * 4 + 1], 1e-6, "local basis row Y");
                    check.ExpectNear(basis[row].Z, engine.Values[row * 4 + 2], 1e-6, "local basis row Z");
                }
            }

            BuildToolBasis(TransformTool::Scale, TransformSpace::World, degrees, basis);
            check.ExpectNear(basis[0].X, engine.Values[0], 1e-6, "scale ignores world space because a transform has no shear");
            BuildToolBasis(TransformTool::Select, TransformSpace::Local, degrees, basis);
            check.Expect(basis[0].X == 1.0 && basis[1].Y == 1.0, "select has the world basis");
        }

        // World-space axis translate with snap vs the exact model, through the two-stage rule
        // (normalize the sum, then snap in the canonical sector).
        const bool ok = RunProperty("TestGizmoApplyOperationsMatchOracles",
            [&policy](Spiral::Tests::ChoiceStream& stream, std::string& message)
            {
                const double extent = 4096.0;
                const double steps[] = { 1.0, 0.5, 0.25, 2.0, 0.125, 8.0, 64.0 };
                const double step = steps[stream.NextSize(0, std::size(steps) - 1)];
                const ExactStep exact = DecomposeStep(step);
                const i128 extentUnits = i128(static_cast<i64>(extent)) << exact.Q;
                const i64 half20 = static_cast<i64>(extent * 0.5 * 1048576.0);

                GizmoTransform start;
                start.Position.Sector = { stream.NextI64(-1000, 1000), 2, -3 };
                const i64 startN = stream.NextI64(-half20, half20 - 1, { -half20, half20 - 1, 0 });
                start.Position.Local = { std::ldexp(static_cast<double>(startN), -20), 5.5, -6.25 };
                start.RotationDegrees = { 12.0f, -34.0f, 56.0f };
                start.Scale = { 1.5f, 2.0f, 0.5f };
                const i64 deltaN = stream.NextI64(-2 * half20, 2 * half20, { 0, 1, half20, -half20, half20 + 1 });
                const double delta = std::ldexp(static_cast<double>(deltaN), -20);

                SnapSettings snap;
                snap.Enabled = true;
                snap.TranslateStep = step;
                GizmoTransform result;
                const DVec3 world[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
                if (!ApplyTranslation(start, world, GizmoHandle::AxisX, TransformSpace::World, { delta, 0.0, 0.0 }, true, snap, policy, result))
                {
                    message = "world translate rejected";
                    return false;
                }

                const i128 sumUnits = ToUnits(i128(startN) + i128(deltaN), exact.Q);
                const i128 carry = FloorDiv(2 * sumUnits + extentUnits, 2 * extentUnits);
                const AxisExpectation expected = OracleSnapAxis(start.Position.Sector.X + Spiral::Tests::ToInt64(carry),
                    sumUnits - carry * extentUnits, exact, extentUnits);
                if (i128(result.Position.Sector.X) != expected.Sector || !WithinUlps(result.Position.Local.X, expected.Local, 2.0))
                {
                    message = "snapped translate sector " + std::to_string(result.Position.Sector.X) + " local "
                        + std::to_string(result.Position.Local.X) + " expected " + std::to_string(Spiral::Tests::ToInt64(expected.Sector)) + " / "
                        + std::to_string(static_cast<double>(expected.Local));
                    return false;
                }

                const bool otherAxesSame = result.Position.Sector.Y == start.Position.Sector.Y && result.Position.Sector.Z == start.Position.Sector.Z
                    && result.Position.Local.Y == start.Position.Local.Y && result.Position.Local.Z == start.Position.Local.Z
                    && SameBits(result.RotationDegrees, start.RotationDegrees) && SameBits(result.Scale, start.Scale);
                if (!otherAxesSame)
                {
                    message = "a world X drag changed another axis, the rotation or the scale";
                    return false;
                }

                // Without snapping the result is exactly the normalized sum.
                GizmoTransform plain;
                if (!ApplyTranslation(start, world, GizmoHandle::AxisX, TransformSpace::World, { delta, 0.0, 0.0 }, false, snap, policy, plain))
                {
                    message = "unsnapped translate rejected";
                    return false;
                }

                const AxisExpectation sum = [&]
                {
                    AxisExpectation value;
                    value.Sector = i128(start.Position.Sector.X) + carry;
                    value.Local = UnitsToLongDouble(sumUnits - carry * extentUnits, exact.Q);
                    return value;
                }();
                if (i128(plain.Position.Sector.X) != sum.Sector || !WithinUlps(plain.Position.Local.X, sum.Local, 1.0))
                {
                    message = "unsnapped translate is not the normalized sum";
                    return false;
                }

                // Snapping off and Ctrl-inverted semantic is the caller's: Enabled=false must not snap.
                GizmoTransform disabled;
                if (!ApplyTranslation(start, world, GizmoHandle::AxisX, TransformSpace::World, { delta, 0.0, 0.0 }, false, snap, policy, disabled)
                    || disabled.Position.Local.X != plain.Position.Local.X)
                {
                    message = "snapActive=false changed the result";
                    return false;
                }

                return true;
            });
        check.Expect(ok, "generated world translate property");

        // Local-space translate snaps the distance along the rotated axis and never touches the lattice.
        {
            GizmoTransform start;
            start.Position = { { 1, 2, 3 }, { 10.0, 20.0, 30.0 } };
            start.RotationDegrees = { 0.0f, 90.0f, 0.0f };
            DVec3 basis[3];
            BuildToolBasis(TransformTool::Translate, TransformSpace::Local, start.RotationDegrees, basis);
            // Yaw 90: local X points along -Z... take whatever the engine says and move 2.6 units along it.
            SnapSettings snap;
            snap.Enabled = true;
            snap.TranslateStep = 1.0;
            GizmoTransform moved;
            const DVec3 raw = basis[0] * 2.6;
            check.Expect(ApplyTranslation(start, basis, GizmoHandle::AxisX, TransformSpace::Local, raw, true, snap, policy, moved), "local translate");
            const DVec3 delta {
                moved.Position.Local.X - 10.0, moved.Position.Local.Y - 20.0, moved.Position.Local.Z - 30.0
            };
            check.ExpectNear(Engine::Math::Dot(delta, basis[0]), 3.0, 1e-9, "2.6 snaps to 3 along the local axis");
            check.ExpectNear(Engine::Math::Length(delta), 3.0, 1e-9, "the move stays on the local axis");
            check.Expect(moved.Position.Sector == start.Position.Sector, "no sector change");
            check.Expect(ApplyTranslation(start, basis, GizmoHandle::AxisX, TransformSpace::Local, raw, false, snap, policy, moved), "unsnapped local translate");
            check.ExpectNear(Engine::Math::Length({ moved.Position.Local.X - 10.0, moved.Position.Local.Y - 20.0, moved.Position.Local.Z - 30.0 }), 2.6, 1e-9, "unsnapped local distance");

            // Plane handle: the out-of-plane component of the raw delta is dropped.
            const DVec3 planeRaw = basis[0] * 1.2 + basis[1] * -2.4 + basis[2] * 0.7;
            check.Expect(ApplyTranslation(start, basis, GizmoHandle::PlaneXY, TransformSpace::Local, planeRaw, true, snap, policy, moved), "local plane translate");
            const DVec3 planeDelta { moved.Position.Local.X - 10.0, moved.Position.Local.Y - 20.0, moved.Position.Local.Z - 30.0 };
            check.ExpectNear(Engine::Math::Dot(planeDelta, basis[0]), 1.0, 1e-9, "plane U component snaps 1.2 -> 1");
            check.ExpectNear(Engine::Math::Dot(planeDelta, basis[1]), -2.0, 1e-9, "plane V component snaps -2.4 -> -2");
            check.ExpectNear(Engine::Math::Dot(planeDelta, basis[2]), 0.0, 1e-9, "out-of-plane component removed");
        }

        // World plane handle ignores the out-of-plane component exactly.
        {
            GizmoTransform start;
            start.Position = { { 0, 0, 0 }, { 1.0, 2.0, 3.0 } };
            const DVec3 world[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
            GizmoTransform moved;
            check.Expect(ApplyTranslation(start, world, GizmoHandle::PlaneXZ, TransformSpace::World, { 0.4, 99.0, -0.4 }, false, off, policy, moved), "plane without snap");
            check.Expect(moved.Position.Local.X == 1.4 && moved.Position.Local.Y == 2.0 && moved.Position.Local.Z == 2.6, "Y is untouched by an XZ plane drag");
        }

        // Failure atomicity and rejection of unusable requests.
        {
            GizmoTransform start;
            start.Position = { { std::numeric_limits<i64>::max(), 0, 0 }, { 2047.9, 0.0, 0.0 } };
            const DVec3 world[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
            GizmoTransform sentinel;
            sentinel.Position = { { 7, 7, 7 }, { 7.0, 7.0, 7.0 } };
            GizmoTransform out = sentinel;
            check.Expect(!ApplyTranslation(start, world, GizmoHandle::AxisX, TransformSpace::World, { 5.0, 0.0, 0.0 }, false, off, policy, out)
                    && std::memcmp(&out.Position, &sentinel.Position, sizeof(sentinel.Position)) == 0,
                "carry beyond INT64_MAX fails without writing");
            check.Expect(!ApplyTranslation(start, world, GizmoHandle::None, TransformSpace::World, { 1.0, 0.0, 0.0 }, false, off, policy, out), "None handle rejected");
            check.Expect(!ApplyTranslation(start, world, GizmoHandle::AxisX, TransformSpace::World, { std::nan(""), 0.0, 0.0 }, false, off, policy, out), "NaN delta rejected");
            check.Expect(ApplyTranslation(start, world, GizmoHandle::AxisX, TransformSpace::World, { 0.0, 0.0, 0.0 }, false, off, policy, out), "a zero move on a representable position succeeds");
        }

        // Rotation: snap to 15 degrees, zero is the exact start, Rodrigues oracle for arbitrary axes.
        {
            GizmoTransform start;
            start.RotationDegrees = { 10.0f, 20.0f, 30.0f };
            GizmoTransform out;
            double applied = 0.0;
            check.Expect(ApplyRotation(start, { 0.0, 1.0, 0.0 }, 0.0, false, off, out, applied) && SameBits(out.RotationDegrees, start.RotationDegrees) && applied == 0.0,
                "zero angle returns the start rotation bit-identical");
            check.Expect(ApplyRotation(start, { 0.0, 1.0, 0.0 }, 0.1, true, on, out, applied) && SameBits(out.RotationDegrees, start.RotationDegrees) && applied == 0.0,
                "an angle under half a step snaps to zero and returns the start bit-identical");
            const double pi = 3.14159265358979323846;
            check.Expect(ApplyRotation(start, { 0.0, 1.0, 0.0 }, 22.4 * pi / 180.0, true, on, out, applied) && std::abs(applied - 15.0) < 1e-9, "22.4 degrees snaps to 15");
            check.Expect(ApplyRotation(start, { 0.0, 1.0, 0.0 }, -22.6 * pi / 180.0, true, on, out, applied) && std::abs(applied + 30.0) < 1e-9, "-22.6 degrees snaps to -30");
            check.Expect(ApplyRotation(start, { 0.0, 1.0, 0.0 }, 22.4 * pi / 180.0, false, on, out, applied) && std::abs(applied - 22.4) < 1e-9, "snapActive=false keeps the raw angle");
            check.Expect(!ApplyRotation(start, { 0.0, 1.0, 0.0 }, std::nan(""), false, on, out, applied), "NaN angle rejected");

            // A local-axis rotation of an entity whose other angles are zero returns exact multiples of the step.
            GizmoTransform yawOnly;
            yawOnly.RotationDegrees = { 0.0f, 30.0f, 0.0f };
            DVec3 basis[3];
            BuildToolBasis(TransformTool::Rotate, TransformSpace::Local, yawOnly.RotationDegrees, basis);
            for (int turns : { -3, -1, 1, 2, 5 })
            {
                check.Expect(ApplyRotation(yawOnly, basis[1], (turns * 15.0 + 2.0) * pi / 180.0, true, on, out, applied), "snapped local yaw");
                check.ExpectNear(out.RotationDegrees.Y, 30.0 + 15.0 * turns, 2e-4, "yaw lands on a multiple of the step");
                check.ExpectNear(out.RotationDegrees.X, 0.0, 2e-4, "pitch stays zero");
                check.ExpectNear(out.RotationDegrees.Z, 0.0, 2e-4, "roll stays zero");
            }
        }

        const bool rotationOk = RunProperty("TestGizmoApplyOperationsMatchOracles.Rotation",
            [](Spiral::Tests::ChoiceStream& stream, std::string& message)
            {
                using Spiral::Tests::RangeDouble;
                GizmoTransform start;
                start.RotationDegrees = {
                    static_cast<float>(RangeDouble(stream, -85.0, 85.0)),
                    static_cast<float>(RangeDouble(stream, -180.0, 180.0)),
                    static_cast<float>(RangeDouble(stream, -180.0, 180.0))
                };
                DVec3 axis { RangeDouble(stream, -1.0, 1.0), RangeDouble(stream, -1.0, 1.0), RangeDouble(stream, -1.0, 1.0) };
                if (!Engine::Math::TryNormalize(axis, axis))
                    return true;

                const bool localAxis = stream.NextBool();
                const Rotation3 before = FromEngine(EngineRotation(start.RotationDegrees));
                const int localIndex = static_cast<int>(stream.NextSize(0, 2));
                if (localAxis)
                    axis = Row(before, static_cast<u32>(localIndex));

                GizmoTransform out;
                double applied = 0.0;
                if (!ApplyRotation(start, axis, 0.0, false, {}, out, applied) || !SameBits(out.RotationDegrees, start.RotationDegrees))
                {
                    message = "a zero angle changed the start rotation bits";
                    return false;
                }

                const double radians = RangeDouble(stream, -6.0, 6.0);
                if (!ApplyRotation(start, axis, radians, false, {}, out, applied))
                {
                    message = "rotation rejected";
                    return false;
                }

                // Rodrigues model: each local axis vector of the start orientation is rotated about the world axis.
                const Rotation3 after = FromEngine(EngineRotation(out.RotationDegrees));
                for (u32 row = 0; row < 3; ++row)
                {
                    const DVec3 expected = RodriguesRotate(Row(before, row), axis, radians);
                    const DVec3 actual = Row(after, row);
                    if (Engine::Math::Length(expected - actual) > 2e-5)
                    {
                        message = "local axis " + std::to_string(row) + " differs from the Rodrigues model by "
                            + std::to_string(Engine::Math::Length(expected - actual));
                        return false;
                    }
                }

                if (localAxis)
                {
                    const DVec3 fixedAxis = Row(after, static_cast<u32>(localIndex));
                    if (Engine::Math::Length(fixedAxis - axis) > 2e-5)
                    {
                        message = "rotating about a local axis moved that axis";
                        return false;
                    }
                }

                return true;
            });
        check.Expect(rotationOk, "generated rotation property");

        // Scale: clamp, identity, constrained components, snapping.
        {
            GizmoTransform start;
            start.Scale = { 50.0f, 2.0f, 0.02f };
            GizmoTransform out;
            double factor = 0.0;
            check.Expect(ApplyScale(start, GizmoHandle::AxisX, 3.0, false, off, out, factor) && out.Scale.X == kMaximumScale && out.Scale.Y == 2.0f && out.Scale.Z == 0.02f, "axis scale clamps to 100 and leaves the others");
            check.Expect(ApplyScale(start, GizmoHandle::Center, 0.1, false, off, out, factor) && out.Scale.X == 5.0f && std::abs(out.Scale.Y - 0.2f) < 1e-6f && out.Scale.Z == kMinimumScale, "uniform scale multiplies every component and clamps to 0.01");
            check.Expect(ApplyScale(start, GizmoHandle::Center, 1.0, false, off, out, factor) && SameBits(out.Scale, start.Scale), "factor 1 returns the start scale bit-identical");
            check.Expect(ApplyScale(start, GizmoHandle::AxisY, 1.04, true, on, out, factor) && SameBits(out.Scale, start.Scale) && factor == 1.0, "1.04 snaps back to 1");
            check.Expect(ApplyScale(start, GizmoHandle::AxisY, 1.26, true, on, out, factor) && std::abs(out.Scale.Y - 2.6f) < 1e-5f && std::abs(factor - 1.3) < 1e-9, "1.26 snaps to 1.3");
            check.Expect(ApplyScale(start, GizmoHandle::AxisY, 1.26, false, on, out, factor) && std::abs(out.Scale.Y - 2.52f) < 1e-5f, "snapActive=false keeps the raw factor");
            check.Expect(ApplyScale(start, GizmoHandle::AxisY, 0.0004, true, on, out, factor) && out.Scale.Y == kMinimumScale, "a snapped factor of zero still clamps to the minimum");
            check.Expect(!ApplyScale(start, GizmoHandle::AxisY, 0.0, false, off, out, factor) && !ApplyScale(start, GizmoHandle::AxisY, -1.0, false, off, out, factor)
                    && !ApplyScale(start, GizmoHandle::AxisY, std::nan(""), false, off, out, factor) && !ApplyScale(start, GizmoHandle::None, 2.0, false, off, out, factor),
                "non-positive, NaN and None requests are rejected");
        }

        return check.Ok;
    }
}
