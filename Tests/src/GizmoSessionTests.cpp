#include "GizmoSessionTests.h"

#include "TestSupport/PropertyRunner.h"

#include "Viewport/GizmoSession.h"

#include "HistoryStore.h"

#include "Engine/Math/DVec3Ops.h"
#include "Engine/Math/Math.h"
#include "Engine/Math/WorldGrid.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace SpiralTests
{
    namespace
    {
        using namespace SpiralEditor;
        using Engine::i64;
        using Engine::u64;
        using Engine::Math::DVec3;
        using Engine::Math::SectorLocalPosition;
        using Engine::Math::Vec3;
        using Engine::Math::WorldGridPolicy;
        using Gizmo::GizmoHandle;
        using Gizmo::GizmoTransform;
        using Gizmo::ScreenPoint;
        using Gizmo::TransformSpace;
        using Gizmo::TransformTool;
        using Spiral::Tests::ChoiceStream;
        using Spiral::Tests::RangeDouble;

        // Failure hypotheses, oracles, and non-claims for the whole file:
        // - The session is the orchestration layer over the already-tested pure
        //   gizmo math, so every drag is checked end to end against an
        //   independent pinhole camera built from the rows of the engine's own
        //   float view matrix (no code shared with GizmoProjection), by
        //   inverse construction: choose a true 3D displacement, project the
        //   point it moves the grabbed handle to, feed that pixel, and require
        //   the Scene to hold start + displacement (snapped by hand arithmetic).
        // - The host is a real Engine::Scene plus a real HistoryStore, so "one
        //   history entry per drag", labels, undo and exact cancel are decided
        //   by the history and the Scene, not by the session's own call log.
        // - The property test replays pointer, key, command, camera, selection
        //   and viewport traces against a reference protocol model (call
        //   grammar, eligibility, tool-specific bit-identity, history parity)
        //   written without reading the implementation.
        // - Properties run from a replayable seed
        //   (SPIRAL_GIZMO_SESSION_SEED / SPIRAL_GIZMO_SESSION_REPLAY).
        // - Tier: fast, in-process. Not claimed: ImGui drawing, real input
        //   delivery, the Editor host, a renderer, or any hardware path.

        struct Checker
        {
            const char* Suite;
            bool Ok = true;

            void Expect(bool condition, const std::string& message)
            {
                if (!condition)
                {
                    std::cerr << "Gizmo session test failed [" << Suite << "]: " << message << '\n';
                    Ok = false;
                }
            }

            void ExpectNear(double actual, double expected, double tolerance, const std::string& message)
            {
                Expect(std::abs(actual - expected) <= tolerance,
                    message + " expected " + std::to_string(expected) + " got " + std::to_string(actual));
            }
        };

        bool SameBits(const Vec3& a, const Vec3& b)
        {
            return std::memcmp(&a, &b, sizeof(Vec3)) == 0;
        }

        bool SamePositionBits(const SectorLocalPosition& a, const SectorLocalPosition& b)
        {
            return a.Sector == b.Sector && std::memcmp(&a.Local, &b.Local, sizeof(a.Local)) == 0;
        }

        bool SameTransformBits(const GizmoTransform& a, const GizmoTransform& b)
        {
            return SamePositionBits(a.Position, b.Position) && SameBits(a.RotationDegrees, b.RotationDegrees)
                && SameBits(a.Scale, b.Scale);
        }

        std::string Fixed(const char* format, double value)
        {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), format, value + 0.0);
            return buffer;
        }

        bool IsFiniteTransform(const GizmoTransform& t)
        {
            return std::isfinite(t.Position.Local.X) && std::isfinite(t.Position.Local.Y) && std::isfinite(t.Position.Local.Z)
                && std::isfinite(t.RotationDegrees.X) && std::isfinite(t.RotationDegrees.Y) && std::isfinite(t.RotationDegrees.Z)
                && std::isfinite(t.Scale.X) && std::isfinite(t.Scale.Y) && std::isfinite(t.Scale.Z);
        }

        // ---- independent pinhole camera -----------------------------------

        // The camera sits at the origin of the camera-relative frame (the view's
        // translation origin is the camera), so v = world * R with R the upper
        // 3x3 of the engine's float view matrix; view axis k in world space is
        // column k.
        struct Pinhole
        {
            Engine::CameraView Camera;
            Gizmo::ViewportRect Rect;
            double R[3][3] {};

            DVec3 Axis(int k) const { return { R[0][k], R[1][k], R[2][k] }; }
            double P0() const { return Camera.Projection.Values[0]; }
            double P5() const { return Camera.Projection.Values[5]; }

            bool Build(const SectorLocalPosition& cameraPosition, const WorldGridPolicy& policy, const Vec3& rotation,
                float fov, float nearClip, float farClip, const Gizmo::ViewportRect& rect)
            {
                Rect = rect;
                Engine::CameraViewOriginTracker tracker;
                Engine::TrackedCameraViewRequest request;
                request.StableViewId = 1;
                request.CanonicalWorldPosition = cameraPosition;
                request.HasCanonicalWorldPosition = true;
                request.RotationDegrees = rotation;
                request.Projection = { fov, nearClip, farClip };
                request.AspectRatio = rect.Height > 0.0 ? static_cast<float>(rect.Width / rect.Height) : 1.0f;
                Engine::Math::TryComposeApproximateWorldPosition(cameraPosition, policy, request.WorldPosition);
                Camera = tracker.BuildView(request, policy);
                for (int row = 0; row < 3; ++row)
                {
                    for (int column = 0; column < 3; ++column)
                        R[row][column] = Camera.View.Values[row * 4 + column];
                }

                return Camera.Valid && Camera.HasCanonicalTranslationOrigin;
            }

            double Depth(const DVec3& relative) const { return Engine::Math::Dot(relative, Axis(2)); }

            ScreenPoint Pixel(const DVec3& relative) const
            {
                const double depth = Depth(relative);
                const double ndcX = Engine::Math::Dot(relative, Axis(0)) * P0() / depth;
                const double ndcY = Engine::Math::Dot(relative, Axis(1)) * P5() / depth;
                return { Rect.X + (ndcX * 0.5 + 0.5) * Rect.Width, Rect.Y + (0.5 - ndcY * 0.5) * Rect.Height };
            }
        };

        // ---- fake host over a real Scene and a real HistoryStore ----------

        struct DocState
        {
            std::vector<std::pair<Engine::EntityId, GizmoTransform>> Transforms;
        };

        GizmoTransform ReadTransform(const Engine::Scene& scene, Engine::Entity entity)
        {
            GizmoTransform result;
            if (const Engine::TransformComponent* transform = scene.TryGetTransform(entity))
            {
                result.Position = transform->GetPosition();
                result.RotationDegrees = transform->RotationDegrees;
                result.Scale = transform->Scale;
            }

            return result;
        }

        class DocAdapter final : public EditorHistory::IHistoryStateAdapter<DocState>
        {
        public:
            explicit DocAdapter(Engine::Scene& scene)
                : m_Scene(scene)
            {
            }

            bool Restore(const Snapshot& snapshot) override
            {
                for (const auto& [id, transform] : snapshot->Transforms)
                {
                    // A deleted entity is skipped: restoring it is the document's
                    // undelete, not the gizmo's concern.
                    if (m_Scene.IsEntityValid({ id }))
                        m_Scene.SetEntityTransform({ id }, transform.Position, transform.RotationDegrees, transform.Scale);
                }

                return true;
            }

            Engine::u64 EstimateBytes(const DocState& state) const override
            {
                return sizeof(DocState) + state.Transforms.size() * sizeof(state.Transforms[0]);
            }

            bool Equal(const DocState& first, const DocState& second) const override
            {
                if (first.Transforms.size() != second.Transforms.size())
                    return false;
                for (size_t index = 0; index < first.Transforms.size(); ++index)
                {
                    if (first.Transforms[index].first != second.Transforms[index].first
                        || !SameTransformBits(first.Transforms[index].second, second.Transforms[index].second))
                    {
                        return false;
                    }
                }

                return true;
            }

        private:
            Engine::Scene& m_Scene;
        };

        enum class EventKind
        {
            Begin,
            Apply,
            ApplyRejected,
            End,
            Cancel
        };

        struct HostEvent
        {
            EventKind Kind = EventKind::Begin;
            Gizmo::GizmoGestureKey Key;
            // Apply: the transform written. Others: the selected entity's stored
            // transform at the moment of the call.
            GizmoTransform Transform;
            std::string Label;
        };

        class SceneHost final : public IGizmoHost
        {
        public:
            explicit SceneHost(Engine::Scene& scene)
                : Scene(scene)
                , Adapter(scene)
                , History(Adapter)
            {
            }

            Engine::Scene& Scene;
            DocAdapter Adapter;
            EditorHistory::HistoryStore<DocState> History;
            Engine::Entity Selected;
            bool HideSelection = false;
            bool RejectApply = false;
            // Reports this stored transform instead of the Scene's (degenerate input).
            std::optional<GizmoTransform> ReportedTransform;
            std::vector<HostEvent> Events;
            std::vector<EditorHistory::HistoryResult> HistoryResults;

            std::shared_ptr<const DocState> Capture() const
            {
                auto state = std::make_shared<DocState>();
                for (const Engine::SceneEntity& entity : Scene.GetEntities())
                    state->Transforms.emplace_back(entity.EntityHandle.Id, ReadTransform(Scene, entity.EntityHandle));
                return state;
            }

            bool TryGetSelection(GizmoSelection& out) override
            {
                const Engine::SceneEntity* entity = HideSelection ? nullptr : Scene.TryGetEntity(Selected);
                if (!entity)
                    return false;

                out.EntityId = entity->EntityHandle.Id;
                out.Name = entity->Name;
                out.Transform = ReportedTransform ? *ReportedTransform : ReadTransform(Scene, Selected);
                out.AllowScale = !entity->Camera.has_value();
                return true;
            }

            void BeginGesture(const EditorHistory::HistoryLabel& label, const Gizmo::GizmoGestureKey& key) override
            {
                Events.push_back({ EventKind::Begin, key, ReadTransform(Scene, Selected), label.Display() });
                HistoryResults.push_back(History.BeginGesture(ToEditGestureKey(key), label, Capture()));
            }

            bool ApplyTransform(const Gizmo::GizmoGestureKey& key, const GizmoTransform& transform) override
            {
                const bool accepted = !RejectApply
                    && Scene.SetEntityTransform({ static_cast<Engine::EntityId>(key.EntityId) }, transform.Position,
                        transform.RotationDegrees, transform.Scale);
                Events.push_back({ accepted ? EventKind::Apply : EventKind::ApplyRejected, key, transform, {} });
                if (accepted)
                    History.UpdateGesture(ToEditGestureKey(key));
                return accepted;
            }

            void EndGesture(const Gizmo::GizmoGestureKey& key) override
            {
                Events.push_back({ EventKind::End, key, ReadTransform(Scene, { static_cast<Engine::EntityId>(key.EntityId) }), {} });
                HistoryResults.push_back(History.EndGesture(ToEditGestureKey(key), Capture()));
            }

            void CancelGesture(const Gizmo::GizmoGestureKey& key) override
            {
                // The stored transform at this instant is the gizmo's own restore;
                // History.CancelGesture restores the Before snapshot afterwards.
                Events.push_back({ EventKind::Cancel, key, ReadTransform(Scene, { static_cast<Engine::EntityId>(key.EntityId) }), {} });
                HistoryResults.push_back(History.CancelGesture());
            }

            size_t Count(EventKind kind) const
            {
                return static_cast<size_t>(std::count_if(Events.begin(), Events.end(), [kind](const HostEvent& e) { return e.Kind == kind; }));
            }
        };

        // ---- rig: one cube in front of an oblique camera -------------------

        constexpr double kExtent = 4096.0;

        struct Rig
        {
            WorldGridPolicy Policy;
            Engine::Scene Scene;
            Engine::Entity Cube;
            Engine::Entity Cube2;
            Engine::Entity Camera;
            SectorLocalPosition CameraPosition;
            Pinhole View;
            SceneHost Host;
            GizmoSession Session;
            CommandRegistry Registry;
            GizmoSessionInput Input;
            GizmoSessionResult Last;
            Gizmo::GizmoStyle Style;

            static WorldGridPolicy MakePolicy()
            {
                WorldGridPolicy policy;
                policy.SectorExtent = kExtent;
                policy.OriginHysteresis = 0.0;
                return policy;
            }

            explicit Rig(const Engine::Math::SectorIndex& sector = {}, const Vec3& cubeRotation = {}, const Vec3& cubeScale = { 1.0f, 1.0f, 1.0f },
                const Vec3& cameraRotation = { 12.0f, -18.0f, 0.0f }, const DVec3& cameraLocal = { 10.5, -4.25, 7.75 })
                : Policy(MakePolicy())
                , Scene("session", Policy)
                , Host(Scene)
            {
                CameraPosition = { sector, cameraLocal };
                const bool built = View.Build(CameraPosition, Policy, cameraRotation, 60.0f, 0.1f, 2000.0f, { 0.0, 0.0, 800.0, 600.0 });
                if (!built)
                    std::cerr << "rig camera failed to build\n";

                // Oblique placement: 12 ahead, 2 left and 0.5 up in camera terms.
                const DVec3 offset = View.Axis(2) * 12.0 + View.Axis(0) * -2.0 + View.Axis(1) * 0.5;
                const SectorLocalPosition at { sector,
                    { CameraPosition.Local.X + offset.X, CameraPosition.Local.Y + offset.Y, CameraPosition.Local.Z + offset.Z } };
                Cube = Scene.CreateEntity("Cube");
                Cube2 = Scene.CreateEntity("Cube2");
                Camera = Scene.CreateEntity("Main Camera");
                Scene.AddCameraComponent(Camera);
                Scene.SetEntityTransform(Cube, at, cubeRotation, cubeScale);
                Scene.SetEntityTransform(Cube2, { sector, { CameraPosition.Local.X - 3.0, CameraPosition.Local.Y, CameraPosition.Local.Z + 9.0 } }, {}, { 1.0f, 1.0f, 1.0f });
                Host.Selected = Cube;
                Input.Camera = &View.Camera;
                Input.Viewport = View.Rect;
                Input.Policy = Policy;
                Input.Pointer.Over = true;
                Session.RegisterCommands(Registry);
            }

            GizmoTransform CubeTransform() const { return ReadTransform(Scene, Cube); }

            // Camera-relative double position, by plain integer-sector arithmetic.
            DVec3 Relative(const SectorLocalPosition& p) const
            {
                return {
                    static_cast<double>(p.Sector.X - CameraPosition.Sector.X) * kExtent + (p.Local.X - CameraPosition.Local.X),
                    static_cast<double>(p.Sector.Y - CameraPosition.Sector.Y) * kExtent + (p.Local.Y - CameraPosition.Local.Y),
                    static_cast<double>(p.Sector.Z - CameraPosition.Sector.Z) * kExtent + (p.Local.Z - CameraPosition.Local.Z)
                };
            }

            DVec3 Pivot() const { return Relative(CubeTransform().Position); }

            // World length of Style.TargetPixels pixels perpendicular to the view at the pivot's depth.
            double Length(const DVec3& pivot) const
            {
                return Style.TargetPixels * 2.0 * View.Depth(pivot) / (View.P5() * View.Rect.Height);
            }

            // Row k of the rotation of an object matrix (the entity's local axis k).
            static DVec3 LocalAxis(const Vec3& rotationDegrees, int k)
            {
                const Engine::Math::Mat4 m = Engine::Math::RotationYawPitchRoll(
                    Engine::Math::DegreesToRadians(rotationDegrees.Y),
                    Engine::Math::DegreesToRadians(rotationDegrees.X),
                    Engine::Math::DegreesToRadians(rotationDegrees.Z));
                return { m.Values[k * 4 + 0], m.Values[k * 4 + 1], m.Values[k * 4 + 2] };
            }

            // The direction a handle's axis k is drawn: the tool basis, flipped
            // toward the eye (the origin of the relative frame) for arrows and boxes.
            DVec3 Direction(TransformTool tool, TransformSpace space, int k) const
            {
                const GizmoTransform t = CubeTransform();
                DVec3 basis = k == 0 ? DVec3 { 1, 0, 0 } : k == 1 ? DVec3 { 0, 1, 0 } : DVec3 { 0, 0, 1 };
                if (tool == TransformTool::Scale || (tool != TransformTool::Select && space == TransformSpace::Local))
                    basis = LocalAxis(t.RotationDegrees, k);
                const bool flips = tool == TransformTool::Translate || tool == TransformTool::Scale;
                if (flips && Engine::Math::Dot(basis, Pivot() * -1.0) < 0.0)
                    basis = basis * -1.0;
                return basis;
            }

            ScreenPoint OnArrow(TransformTool tool, TransformSpace space, int k, double fraction) const
            {
                const DVec3 pivot = Pivot();
                return View.Pixel(pivot + Direction(tool, space, k) * (fraction * Length(pivot)));
            }

            ScreenPoint Frame(const ScreenPoint& cursor, bool down, bool ctrl = false, bool escape = false, bool over = true)
            {
                Input.Pointer.Position = cursor;
                Input.Pointer.Over = over;
                Input.Pointer.PrimaryDown = down;
                Input.Pointer.CtrlDown = ctrl;
                Input.EscapePressed = escape;
                Last = Session.Update(Input, Host);
                return cursor;
            }
        };

        double SnapHalfUp(double value, double step)
        {
            return std::floor(value / step + 0.5) * step;
        }

        // Moves a raw coordinate away from a snap tie so a float-sized solver
        // error cannot flip the lattice index.
        double AwayFromTie(double value, double step)
        {
            double result = value;
            for (int attempt = 0; attempt < 20; ++attempt)
            {
                const double fraction = result / step - std::floor(result / step);
                if (std::abs(fraction - 0.5) > 0.12)
                    break;
                result += 0.1 * step;
            }

            return result;
        }
    }

    bool TestGizmoSessionStateSnapSettingsAndCommands()
    {
        Checker check { "state" };
        GizmoSession session;

        // Documented defaults: snapping off, translate 1.0, rotate 15 degrees, scale 0.1.
        const ViewportToolState defaults = session.State();
        check.Expect(!defaults.Snap.Enabled && defaults.Snap.TranslateStep == 1.0 && defaults.Snap.RotateStepDegrees == 15.0
                && defaults.Snap.ScaleStep == 0.1 && defaults.Tool == TransformTool::Translate && defaults.Space == TransformSpace::World,
            "defaults: snap off, 1.0 / 15 / 0.1, Translate, World");

        // Validated ranges, hand table of [minimum, maximum] per step. A rejected
        // value changes nothing, not even the other fields of the same request.
        struct Row
        {
            const char* Name;
            double Gizmo::SnapSettings::* Field;
            double Minimum;
            double Maximum;
            Gizmo::SnapSettingsError Error;
        };
        const Row rows[] = {
            { "translate", &Gizmo::SnapSettings::TranslateStep, 0.0001, 10000.0, Gizmo::SnapSettingsError::TranslateStep },
            { "rotate", &Gizmo::SnapSettings::RotateStepDegrees, 0.01, 180.0, Gizmo::SnapSettingsError::RotateStep },
            { "scale", &Gizmo::SnapSettings::ScaleStep, 0.001, 10.0, Gizmo::SnapSettingsError::ScaleStep }
        };
        for (const Row& row : rows)
        {
            const double accepted[] = { row.Minimum, row.Maximum, (row.Minimum + row.Maximum) * 0.5 };
            for (const double value : accepted)
            {
                Gizmo::SnapSettings snap;
                snap.*(row.Field) = value;
                const ToolStateResult result = session.SetSnapSettings(snap);
                check.Expect(result.Ok() && session.State().Snap == snap, std::string(row.Name) + " step accepted at " + std::to_string(value));
            }

            const double rejected[] = { row.Minimum * 0.5, row.Maximum * 1.5, 0.0, -row.Minimum,
                std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity() };
            for (const double value : rejected)
            {
                const ViewportToolState before = session.State();
                Gizmo::SnapSettings snap = before.Snap;
                snap.*(row.Field) = value;
                snap.Enabled = !snap.Enabled;
                const ToolStateResult result = session.SetSnapSettings(snap);
                check.Expect(result.Status == ToolStateStatus::InvalidSnapSettings && result.SnapError == row.Error && !result.Ok(),
                    std::string(row.Name) + " step rejected at " + std::to_string(value));
                check.Expect(session.State() == before, std::string(row.Name) + " rejection is atomic (toggle unchanged too)");
            }

            session.SetSnapSettings({});
        }

        // The first bad step is reported; a bad request does not move the tool either.
        {
            Gizmo::SnapSettings snap;
            snap.RotateStepDegrees = 0.0;
            snap.TranslateStep = -1.0;
            ViewportToolState state;
            state.Tool = TransformTool::Rotate;
            state.Space = TransformSpace::Local;
            state.Snap = snap;
            const ToolStateResult result = session.SetState(state);
            check.Expect(result.SnapError == Gizmo::SnapSettingsError::TranslateStep && session.State() == defaults,
                "SetState reports the first bad step and applies nothing");
            state.Snap = {};
            check.Expect(session.SetState(state).Ok() && session.State() == state, "a valid complete state is applied as given");
            session.SetState(defaults);
        }

        // Toggles.
        check.Expect(session.ToggleSnap().Ok() && session.State().Snap.Enabled, "snap toggles on");
        check.Expect(session.ToggleSnap().Ok() && !session.State().Snap.Enabled, "snap toggles off");
        check.Expect(session.ToggleSpace().Ok() && session.State().Space == TransformSpace::Local, "space toggles to Local");
        check.Expect(session.ToggleSpace().Ok() && session.State().Space == TransformSpace::World, "space toggles to World");
        check.Expect(session.SetTool(TransformTool::Scale).Ok() && session.State().Tool == TransformTool::Scale, "tool set");
        session.SetState(defaults);

        // Sector-lattice note: step divides the extent exactly or it does not.
        {
            WorldGridPolicy policy;
            policy.SectorExtent = 4096.0;
            Gizmo::SnapSettings snap;
            for (const double step : { 1.0, 2.0, 4.0, 0.5, 0.25, 8.0, 16.0 })
            {
                snap.TranslateStep = step;
                check.Expect(SnapLatticeNote(snap, policy).empty(), "no note for step " + std::to_string(step) + " in a 4096 sector");
            }
            for (const double step : { 3.0, 5.0, 10.0, 1000.0, 7.0 })
            {
                snap.TranslateStep = step;
                check.Expect(SnapLatticeNote(snap, policy) == "grid restarts at sector boundaries", "note for step " + std::to_string(step));
            }

            policy.SectorExtent = 1000.0;
            snap.TranslateStep = 10.0;
            check.Expect(SnapLatticeNote(snap, policy).empty(), "10 divides a 1000 sector");
            snap.TranslateStep = 3.0;
            check.Expect(!SnapLatticeNote(snap, policy).empty(), "3 does not divide a 1000 sector");
            snap.TranslateStep = std::numeric_limits<double>::quiet_NaN();
            check.Expect(SnapLatticeNote(snap, policy).empty(), "an invalid step has no note (the field error is shown instead)");
            policy.SectorExtent = -1.0;
            snap.TranslateStep = 3.0;
            check.Expect(SnapLatticeNote(snap, policy).empty(), "an invalid policy has no note");
        }

        // History identity: bit 63 separates gizmo gestures from 32-bit widget ids.
        {
            const Gizmo::GizmoGestureKey position { 7, 42, Gizmo::GizmoProperty::Position };
            const Gizmo::GizmoGestureKey rotation { 7, 42, Gizmo::GizmoProperty::Rotation };
            const Gizmo::GizmoGestureKey scale { 7, 42, Gizmo::GizmoProperty::Scale };
            const Gizmo::GizmoGestureKey next { 8, 42, Gizmo::GizmoProperty::Position };
            const auto a = ToEditGestureKey(position);
            check.Expect((a.Item >> 63) == 1 && a.Entity == 42 && a.Property == 1, "position key");
            check.Expect(ToEditGestureKey(rotation).Property == 2 && ToEditGestureKey(scale).Property == 3, "rotation and scale properties");
            check.Expect(ToEditGestureKey(next) != a && ToEditGestureKey(rotation) != a, "keys differ by gesture and by property");
            check.Expect(a.Item > 0xFFFFFFFFull, "never equal to a 32-bit ImGui item id");
        }

        // Commands: ids, shortcuts and dispatch through a real registry.
        {
            CommandRegistry registry;
            check.Expect(session.RegisterCommands(registry).Ok(), "commands register");
            const RegisterResult again = session.RegisterCommands(registry);
            check.Expect(again.Status == RegisterStatus::DuplicateId, "a second registration is a duplicate");

            struct Expected
            {
                const char* Id;
                const char* Title;
                int Key;
            };
            const Expected expected[] = {
                { "viewport.tool.select", "Select Tool", 81 },
                { "viewport.tool.translate", "Translate Tool", 87 },
                { "viewport.tool.rotate", "Rotate Tool", 69 },
                { "viewport.tool.scale", "Scale Tool", 82 },
                { "viewport.space.toggle", "Toggle Local/World Space", 88 },
                { "viewport.snap.toggle", "Toggle Snapping", 0 }
            };
            const ShortcutScope viewportScope { ShortcutScopeKind::Viewport, {} };
            const ShortcutScope globalScope { ShortcutScopeKind::Global, {} };
            for (const Expected& row : expected)
            {
                const CommandDescriptor* descriptor = registry.Find(row.Id);
                check.Expect(descriptor && descriptor->Title == row.Title && descriptor->Category == "Viewport", std::string("registered ") + row.Id);
                if (row.Key == 0)
                {
                    check.Expect(descriptor && descriptor->DefaultShortcuts.empty(), "snap toggle has no default chord");
                    continue;
                }

                const std::optional<ShortcutBinding> bound = registry.DefaultShortcuts().Resolve({ row.Key, Engine::InputModifierNone }, viewportScope);
                check.Expect(bound && bound->CommandId == row.Id, std::string("chord ") + std::to_string(row.Key) + " resolves in the viewport scope");
                check.Expect(!registry.DefaultShortcuts().Resolve({ row.Key, Engine::InputModifierNone }, globalScope).has_value(),
                    "viewport tool keys are inert in the global scope");
                check.Expect(!registry.DefaultShortcuts().Resolve({ row.Key, Engine::InputModifierControl }, viewportScope).has_value()
                        && !registry.DefaultShortcuts().Resolve({ row.Key, Engine::InputModifierAlt }, viewportScope).has_value()
                        && !registry.DefaultShortcuts().Resolve({ row.Key, Engine::InputModifierShift }, viewportScope).has_value(),
                    "tool keys with Ctrl, Alt or Shift are not bound");
            }

            const auto dispatch = [&](const char* id, CommandSource source = CommandSource::Shortcut)
            {
                return registry.Dispatch(id, source);
            };
            DispatchResult result = dispatch("viewport.tool.rotate");
            check.Expect(result.Executed() && result.Message == "Rotate tool" && session.State().Tool == TransformTool::Rotate, "E selects Rotate");
            check.Expect(dispatch("viewport.tool.scale").Executed() && session.State().Tool == TransformTool::Scale, "R selects Scale");
            check.Expect(dispatch("viewport.tool.select").Executed() && session.State().Tool == TransformTool::Select, "Q selects Select");
            check.Expect(dispatch("viewport.tool.translate").Executed() && session.State().Tool == TransformTool::Translate, "W selects Translate");
            result = dispatch("viewport.space.toggle");
            check.Expect(result.Executed() && result.Message == "Local space" && session.State().Space == TransformSpace::Local, "X toggles to Local");
            result = dispatch("viewport.space.toggle", CommandSource::Menu);
            check.Expect(result.Executed() && result.Message == "World space" && session.State().Space == TransformSpace::World, "and back");
            result = dispatch("viewport.snap.toggle", CommandSource::Menu);
            check.Expect(result.Executed() && result.Message == "Snapping on" && session.State().Snap.Enabled, "snap toggle on");
            result = dispatch("viewport.snap.toggle", CommandSource::Palette);
            check.Expect(result.Executed() && result.Message == "Snapping off" && !session.State().Snap.Enabled, "snap toggle off");
            check.Expect(dispatch("viewport.tool.rotate", CommandSource::Typed).Status == DispatchStatus::SourceNotAllowed,
                "typed control is not admitted by these commands");
            check.Expect(session.State() == defaults, "state is back at the defaults");
        }

        return check.Ok;
    }

    namespace
    {
        // The drawn arrow of one axis frozen at the drag start: the pivot and the
        // drawn direction do not move while the entity does.
        struct Arrow
        {
            const Rig* Owner = nullptr;
            DVec3 Pivot;
            DVec3 Dir;
            double Length = 0.0;

            Arrow(const Rig& rig, TransformTool tool, TransformSpace space, int axis)
                : Owner(&rig)
                , Pivot(rig.Pivot())
                , Dir(rig.Direction(tool, space, axis))
                , Length(rig.Length(Pivot))
            {
            }

            // The pixel at fraction * Length along the arrow plus `distance` further.
            ScreenPoint At(double fraction, double distance = 0.0) const
            {
                return Owner->View.Pixel(Pivot + Dir * (fraction * Length + distance));
            }
        };
    }

    namespace
    {
        // A world-space rotate ring of an unrotated entity frozen at the drag
        // start; At(theta) is the pixel of the point theta degrees round the ring
        // from the point nearest the eye (right-handed about the axis).
        struct Ring
        {
            const Rig* Owner = nullptr;
            DVec3 Pivot;
            DVec3 U;
            DVec3 V;
            double Length = 0.0;
            double Phi0 = 0.0;

            Ring(const Rig& rig, int axis)
                : Owner(&rig)
                , Pivot(rig.Pivot())
                , Length(rig.Length(Pivot))
            {
                const DVec3 unit[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
                U = unit[(axis + 1) % 3];
                V = unit[(axis + 2) % 3];
                const DVec3 toEye = Pivot * -1.0;
                Phi0 = std::atan2(Engine::Math::Dot(V, toEye), Engine::Math::Dot(U, toEye));
            }

            ScreenPoint At(double thetaDegrees) const
            {
                const double phi = Phi0 + thetaDegrees * 3.14159265358979323846 / 180.0;
                return Owner->View.Pixel(Pivot + (U * std::cos(phi) + V * std::sin(phi)) * Length);
            }
        };

        // Press, then `steps` frames moving the pointer a further `distance` along the arrow, then release.
        void DragArrow(Rig& rig, const Arrow& arrow, double distance, int steps = 6, bool release = true)
        {
            rig.Frame(arrow.At(0.9), false);
            rig.Frame(arrow.At(0.9), true);
            for (int step = 1; step <= steps; ++step)
                rig.Frame(arrow.At(0.9, distance * step / steps), true);
            if (release)
                rig.Frame(arrow.At(0.9, distance), false);
        }
    }

    bool TestGizmoSessionTranslateIsStartPlusSnappedDelta()
    {
        Checker check { "translate" };
        Rig rig;
        const GizmoTransform start = rig.CubeTransform();
        const double sigma = rig.Direction(TransformTool::Translate, TransformSpace::World, 0).X;
        check.Expect(std::abs(std::abs(sigma) - 1.0) < 1e-12, "the world X arrow is drawn along +-X");

        // Hover first: ownership, cursor, status, no host traffic.
        const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
        const ScreenPoint grab = arrow.At(0.9);
        rig.Frame(grab, false);
        check.Expect(rig.Last.Phase == Gizmo::GizmoPhase::Hover && rig.Last.Hover == GizmoHandle::AxisX && rig.Last.OwnsPointer
                && !rig.Last.PressStartedOnGizmo && rig.Last.Cursor == GizmoCursor::Hand && rig.Last.Status == "Move X axis",
            "hovering the X arrow: hover phase, owns the pointer, hand cursor, status " + rig.Last.Status);
        check.Expect(rig.Host.Events.empty(), "hover never touches the host");
        check.Expect(!rig.Last.Primitives.empty(), "handles are drawn");

        // Press: gesture 1 begins, the label names the verb and the entity, nothing is written yet.
        rig.Frame(grab, true);
        check.Expect(rig.Last.Phase == Gizmo::GizmoPhase::Active && rig.Last.Active == GizmoHandle::AxisX && rig.Last.GestureBegan
                && rig.Last.GestureId == 1 && rig.Last.PressStartedOnGizmo && rig.Last.OwnsPointer && rig.Last.Cursor == GizmoCursor::Grabbing,
            "press begins gesture 1");
        check.Expect(rig.Host.Events.size() == 1 && rig.Host.Events[0].Kind == EventKind::Begin && rig.Host.Events[0].Label == "Move Cube"
                && rig.Host.Events[0].Key.EntityId == rig.Cube.Id && rig.Host.Events[0].Key.Property == Gizmo::GizmoProperty::Position,
            "Begin precedes any write and names 'Move Cube'");
        check.Expect(SameTransformBits(rig.CubeTransform(), start), "the press itself writes nothing");
        check.Expect(rig.Last.Status == "Move: 0.00, 0.00, 0.00", "status at the press: " + rig.Last.Status);

        // Unsnapped frames are start + the total solved delta; only X changes, bit-exactly elsewhere.
        for (const double d : { 0.4, 1.1, 2.37, 3.0 })
        {
            rig.Frame(arrow.At(0.9, d), true);
            const GizmoTransform now = rig.CubeTransform();
            check.ExpectNear(now.Position.Local.X, start.Position.Local.X + sigma * d, 3e-4, "x is start + sigma * " + std::to_string(d));
            check.Expect(now.Position.Local.Y == start.Position.Local.Y && now.Position.Local.Z == start.Position.Local.Z
                    && now.Position.Sector == start.Position.Sector && SameBits(now.RotationDegrees, start.RotationDegrees) && SameBits(now.Scale, start.Scale),
                "an X drag leaves Y, Z, the sector, rotation and scale bit-identical");
            check.Expect(rig.Last.Status == "Move: " + Fixed("%+.2f", sigma * d) + ", 0.00, 0.00" && !rig.Last.SnapActive,
                "status shows the signed delta: " + rig.Last.Status);
        }

        // Ctrl with snapping off snaps to the absolute lattice (step 1) for that frame only.
        const double rawTarget = AwayFromTie(start.Position.Local.X + sigma * 3.6, 1.0);
        const ScreenPoint far = arrow.At(0.9, sigma * (rawTarget - start.Position.Local.X));
        rig.Frame(far, true, false);
        check.ExpectNear(rig.CubeTransform().Position.Local.X, rawTarget, 3e-4, "without Ctrl the position is unsnapped");
        rig.Frame(far, true, true);
        const double snapped = SnapHalfUp(rawTarget, 1.0);
        check.Expect(rig.CubeTransform().Position.Local.X == snapped, "Ctrl with snap off lands exactly on the lattice: " + std::to_string(snapped));
        check.Expect(rig.Last.SnapActive, "SnapActive reflects the inversion");
        check.Expect(rig.Last.Status == "Move: " + Fixed("%+.2f", snapped - start.Position.Local.X) + ", 0.00, 0.00 (snap 1 m, Ctrl)",
            "snapping is noted in the status: " + rig.Last.Status);
        rig.Frame(far, true, false);
        check.ExpectNear(rig.CubeTransform().Position.Local.X, rawTarget, 3e-4, "releasing Ctrl mid-drag returns to the unsnapped position (nothing accumulated)");
        rig.Frame(far, true, true);

        // Release with Ctrl still held: the last frame's snapped transform stays, one gesture closes.
        rig.Frame(far, false, true);
        check.Expect(rig.Last.GestureEnded && !rig.Last.GestureCancelled && rig.Last.Phase != Gizmo::GizmoPhase::Active, "release ends the gesture");
        check.Expect(rig.CubeTransform().Position.Local.X == snapped, "the snapped transform stays after release");
        check.Expect(rig.Last.PressStartedOnGizmo && rig.Last.OwnsPointer, "the release frame still owns the pointer (the click must not pick)");
        check.Expect(rig.Host.Count(EventKind::Begin) == 1 && rig.Host.Count(EventKind::End) == 1 && rig.Host.Count(EventKind::Cancel) == 0
                && rig.Host.Events.back().Kind == EventKind::End, "exactly one Begin and one End");
        check.Expect(rig.Host.History.EntryCount() == 1 && rig.Host.History.TopUndo() && rig.Host.History.TopUndo()->Label.Display() == "Move Cube",
            "the history holds one entry named 'Move Cube'");
        rig.Frame(far, false);
        check.Expect(!rig.Last.PressStartedOnGizmo && rig.Last.GestureId == 0 && rig.Last.Status.empty() == (rig.Last.Hover == GizmoHandle::None),
            "the latch clears one frame after the release");

        // Second drag with snapping ON at step 0.5: Ctrl suspends it; the toggle persists across drags.
        check.Expect(rig.Session.SetSnapSettings({ true, 0.5, 15.0, 0.1 }).Ok(), "snap settings between drags");
        rig.Host.Events.clear();
        const GizmoTransform second = rig.CubeTransform();
        const Arrow arrow2(rig, TransformTool::Translate, TransformSpace::World, 0);
        rig.Frame(arrow2.At(0.9), false);
        rig.Frame(arrow2.At(0.9), true);
        check.Expect(rig.Last.GestureId == 2, "the next drag is gesture 2");
        const double raw2 = AwayFromTie(second.Position.Local.X + sigma * 1.9, 0.5);
        const ScreenPoint to2 = arrow2.At(0.9, sigma * (raw2 - second.Position.Local.X));
        rig.Frame(to2, true, false);
        check.Expect(rig.CubeTransform().Position.Local.X == SnapHalfUp(raw2, 0.5) && rig.Last.SnapActive, "snap on: lattice 0.5");
        check.Expect(rig.Last.Status.find("(snap 0.5 m)") != std::string::npos, "status names the step: " + rig.Last.Status);
        rig.Frame(to2, true, true);
        check.ExpectNear(rig.CubeTransform().Position.Local.X, raw2, 3e-4, "Ctrl suspends snapping");
        check.Expect(!rig.Last.SnapActive && rig.Last.Status.find("(snap suspended, Ctrl)") != std::string::npos, "and says so: " + rig.Last.Status);
        rig.Frame(to2, false, false);
        check.Expect(rig.CubeTransform().Position.Local.X == SnapHalfUp(raw2, 0.5), "release without Ctrl keeps the snapped transform");
        check.Expect(rig.Host.History.EntryCount() == 2, "two drags, two entries");

        return check.Ok;
    }

    bool TestGizmoSessionLocalSpaceTranslateSnapsRelativeIncrements()
    {
        Checker check { "local" };
        Rig rig({}, { 0.0f, 30.0f, 0.0f });
        ViewportToolState state;
        state.Space = TransformSpace::Local;
        state.Snap = { true, 0.5, 15.0, 0.1 };
        check.Expect(rig.Session.SetState(state).Ok(), "Local space with snapping on");

        const GizmoTransform start = rig.CubeTransform();
        const Arrow arrow(rig, TransformTool::Translate, TransformSpace::Local, 0);
        const DVec3 localAxis = Rig::LocalAxis(start.RotationDegrees, 0);
        const double drawn = Engine::Math::Dot(arrow.Dir, localAxis);
        check.Expect(std::abs(std::abs(drawn) - 1.0) < 1e-6, "the arrow follows the entity's rotated X axis");

        rig.Frame(arrow.At(0.9), false);
        check.Expect(rig.Last.Hover == GizmoHandle::AxisX && rig.Last.Status == "Move X axis", "hover on the local X arrow");
        rig.Frame(arrow.At(0.9), true);

        // Snapped: the distance along the axis is a multiple of the step relative to the start.
        const double distance = AwayFromTie(2.3, 0.5);
        rig.Frame(arrow.At(0.9, distance), true);
        const double snappedDistance = std::floor(std::abs(distance) / 0.5 + 0.5) * 0.5 * (distance < 0 ? -1.0 : 1.0);
        GizmoTransform now = rig.CubeTransform();
        const DVec3 expected { start.Position.Local.X + arrow.Dir.X * snappedDistance, start.Position.Local.Y + arrow.Dir.Y * snappedDistance,
            start.Position.Local.Z + arrow.Dir.Z * snappedDistance };
        check.ExpectNear(now.Position.Local.X, expected.X, 1e-5, "local snapped x");
        check.ExpectNear(now.Position.Local.Y, expected.Y, 1e-5, "local snapped y");
        check.ExpectNear(now.Position.Local.Z, expected.Z, 1e-5, "local snapped z");
        check.Expect(SameBits(now.RotationDegrees, start.RotationDegrees) && SameBits(now.Scale, start.Scale),
            "rotation and scale are untouched by a local translate");
        // The status is expressed on the entity's own (undrawn-flip) X axis.
        const double shownAlongAxis = snappedDistance * drawn;
        check.Expect(rig.Last.Status == "Move (local): " + Fixed("%+.2f", shownAlongAxis) + ", 0.00, 0.00 (snap 0.5 m)",
            "local status: " + rig.Last.Status);

        // The same pointer with Ctrl suspends the snap: the exact solved distance.
        rig.Frame(arrow.At(0.9, distance), true, true);
        now = rig.CubeTransform();
        check.ExpectNear(now.Position.Local.X, start.Position.Local.X + arrow.Dir.X * distance, 3e-4, "unsnapped local x");
        check.ExpectNear(now.Position.Local.Z, start.Position.Local.Z + arrow.Dir.Z * distance, 3e-4, "unsnapped local z");
        rig.Frame(arrow.At(0.9, distance), false);
        check.Expect(rig.Host.History.EntryCount() == 1, "one entry");
        return check.Ok;
    }

    bool TestGizmoSessionRotateAndScaleDragsMatchOracles()
    {
        Checker check { "rotate-scale" };
        constexpr double kPi = 3.14159265358979323846;

        // ---- Rotate: a world X ring on an unrotated entity --------------------
        {
            Rig rig;
            rig.Session.SetTool(TransformTool::Rotate);
            const GizmoTransform start = rig.CubeTransform();
            const DVec3 pivot = rig.Pivot();
            const double length = rig.Length(pivot);
            const DVec3 toEye = pivot * -1.0;
            // Ring X lies in the YZ plane; start at the point nearest the eye so it is on the pickable half.
            const double phi0 = std::atan2(toEye.Z, toEye.Y);
            const auto ringPixel = [&](double thetaDegrees)
            {
                const double phi = phi0 + thetaDegrees * kPi / 180.0;
                return rig.View.Pixel(pivot + DVec3 { 0.0, std::cos(phi), std::sin(phi) } * length);
            };

            rig.Frame(ringPixel(0.0), false);
            check.Expect(rig.Last.Hover == GizmoHandle::AxisX && rig.Last.Status == "Rotate X ring", "hover on the X ring: " + rig.Last.Status);
            rig.Frame(ringPixel(0.0), true);
            check.Expect(rig.Last.Active == GizmoHandle::AxisX && rig.Host.Events.size() == 1 && rig.Host.Events[0].Label == "Rotate Cube"
                    && rig.Host.Events[0].Key.Property == Gizmo::GizmoProperty::Rotation, "press begins 'Rotate Cube' on the Rotation property");
            check.Expect(rig.Last.Status == "Rotate X: +0.0\xC2\xB0", "status at the press: " + rig.Last.Status);

            rig.Frame(ringPixel(40.0), true);
            GizmoTransform now = rig.CubeTransform();
            check.ExpectNear(now.RotationDegrees.X, 40.0, 0.02, "unsnapped pitch tracks the pointer angle");
            check.ExpectNear(now.RotationDegrees.Y, 0.0, 0.02, "yaw stays zero");
            check.ExpectNear(now.RotationDegrees.Z, 0.0, 0.02, "roll stays zero");
            check.Expect(SamePositionBits(now.Position, start.Position) && SameBits(now.Scale, start.Scale), "a rotate leaves position and scale bit-identical");
            check.Expect(rig.Last.Status == "Rotate X: +40.0\xC2\xB0", "unsnapped status: " + rig.Last.Status);

            rig.Frame(ringPixel(40.0), true, true);
            now = rig.CubeTransform();
            check.ExpectNear(now.RotationDegrees.X, 45.0, 0.02, "Ctrl snaps 40 degrees to 45");
            check.Expect(rig.Last.Status == "Rotate X: +45.0\xC2\xB0 (snap 15\xC2\xB0, Ctrl)", "snapped status: " + rig.Last.Status);

            // Keep turning past half a turn: the angle accumulates continuously.
            for (double theta = 60.0; theta <= 200.0; theta += 20.0)
                rig.Frame(ringPixel(theta), true);
            now = rig.CubeTransform();
            // The Euler triple is the one nearest the start rotation, so 200 degrees of
            // drag is reported as -160; the rotation itself is the same turn.
            const double wrapped = now.RotationDegrees.X - 360.0 * std::round((now.RotationDegrees.X - 200.0) / 360.0);
            check.ExpectNear(wrapped, 200.0, 0.05, "200 degrees of drag is a 200 degree turn about X (continuous accumulation through +-180)");
            check.ExpectNear(now.RotationDegrees.Y, 0.0, 0.02, "still no yaw after a turn past half a revolution");
            check.ExpectNear(now.RotationDegrees.Z, 0.0, 0.02, "still no roll after a turn past half a revolution");
            rig.Frame(ringPixel(200.0), false);
            check.Expect(rig.Last.GestureEnded && rig.Host.History.EntryCount() == 1 && rig.Host.History.TopUndo()->Label.Display() == "Rotate Cube",
                "one 'Rotate Cube' entry");
        }

        // ---- Scale: an axis box on a non-uniformly scaled entity ---------------
        {
            Rig rig({}, {}, { 2.0f, 3.0f, 4.0f });
            rig.Session.SetTool(TransformTool::Scale);
            const GizmoTransform start = rig.CubeTransform();
            const Arrow arrow(rig, TransformTool::Scale, TransformSpace::World, 0);
            rig.Frame(arrow.At(0.9), false);
            check.Expect(rig.Last.Hover == GizmoHandle::AxisX && rig.Last.Status == "Scale X axis", "hover on the X scale box: " + rig.Last.Status);
            rig.Frame(arrow.At(0.9), true);
            check.Expect(rig.Host.Events.size() == 1 && rig.Host.Events[0].Label == "Scale Cube"
                    && rig.Host.Events[0].Key.Property == Gizmo::GizmoProperty::Scale, "press begins 'Scale Cube'");
            check.Expect(rig.Last.Status == "Scale X: x1.00", "status at the press: " + rig.Last.Status);

            // tCurrent / tGrab = 1.4 / 0.9 along the drawn axis.
            const double factor = 1.4 / 0.9;
            rig.Frame(arrow.At(1.4), true);
            GizmoTransform now = rig.CubeTransform();
            check.ExpectNear(now.Scale.X, start.Scale.X * factor, 2e-3, "x scale is the start times the ratio of grab distances");
            check.Expect(now.Scale.Y == start.Scale.Y && now.Scale.Z == start.Scale.Z && SameBits(now.RotationDegrees, start.RotationDegrees)
                    && SamePositionBits(now.Position, start.Position), "an axis scale leaves the other axes, rotation and position bit-identical");
            check.Expect(rig.Last.Status == "Scale X: x" + Fixed("%.2f", factor), "unsnapped status: " + rig.Last.Status);

            rig.Frame(arrow.At(1.4), true, true);
            now = rig.CubeTransform();
            const double snappedFactor = 1.0 + std::round((factor - 1.0) / 0.1) * 0.1;
            check.Expect(now.Scale.X == static_cast<float>(static_cast<double>(start.Scale.X) * snappedFactor), "Ctrl snaps the factor to 1.6");
            check.Expect(rig.Last.Status == "Scale X: x1.60 (snap 0.1, Ctrl)", "snapped status: " + rig.Last.Status);

            // The range clamp: through the pivot is the Inspector minimum.
            rig.Frame(arrow.At(-0.3), true);
            check.Expect(rig.CubeTransform().Scale.X == 0.01f, "clamped to the Inspector minimum");
            rig.Frame(arrow.At(0.9), true);
            check.Expect(SameTransformBits(rig.CubeTransform(), start), "returning to the grab point returns the start scale exactly");
            rig.Frame(arrow.At(0.9), false);
            check.Expect(rig.Host.History.EntryCount() == 0, "a drag that ends where it began records nothing");
            check.Expect(rig.Host.HistoryResults.back().Status == EditorHistory::HistoryStatus::NoChange, "the history reports no change");
        }

        // ---- The Inspector maximum ---------------------------------------------
        {
            Rig rig({}, {}, { 80.0f, 3.0f, 4.0f });
            rig.Session.SetTool(TransformTool::Scale);
            const Arrow arrow(rig, TransformTool::Scale, TransformSpace::World, 0);
            rig.Frame(arrow.At(0.9), false);
            rig.Frame(arrow.At(0.9), true);
            rig.Frame(arrow.At(1.4), true);
            check.Expect(rig.CubeTransform().Scale.X == 100.0f, "80 times 1.56 is clamped to the Inspector maximum");
            rig.Frame(arrow.At(1.4), false);
        }

        // ---- Uniform scale from the centre ------------------------------------
        {
            Rig rig({}, {}, { 2.0f, 3.0f, 4.0f });
            rig.Session.SetTool(TransformTool::Scale);
            const GizmoTransform start = rig.CubeTransform();
            const ScreenPoint centre = rig.View.Pixel(rig.Pivot());
            rig.Frame(centre, false);
            check.Expect(rig.Last.Hover == GizmoHandle::Center && rig.Last.Status == "Scale uniformly", "the centre handle scales uniformly: " + rig.Last.Status);
            rig.Frame(centre, true);
            rig.Frame({ centre.X + 60.0, centre.Y - 40.0 }, true);
            GizmoTransform now = rig.CubeTransform();
            const double fx = now.Scale.X / start.Scale.X;
            check.Expect(fx > 1.05, "right and up grows");
            check.ExpectNear(now.Scale.Y / start.Scale.Y, fx, 1e-5, "Y grows by the same factor");
            check.ExpectNear(now.Scale.Z / start.Scale.Z, fx, 1e-5, "Z grows by the same factor");
            check.Expect(rig.Last.Status.rfind("Scale: x", 0) == 0, "uniform status has no axis letter: " + rig.Last.Status);
            rig.Frame({ centre.X - 60.0, centre.Y + 40.0 }, true);
            check.Expect(rig.CubeTransform().Scale.X < start.Scale.X, "left and down shrinks");
            rig.Frame(centre, true);
            check.Expect(SameTransformBits(rig.CubeTransform(), start), "zero travel is exactly the start");
            rig.Frame(centre, false);
        }

        // ---- A camera-bearing entity has no Scale tool --------------------------
        {
            Rig rig;
            rig.Host.Selected = rig.Camera;
            rig.Session.SetTool(TransformTool::Scale);
            rig.Frame({ 400.0, 300.0 }, false);
            check.Expect(rig.Last.Unavailable == GizmoUnavailable::ScaleNotAllowed && rig.Last.Primitives.empty() && !rig.Last.OwnsPointer,
                "no handles, no pointer ownership for a camera's Scale");
            check.Expect(std::string(DescribeGizmoUnavailable(GizmoUnavailable::ScaleNotAllowed)).find("Scale") != std::string::npos
                    && std::string(DescribeGizmoUnavailable(GizmoUnavailable::None)).empty(), "the reasons are worded");
        }

        return check.Ok;
    }

    bool TestGizmoSessionOneHistoryEntryPerDragAndExactCancel()
    {
        Checker check { "history" };
        Rig rig;
        const GizmoTransform t0 = rig.CubeTransform();

        // Three drags, three tools: three entries, each a verb and the entity name.
        {
            const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
            DragArrow(rig, arrow, 2.4, 8);
        }
        const GizmoTransform t1 = rig.CubeTransform();
        check.Expect(rig.Host.Count(EventKind::Begin) == 1 && rig.Host.Count(EventKind::End) == 1 && rig.Host.Count(EventKind::Apply) >= 8,
            "eight moving frames: one Begin, one End, many writes");
        check.Expect(rig.Host.History.EntryCount() == 1 && rig.Host.History.TopUndo()->Label.Display() == "Move Cube", "one 'Move Cube' entry");
        check.Expect(!SameTransformBits(t1, t0), "the move changed the transform");

        rig.Session.SetTool(TransformTool::Rotate);
        {
            const Ring ring(rig, 1);
            rig.Frame(ring.At(0.0), false);
            rig.Frame(ring.At(0.0), true);
            for (int step = 1; step <= 5; ++step)
                rig.Frame(ring.At(12.0 * step), true);
            rig.Frame(ring.At(60.0), false);
        }
        const GizmoTransform t2 = rig.CubeTransform();
        check.Expect(rig.Host.History.EntryCount() == 2 && rig.Host.History.TopUndo()->Label.Display() == "Rotate Cube", "one 'Rotate Cube' entry");
        check.Expect(!SameTransformBits(t2, t1) && SamePositionBits(t2.Position, t1.Position), "the rotate changed only the rotation");

        rig.Session.SetTool(TransformTool::Scale);
        {
            const Arrow arrow(rig, TransformTool::Scale, TransformSpace::World, 1);
            rig.Frame(arrow.At(0.9), false);
            rig.Frame(arrow.At(0.9), true);
            rig.Frame(arrow.At(1.2), true);
            rig.Frame(arrow.At(1.5), true);
            rig.Frame(arrow.At(1.5), false);
        }
        const GizmoTransform t3 = rig.CubeTransform();
        check.Expect(rig.Host.History.EntryCount() == 3 && rig.Host.History.TopUndo()->Label.Display() == "Scale Cube", "one 'Scale Cube' entry");
        check.Expect(rig.Host.Count(EventKind::Begin) == 3 && rig.Host.Count(EventKind::End) == 3 && rig.Host.Count(EventKind::Cancel) == 0, "three Begin and three End");

        // Undo and redo restore the exact bits.
        check.Expect(rig.Host.History.Undo().Succeeded() && SameTransformBits(rig.CubeTransform(), t2), "undo scale");
        check.Expect(rig.Host.History.Undo().Succeeded() && SameTransformBits(rig.CubeTransform(), t1), "undo rotate");
        check.Expect(rig.Host.History.Undo().Succeeded() && SameTransformBits(rig.CubeTransform(), t0), "undo move restores the original bits");
        check.Expect(rig.Host.History.Redo().Succeeded() && SameTransformBits(rig.CubeTransform(), t1), "redo move");
        check.Expect(rig.Host.History.Redo().Succeeded() && SameTransformBits(rig.CubeTransform(), t2), "redo rotate");
        check.Expect(rig.Host.History.Redo().Succeeded() && SameTransformBits(rig.CubeTransform(), t3), "redo scale");
        const size_t entries = rig.Host.History.EntryCount();

        // A press and release without moving records nothing.
        rig.Session.SetTool(TransformTool::Translate);
        rig.Host.Events.clear();
        {
            const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
            rig.Frame(arrow.At(0.9), false);
            rig.Frame(arrow.At(0.9), true);
            rig.Frame(arrow.At(0.9), false);
        }
        check.Expect(rig.Host.Count(EventKind::Begin) == 1 && rig.Host.Count(EventKind::End) == 1 && rig.Host.Count(EventKind::Apply) == 0,
            "an unmoved drag is Begin and End with no write");
        check.Expect(rig.Host.History.EntryCount() == entries && SameTransformBits(rig.CubeTransform(), t3), "and no entry");

        // Esc mid-drag restores exactly and records nothing; the button still being down starts nothing new.
        rig.Host.Events.clear();
        const GizmoTransform beforeCancel = rig.CubeTransform();
        {
            const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
            rig.Frame(arrow.At(0.9), false);
            rig.Frame(arrow.At(0.9), true);
            const u64 firstId = rig.Last.GestureId;
            for (int step = 1; step <= 3; ++step)
                rig.Frame(arrow.At(0.9, 0.7 * step), true);
            check.Expect(!SameTransformBits(rig.CubeTransform(), beforeCancel), "the drag moved the entity");
            rig.Frame(arrow.At(0.9, 2.1), true, false, true);
            check.Expect(rig.Last.GestureCancelled && !rig.Last.GestureEnded && rig.Last.Phase == Gizmo::GizmoPhase::Cancelled, "Esc cancels the gesture");
            check.Expect(SameTransformBits(rig.CubeTransform(), beforeCancel), "the entity is back at the start, bit for bit");
            const HostEvent& cancel = rig.Host.Events.back();
            const HostEvent& restore = rig.Host.Events[rig.Host.Events.size() - 2];
            check.Expect(cancel.Kind == EventKind::Cancel && SameTransformBits(cancel.Transform, beforeCancel),
                "the gizmo restored the start before CancelGesture was called");
            check.Expect(restore.Kind == EventKind::Apply && SameTransformBits(restore.Transform, beforeCancel), "through an ApplyTransform of the start transform");
            check.Expect(rig.Host.History.EntryCount() == entries && !rig.Host.History.GestureOpen(), "no entry, gesture closed");
            check.Expect(rig.Last.PressStartedOnGizmo && rig.Last.OwnsPointer && rig.Last.Active == GizmoHandle::None,
                "after Esc the held press still owns the pointer (releasing must not pick)");

            const size_t eventCount = rig.Host.Events.size();
            rig.Frame(arrow.At(0.9, 1.0), true);
            rig.Frame(arrow.At(0.9, 1.5), true);
            check.Expect(rig.Host.Events.size() == eventCount && rig.Last.Phase == Gizmo::GizmoPhase::Cancelled, "a held button after Esc starts no new gesture");
            rig.Frame(arrow.At(0.9, 1.5), false);
            check.Expect(rig.Last.PressStartedOnGizmo && rig.Host.Events.size() == eventCount, "the release frame is still owned, and writes nothing");
            rig.Frame(arrow.At(0.9, 1.5), false);
            check.Expect(!rig.Last.PressStartedOnGizmo, "the latch clears afterwards");

            // A fresh press begins a new gesture with the next id.
            rig.Frame(arrow.At(0.9), true);
            check.Expect(rig.Last.GestureBegan && rig.Last.GestureId > firstId, "a new press begins a new gesture");
            rig.Frame(arrow.At(0.9, 0.5), true);
            rig.Frame(arrow.At(0.9, 0.5), false);
            check.Expect(rig.Host.History.EntryCount() == entries + 1, "which records its own entry");
        }

        // Esc right after the press: nothing was written, so nothing is restored.
        rig.Host.Events.clear();
        {
            const GizmoTransform before = rig.CubeTransform();
            const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
            rig.Frame(arrow.At(0.9), false);
            rig.Frame(arrow.At(0.9), true);
            rig.Frame(arrow.At(0.9), true, false, true);
            check.Expect(rig.Host.Count(EventKind::Begin) == 1 && rig.Host.Count(EventKind::Cancel) == 1 && rig.Host.Count(EventKind::Apply) == 0
                    && SameTransformBits(rig.CubeTransform(), before), "Esc before any write is Begin then Cancel only");
            rig.Frame(arrow.At(0.9), false);
        }

        // Esc with no drag does nothing at all.
        rig.Host.Events.clear();
        rig.Frame({ 700.0, 60.0 }, false, false, true);
        check.Expect(rig.Host.Events.empty() && !rig.Last.GestureCancelled, "Esc while idle is inert");

        // The label sanitises the entity name through the history rules.
        rig.Scene.TryGetEntity(rig.Cube)->Name = "Cube\nTwo";
        {
            const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
            DragArrow(rig, arrow, 1.0, 3);
            check.Expect(rig.Host.History.TopUndo()->Label.Display() == "Move Cube Two", "control characters in a name become spaces: " + rig.Host.History.TopUndo()->Label.Display());
            rig.Scene.TryGetEntity(rig.Cube)->Name = std::string(100, 'a');
            DragArrow(rig, Arrow(rig, TransformTool::Translate, TransformSpace::World, 0), -1.0, 3);
            const EditorHistory::HistoryLabel& label = rig.Host.History.TopUndo()->Label;
            check.Expect(label.Verb == "Move" && label.Target.size() == 32 && label.Target.substr(29) == "...", "a long name is shortened to 32 code points: " + label.Target);
        }

        return check.Ok;
    }

    bool TestGizmoSessionPointerOwnershipAndHitPriority()
    {
        Checker check { "pointer" };
        const ScreenPoint empty { 700.0, 60.0 };

        // A click in empty space never touches the gizmo: nothing to suppress.
        {
            Rig rig;
            for (const bool down : { false, true, true, false, false })
            {
                rig.Frame(empty, down);
                check.Expect(!rig.Last.OwnsPointer && !rig.Last.PressStartedOnGizmo && rig.Last.Hover == GizmoHandle::None
                        && rig.Last.Cursor == GizmoCursor::Default && rig.Last.Status.empty(), "empty space owns nothing");
            }

            check.Expect(rig.Host.Events.empty(), "and writes nothing");
        }

        // A press that starts in empty space and is dragged across a handle with the button held:
        // the handle is hovered, but the click did not start on it and no gesture begins.
        {
            Rig rig;
            const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
            rig.Frame(empty, true);
            rig.Frame(arrow.At(0.9), true);
            check.Expect(rig.Last.Hover == GizmoHandle::AxisX && rig.Last.OwnsPointer && !rig.Last.PressStartedOnGizmo && !rig.Last.GestureBegan,
                "dragging onto a handle with the button held hovers it but is not a gizmo press");
            rig.Frame(arrow.At(0.9), false);
            check.Expect(!rig.Last.PressStartedOnGizmo && rig.Host.Events.empty(), "and releasing there is not a gizmo click");
        }

        // A click that starts on a handle owns the pointer through the release frame, then lets go.
        {
            Rig rig;
            const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
            rig.Frame(arrow.At(0.9), false);
            check.Expect(rig.Last.OwnsPointer && !rig.Last.PressStartedOnGizmo, "hover owns the pointer (the next press must not navigate or pick)");
            rig.Frame(arrow.At(0.9), true);
            check.Expect(rig.Last.OwnsPointer && rig.Last.PressStartedOnGizmo && rig.Last.GestureBegan, "press on the handle");
            rig.Frame(arrow.At(0.9, 0.5), true);
            check.Expect(rig.Last.OwnsPointer && rig.Last.PressStartedOnGizmo && !rig.Last.GestureBegan, "dragging");
            rig.Frame(empty, false);
            check.Expect(rig.Last.PressStartedOnGizmo && rig.Last.OwnsPointer && rig.Last.GestureEnded, "the release frame (pointer already away) is still owned");
            rig.Frame(empty, false);
            check.Expect(!rig.Last.OwnsPointer && !rig.Last.PressStartedOnGizmo, "the frame after releases the pointer");
        }

        // Hover requires an eligible pointer and a focused window.
        {
            Rig rig;
            const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
            rig.Frame(arrow.At(0.9), false, false, false, false);
            check.Expect(rig.Last.Hover == GizmoHandle::None && !rig.Last.OwnsPointer, "a pointer under a toolbar widget or outside the image hovers nothing");
            rig.Frame(arrow.At(0.9), true, false, false, false);
            check.Expect(rig.Host.Events.empty(), "and cannot press");
            rig.Frame(arrow.At(0.9), false);
            rig.Input.WindowFocused = false;
            rig.Frame(arrow.At(0.9), false);
            check.Expect(rig.Last.Hover == GizmoHandle::None, "an unfocused window hovers nothing");
            rig.Input.WindowFocused = true;
        }

        // Hit priority: the centre disc beats the axes that start under it.
        {
            Rig rig;
            const GizmoTransform start = rig.CubeTransform();
            const DVec3 pivot = rig.Pivot();
            const ScreenPoint centre = rig.View.Pixel(pivot);
            rig.Frame(centre, false);
            check.Expect(rig.Last.Hover == GizmoHandle::Center && rig.Last.Status == "Move in the screen plane", "centre wins at the pivot: " + rig.Last.Status);
            rig.Frame(centre, true);
            check.Expect(rig.Last.Active == GizmoHandle::Center && rig.Last.GestureBegan, "pressing the centre begins a screen-plane move");
            // Move 1.0 right and 0.5 up in camera terms: the point stays in the plane through the pivot facing the camera.
            const DVec3 target = pivot + rig.View.Axis(0) * 1.0 + rig.View.Axis(1) * 0.5;
            rig.Frame(rig.View.Pixel(target), true);
            const GizmoTransform now = rig.CubeTransform();
            const DVec3 moved { now.Position.Local.X - start.Position.Local.X, now.Position.Local.Y - start.Position.Local.Y,
                now.Position.Local.Z - start.Position.Local.Z };
            check.ExpectNear(moved.X, target.X - pivot.X, 3e-4, "screen-plane move x");
            check.ExpectNear(moved.Y, target.Y - pivot.Y, 3e-4, "screen-plane move y");
            check.ExpectNear(moved.Z, target.Z - pivot.Z, 3e-4, "screen-plane move z");
            check.ExpectNear(Engine::Math::Dot(moved, rig.View.Axis(2)), 0.0, 3e-4, "the move has no component along the view direction");
            rig.Frame(rig.View.Pixel(target), false);
            check.Expect(rig.Host.History.EntryCount() == 1, "one entry");
        }

        // A plane handle moves in its plane.
        {
            Rig rig;
            const GizmoTransform start = rig.CubeTransform();
            const DVec3 pivot = rig.Pivot();
            const double length = rig.Length(pivot);
            const DVec3 dx = rig.Direction(TransformTool::Translate, TransformSpace::World, 0);
            const DVec3 dy = rig.Direction(TransformTool::Translate, TransformSpace::World, 1);
            const ScreenPoint inside = rig.View.Pixel(pivot + dx * (0.45 * length) + dy * (0.45 * length));
            rig.Frame(inside, false);
            check.Expect(rig.Last.Hover == GizmoHandle::PlaneXY && rig.Last.Status == "Move XY plane", "inside the XY square: " + rig.Last.Status);
            rig.Frame(inside, true);
            const DVec3 to = pivot + dx * (0.45 * length + 1.2) + dy * (0.45 * length - 0.7);
            rig.Frame(rig.View.Pixel(to), true);
            const GizmoTransform now = rig.CubeTransform();
            check.ExpectNear(now.Position.Local.X - start.Position.Local.X, dx.X * 1.2, 3e-4, "plane move x");
            check.ExpectNear(now.Position.Local.Y - start.Position.Local.Y, dy.Y * -0.7, 3e-4, "plane move y");
            check.Expect(now.Position.Local.Z == start.Position.Local.Z, "the plane move leaves Z bit-identical");
            check.Expect(rig.Last.Status == "Move: " + Fixed("%+.2f", dx.X * 1.2) + ", " + Fixed("%+.2f", dy.Y * -0.7) + ", 0.00",
                "plane status: " + rig.Last.Status);
            rig.Frame(rig.View.Pixel(to), false);
        }

        return check.Ok;
    }

    bool TestGizmoSessionToolSwitchingAndDragLock()
    {
        Checker check { "tools" };
        Rig rig({}, { 0.0f, 30.0f, 0.0f });
        const auto signature = [&]()
        {
            std::string text;
            for (const Gizmo::GizmoDrawPrimitive& primitive : rig.Last.Primitives)
                text += std::to_string(static_cast<int>(primitive.Kind)) + ":" + std::to_string(primitive.Points.size()) + ";";
            return text;
        };

        // Hovering the X handle of each tool in turn, through the registry (the keys W, E, R, Q).
        const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
        const Arrow scaleArrow(rig, TransformTool::Scale, TransformSpace::World, 0);
        const Ring ring(rig, 0);
        rig.Frame(arrow.At(0.9), false);
        check.Expect(rig.Last.Hover == GizmoHandle::AxisX && rig.Last.Status == "Move X axis", "W: translate arrow");
        const std::string translateSignature = signature();

        check.Expect(rig.Registry.Dispatch("viewport.tool.rotate", CommandSource::Shortcut).Executed(), "E");
        rig.Frame(ring.At(0.0), false);
        check.Expect(rig.Last.Hover == GizmoHandle::AxisX && rig.Last.Status == "Rotate X ring", "E: rotate ring (status " + rig.Last.Status + ")");
        const std::string rotateSignature = signature();
        // The translate arrow's pixel under the Rotate tool is never reported as a translate handle.
        rig.Frame(arrow.At(0.9), false);
        check.Expect(rig.Last.Status.empty() || rig.Last.Status.rfind("Rotate ", 0) == 0, "switching tools leaves no stale hover: '" + rig.Last.Status + "'");

        check.Expect(rig.Registry.Dispatch("viewport.tool.scale", CommandSource::Shortcut).Executed(), "R");
        rig.Frame(scaleArrow.At(0.9), false);
        check.Expect(rig.Last.Hover == GizmoHandle::AxisX && rig.Last.Status == "Scale X axis", "R: scale box (status " + rig.Last.Status + ")");
        const std::string scaleSignature = signature();
        check.Expect(translateSignature != rotateSignature && rotateSignature != scaleSignature && translateSignature != scaleSignature,
            "each tool draws its own handle set");

        check.Expect(rig.Registry.Dispatch("viewport.tool.select", CommandSource::Shortcut).Executed(), "Q");
        rig.Frame(scaleArrow.At(0.9), false);
        check.Expect(rig.Last.Hover == GizmoHandle::None && !rig.Last.OwnsPointer && rig.Last.Primitives.empty() && rig.Last.Cursor == GizmoCursor::Default
                && rig.Last.Unavailable == GizmoUnavailable::SelectTool && rig.Last.Status.empty(), "Q: nothing shown, nothing owned");
        rig.Frame(scaleArrow.At(0.9), true);
        check.Expect(rig.Host.Events.empty() && !rig.Last.PressStartedOnGizmo, "and a press does nothing");
        rig.Frame(scaleArrow.At(0.9), false);

        check.Expect(rig.Registry.Dispatch("viewport.tool.translate", CommandSource::Shortcut).Executed(), "W again");
        rig.Frame(arrow.At(0.9), false);
        check.Expect(rig.Last.Hover == GizmoHandle::AxisX && rig.Last.Status == "Move X axis", "translate is back");

        // Switching the tool on the very frame of the press decides the property edited.
        rig.Session.SetTool(TransformTool::Rotate);
        rig.Frame(ring.At(0.0), true);
        check.Expect(rig.Last.GestureBegan && rig.Host.Events.size() == 1 && rig.Host.Events[0].Label == "Rotate Cube"
                && rig.Host.Events[0].Key.Property == Gizmo::GizmoProperty::Rotation, "a press after switching to E begins 'Rotate Cube'");

        // While a drag is open nothing about the tool, space or snapping can change.
        {
            const ViewportToolState before = rig.Session.State();
            check.Expect(rig.Session.DragActive(), "the drag is open");
            check.Expect(rig.Session.SetTool(TransformTool::Translate).Status == ToolStateStatus::DragInProgress, "SetTool refused");
            check.Expect(rig.Session.ToggleSpace().Status == ToolStateStatus::DragInProgress, "ToggleSpace refused");
            check.Expect(rig.Session.ToggleSnap().Status == ToolStateStatus::DragInProgress, "ToggleSnap refused");
            check.Expect(rig.Session.SetSnapSettings({ true, 2.0, 30.0, 0.5 }).Status == ToolStateStatus::DragInProgress, "SetSnapSettings refused");
            check.Expect(rig.Session.SetState({}).Status == ToolStateStatus::DragInProgress && rig.Session.State() == before, "SetState refused, nothing changed");
            for (const char* id : { "viewport.tool.select", "viewport.tool.translate", "viewport.tool.rotate", "viewport.tool.scale",
                     "viewport.space.toggle", "viewport.snap.toggle" })
            {
                const CommandAvailability availability = rig.Registry.Query(id, CommandSource::Shortcut);
                check.Expect(!availability.Enabled && availability.Reason == "A gizmo drag is in progress", std::string(id) + " is disabled with a reason");
                const DispatchResult result = rig.Registry.Dispatch(id, CommandSource::Shortcut);
                check.Expect(result.Status == DispatchStatus::Disabled, std::string(id) + " does not run");
            }

            check.Expect(rig.Session.State() == before, "the state survived every attempt");
            rig.Frame(ring.At(30.0), true);
            check.Expect(rig.Last.Status.rfind("Rotate X: ", 0) == 0, "the open drag continues with its own tool: " + rig.Last.Status);
            rig.Frame(ring.At(30.0), false);
            for (const char* id : { "viewport.tool.translate", "viewport.space.toggle", "viewport.snap.toggle" })
                check.Expect(rig.Registry.Dispatch(id, CommandSource::Shortcut).Executed(), std::string(id) + " runs once the drag is over");
        }

        // World and Local differ for a rotated entity: the same pixels hover different things.
        {
            Rig rotated({}, { 0.0f, 30.0f, 0.0f });
            const Arrow world(rotated, TransformTool::Translate, TransformSpace::World, 0);
            const Arrow local(rotated, TransformTool::Translate, TransformSpace::Local, 0);
            check.Expect(Engine::Math::Length(world.Dir - local.Dir) > 0.3, "the local X axis differs from the world X axis");
            rotated.Frame(world.At(0.9), false);
            check.Expect(rotated.Last.Hover == GizmoHandle::AxisX, "World: the world arrow");
            rotated.Frame(local.At(0.9), false);
            check.Expect(rotated.Last.Hover != GizmoHandle::AxisX, "World: the local arrow's pixel is not the X arrow");
            rotated.Registry.Dispatch("viewport.space.toggle", CommandSource::Shortcut);
            rotated.Frame(local.At(0.9), false);
            check.Expect(rotated.Last.Hover == GizmoHandle::AxisX, "X switched to Local: the local arrow");
            rotated.Frame(world.At(0.9), false);
            check.Expect(rotated.Last.Hover != GizmoHandle::AxisX, "Local: the world arrow's pixel is no longer the X arrow");
        }

        // Selecting a camera while Scale is the tool hides the handles; selecting a mesh brings them back.
        {
            Rig mixed;
            mixed.Session.SetTool(TransformTool::Scale);
            const Arrow handle(mixed, TransformTool::Scale, TransformSpace::World, 0);
            mixed.Frame(handle.At(0.9), false);
            check.Expect(mixed.Last.Hover == GizmoHandle::AxisX, "scale handles for the cube");
            mixed.Host.Selected = mixed.Camera;
            mixed.Frame(handle.At(0.9), false);
            check.Expect(mixed.Last.Hover == GizmoHandle::None && mixed.Last.Unavailable == GizmoUnavailable::ScaleNotAllowed, "none for the camera");
            mixed.Host.Selected = mixed.Cube;
            mixed.Frame(handle.At(0.9), false);
            check.Expect(mixed.Last.Hover == GizmoHandle::AxisX, "and back for the cube");
        }

        return check.Ok;
    }

    bool TestGizmoSessionYieldsToNavigationAndBlockedInput()
    {
        Checker check { "navigation" };

        struct Blocker
        {
            const char* Name;
            void (*Set)(Rig&, bool);
            // Navigation and blocking states end an open drag; a stray secondary or middle button does not.
            bool CancelsDrag;
        };
        const Blocker blockers[] = {
            { "navigation captured", [](Rig& rig, bool on) { rig.Input.NavigationActive = on; }, true },
            { "input blocked", [](Rig& rig, bool on) { rig.Input.InputBlocked = on; }, true },
            { "window unfocused", [](Rig& rig, bool on) { rig.Input.WindowFocused = !on; }, true },
            { "secondary button", [](Rig& rig, bool on) { rig.Input.Pointer.SecondaryDown = on; }, false },
            { "middle button", [](Rig& rig, bool on) { rig.Input.Pointer.MiddleDown = on; }, false }
        };

        for (const Blocker& blocker : blockers)
        {
            const std::string name = blocker.Name;

            // Idle: no hover, no press, nothing owned, the handles stay visible.
            {
                Rig rig;
                const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
                blocker.Set(rig, true);
                rig.Frame(arrow.At(0.9), false);
                check.Expect(rig.Last.Hover == GizmoHandle::None && !rig.Last.OwnsPointer && rig.Last.Cursor == GizmoCursor::Default
                        && !rig.Last.Primitives.empty(), name + ": no hover while blocked, handles still drawn");
                rig.Frame(arrow.At(0.9), true);
                check.Expect(rig.Host.Events.empty() && !rig.Last.GestureBegan && !rig.Last.PressStartedOnGizmo && !rig.Last.OwnsPointer,
                    name + ": a press on a handle neither begins nor owns the click");
                // Navigation ends while the button is still held: the old press never becomes a gizmo drag.
                blocker.Set(rig, false);
                rig.Frame(arrow.At(0.9, 0.5), true);
                check.Expect(rig.Host.Events.empty() && !rig.Last.PressStartedOnGizmo, name + ": a button held across the end of the block does not start a drag");
                rig.Frame(arrow.At(0.9), false);
                rig.Frame(arrow.At(0.9), true);
                check.Expect(rig.Last.GestureBegan && rig.Last.PressStartedOnGizmo, name + ": a fresh press afterwards begins normally");
                rig.Frame(arrow.At(0.9, 0.5), false);
            }

            // Mid-drag.
            {
                Rig rig;
                const GizmoTransform start = rig.CubeTransform();
                const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
                rig.Frame(arrow.At(0.9), false);
                rig.Frame(arrow.At(0.9), true);
                rig.Frame(arrow.At(0.9, 0.6), true);
                rig.Frame(arrow.At(0.9, 1.2), true);
                check.Expect(!SameTransformBits(rig.CubeTransform(), start), name + ": the drag moved the entity");
                blocker.Set(rig, true);
                rig.Frame(arrow.At(0.9, 1.8), true);
                if (blocker.CancelsDrag)
                {
                    check.Expect(rig.Last.GestureCancelled && rig.Last.Phase == Gizmo::GizmoPhase::Cancelled && SameTransformBits(rig.CubeTransform(), start),
                        name + ": the drag is cancelled and the start restored bit for bit");
                    check.Expect(rig.Host.Events.back().Kind == EventKind::Cancel && SameTransformBits(rig.Host.Events.back().Transform, start),
                        name + ": restored before CancelGesture");
                    check.Expect(rig.Host.History.EntryCount() == 0 && !rig.Host.History.GestureOpen(), name + ": no history entry");
                    check.Expect(rig.Last.PressStartedOnGizmo && rig.Last.OwnsPointer, name + ": the held press still owns the pointer");
                    blocker.Set(rig, false);
                    const size_t events = rig.Host.Events.size();
                    rig.Frame(arrow.At(0.9, 1.0), true);
                    check.Expect(rig.Host.Events.size() == events && rig.Last.Phase == Gizmo::GizmoPhase::Cancelled,
                        name + ": the held button does not resume the drag");
                    rig.Frame(arrow.At(0.9, 1.0), false);
                    check.Expect(rig.Host.Events.size() == events && rig.Host.History.EntryCount() == 0, name + ": release records nothing");
                }
                else
                {
                    check.Expect(!rig.Last.GestureCancelled && rig.Last.Phase == Gizmo::GizmoPhase::Active, name + ": the drag continues");
                    check.Expect(rig.Last.Hover == GizmoHandle::None && rig.Last.Active == GizmoHandle::AxisX, name + ": still on the same handle");
                    blocker.Set(rig, false);
                    rig.Frame(arrow.At(0.9, 1.8), false);
                    check.Expect(rig.Last.GestureEnded && rig.Host.History.EntryCount() == 1, name + ": it ends normally with one entry");
                }
            }
        }

        // A pointer that leaves the viewport image mid-drag keeps dragging (the drag owns the pointer).
        {
            Rig rig;
            const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
            rig.Frame(arrow.At(0.9), false);
            rig.Frame(arrow.At(0.9), true);
            const double x0 = rig.CubeTransform().Position.Local.X;
            rig.Frame(arrow.At(0.9, 1.5), true, false, false, false);
            check.Expect(rig.Last.Phase == Gizmo::GizmoPhase::Active && rig.CubeTransform().Position.Local.X != x0, "a pointer outside the image does not end the drag");
            rig.Frame(arrow.At(0.9, 1.5), false, false, false, false);
            check.Expect(rig.Last.GestureEnded && rig.Host.History.EntryCount() == 1, "and the release still ends it");
        }

        // A changed camera (the view moved, even by a sliver) ends the drag like Esc.
        {
            Rig rig;
            const GizmoTransform start = rig.CubeTransform();
            const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
            rig.Frame(arrow.At(0.9), false);
            rig.Frame(arrow.At(0.9), true);
            rig.Frame(arrow.At(0.9, 1.0), true);
            Pinhole nudged;
            nudged.Build(rig.CameraPosition, rig.Policy, { 12.0f, -17.9f, 0.0f }, 60.0f, 0.1f, 2000.0f, rig.View.Rect);
            rig.Input.Camera = &nudged.Camera;
            rig.Frame(arrow.At(0.9, 1.5), true);
            check.Expect(rig.Last.GestureCancelled && SameTransformBits(rig.CubeTransform(), start) && rig.Host.History.EntryCount() == 0,
                "a moved camera cancels and restores");
            rig.Input.Camera = &rig.View.Camera;
            rig.Frame(arrow.At(0.9), false);

            // And no camera at all.
            rig.Frame(arrow.At(0.9), false);
            rig.Frame(arrow.At(0.9), true);
            rig.Frame(arrow.At(0.9, 1.0), true);
            rig.Input.Camera = nullptr;
            rig.Frame(arrow.At(0.9, 1.5), true);
            check.Expect(rig.Last.GestureCancelled && SameTransformBits(rig.CubeTransform(), start) && rig.Last.Unavailable == GizmoUnavailable::NoView,
                "losing the camera cancels and restores");
        }

        return check.Ok;
    }

    bool TestGizmoSessionFarSectorPrecisionAndRollover()
    {
        Checker check { "far-sector" };
        const Engine::Math::SectorIndex far { 1'000'000'000'000, -2'000'000'000'000, 3'000'000'000'000 };

        // A plain drag far from the origin is the same drag: the sector never moves, the other axes stay bit-exact.
        {
            Rig rig(far);
            const GizmoTransform start = rig.CubeTransform();
            const double sigma = rig.Direction(TransformTool::Translate, TransformSpace::World, 0).X;
            const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
            check.Expect(Engine::Math::IsCanonical(start.Position, rig.Policy), "the start is canonical");
            rig.Frame(arrow.At(0.9), false);
            check.Expect(rig.Last.Hover == GizmoHandle::AxisX, "the handle is found at sector 1e12 (the view origin is the camera, not the world origin)");
            rig.Frame(arrow.At(0.9), true);
            for (const double d : { 0.7, 1.9, -2.2 })
            {
                rig.Frame(arrow.At(0.9, d), true);
                const GizmoTransform now = rig.CubeTransform();
                check.ExpectNear(now.Position.Local.X, start.Position.Local.X + sigma * d, 3e-4, "far x is start + sigma * " + std::to_string(d));
                check.Expect(now.Position.Sector == start.Position.Sector && now.Position.Local.Y == start.Position.Local.Y
                        && now.Position.Local.Z == start.Position.Local.Z, "far: sector, y and z bit-identical");
            }

            rig.Frame(arrow.At(0.9, -2.2), false);
            check.Expect(rig.Host.History.EntryCount() == 1 && Engine::Math::IsCanonical(rig.CubeTransform().Position, rig.Policy), "far: one entry, canonical");
        }

        // Snapping that reaches +E/2 carries into the next sector and back again (aligned step 4 divides 4096).
        {
            Rig rig(far, {}, { 1.0f, 1.0f, 1.0f }, { 12.0f, -18.0f, 0.0f }, { 2047.5, -4.25, 7.75 });
            rig.Session.SetSnapSettings({ true, 4.0, 15.0, 0.1 });
            const GizmoTransform start = rig.CubeTransform();
            check.Expect(start.Position.Local.X > 2030.0 && start.Position.Local.X < 2047.0, "the cube starts just inside +E/2: " + std::to_string(start.Position.Local.X));
            const double sigma = rig.Direction(TransformTool::Translate, TransformSpace::World, 0).X;
            const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
            rig.Frame(arrow.At(0.9), false);
            rig.Frame(arrow.At(0.9), true);

            // Raw x 2047.3 snaps to 2048 = +E/2: sector + 1, local -2048, exactly.
            rig.Frame(arrow.At(0.9, sigma * (2047.3 - start.Position.Local.X)), true);
            GizmoTransform now = rig.CubeTransform();
            check.Expect(now.Position.Sector.X == far.X + 1 && now.Position.Local.X == -2048.0, "snapping to +E/2 carries: sector + 1, local -2048 exactly");
            check.Expect(now.Position.Sector.Y == far.Y && now.Position.Sector.Z == far.Z && now.Position.Local.Y == start.Position.Local.Y
                    && now.Position.Local.Z == start.Position.Local.Z, "the other axes do not move");
            check.Expect(Engine::Math::IsCanonical(now.Position, rig.Policy), "the carried position is canonical");

            // Raw x 2041 snaps back to 2040 in the original sector: derived from the start, nothing stuck in the new sector.
            rig.Frame(arrow.At(0.9, sigma * (2041.0 - start.Position.Local.X)), true);
            now = rig.CubeTransform();
            check.Expect(now.Position.Sector == start.Position.Sector && now.Position.Local.X == 2040.0, "back across the boundary: sector unchanged, local 2040");

            // Cross again and release: the gizmo lands in the new sector and still finds the handle there.
            rig.Frame(arrow.At(0.9, sigma * (2047.3 - start.Position.Local.X)), true);
            rig.Frame(arrow.At(0.9, sigma * (2047.3 - start.Position.Local.X)), false);
            now = rig.CubeTransform();
            check.Expect(now.Position.Sector.X == far.X + 1 && now.Position.Local.X == -2048.0 && rig.Host.History.EntryCount() == 1, "released in the next sector, one entry");
            const Arrow after(rig, TransformTool::Translate, TransformSpace::World, 0);
            const DVec3 relative = rig.Pivot();
            check.ExpectNear(relative.X, 4096.0 - 2048.0 - 2047.5, 1e-6, "the pivot is exactly 2048 - 2047.5 + 4096 - 4096 away across the boundary");
            rig.Frame(after.At(0.9), false);
            check.Expect(rig.Last.Hover == GizmoHandle::AxisX, "the handle is hoverable after the carry (relative position uses the sector difference)");

            // Undo restores the pre-drag sector and local exactly.
            check.Expect(rig.Host.History.Undo().Succeeded() && SameTransformBits(rig.CubeTransform(), start), "undo restores sector and local bit for bit");
        }

        // At the end of the sector range the carry cannot be represented: the update is refused, nothing is written.
        {
            const Engine::Math::SectorIndex edge { std::numeric_limits<i64>::max(), 0, 0 };
            Rig rig(edge, {}, { 1.0f, 1.0f, 1.0f }, { 12.0f, -18.0f, 0.0f }, { 2047.5, -4.25, 7.75 });
            rig.Session.SetSnapSettings({ true, 4.0, 15.0, 0.1 });
            const GizmoTransform start = rig.CubeTransform();
            const double sigma = rig.Direction(TransformTool::Translate, TransformSpace::World, 0).X;
            const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
            rig.Frame(arrow.At(0.9), false);
            rig.Frame(arrow.At(0.9), true);
            rig.Frame(arrow.At(0.9, sigma * (2043.0 - start.Position.Local.X)), true);
            const GizmoTransform good = rig.CubeTransform();
            check.Expect(good.Position.Local.X == 2044.0 && good.Position.Sector == start.Position.Sector, "x lands on the lattice below the boundary");
            rig.Frame(arrow.At(0.9, sigma * (2047.3 - start.Position.Local.X)), true);
            check.Expect(SameTransformBits(rig.CubeTransform(), good), "a carry past INT64_MAX is refused and the last good transform stays");
            rig.Frame(arrow.At(0.9, sigma * (2047.3 - start.Position.Local.X)), false);
            check.Expect(Engine::Math::IsCanonical(rig.CubeTransform().Position, rig.Policy) && rig.Host.History.EntryCount() == 1, "still canonical, one entry");
        }

        return check.Ok;
    }

    bool TestGizmoSessionDegenerateInputs()
    {
        Checker check { "degenerate" };
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();

        // A viewport or camera that cannot define a projection shows nothing and starts nothing.
        {
            Rig rig;
            const ScreenPoint centre = rig.View.Pixel(rig.Pivot());
            struct Case
            {
                const char* Name;
                Gizmo::ViewportRect Rect;
            };
            const Case rects[] = {
                { "zero width", { 0.0, 0.0, 0.0, 600.0 } },
                { "zero height", { 0.0, 0.0, 800.0, 0.0 } },
                { "negative width", { 0.0, 0.0, -800.0, 600.0 } },
                { "NaN height", { 0.0, 0.0, 800.0, nan } },
                { "infinite width", { 0.0, 0.0, inf, 600.0 } },
                { "zero size", { 0.0, 0.0, 0.0, 0.0 } }
            };
            for (const Case& row : rects)
            {
                rig.Input.Viewport = row.Rect;
                rig.Frame(centre, false);
                const bool idle = rig.Last.Primitives.empty() && rig.Last.Hover == GizmoHandle::None && !rig.Last.OwnsPointer
                    && rig.Last.Unavailable == GizmoUnavailable::NoView;
                rig.Frame(centre, true);
                rig.Frame(centre, false);
                check.Expect(idle && rig.Host.Events.empty(), std::string(row.Name) + ": nothing drawn, hovered, owned or written");
            }

            rig.Input.Viewport = rig.View.Rect;
            struct CameraCase
            {
                const char* Name;
                void (*Mutate)(Engine::CameraView&);
            };
            const CameraCase cameras[] = {
                { "invalid view", [](Engine::CameraView& view) { view.Valid = false; } },
                { "NaN view matrix", [](Engine::CameraView& view) { view.View.Values[0] = std::numeric_limits<float>::quiet_NaN(); } },
                { "NaN projection", [](Engine::CameraView& view) { view.Projection.Values[0] = std::numeric_limits<float>::quiet_NaN(); } },
                { "orthographic projection", [](Engine::CameraView& view) { view.Projection.Values[11] = 0.0f; view.Projection.Values[15] = 1.0f; } },
                { "singular view", [](Engine::CameraView& view) { for (float& v : view.View.Values) v = 0.0f; } }
            };
            for (const CameraCase& row : cameras)
            {
                Engine::CameraView broken = rig.View.Camera;
                row.Mutate(broken);
                rig.Input.Camera = &broken;
                rig.Frame(centre, false);
                const bool idle = rig.Last.Primitives.empty() && rig.Last.Hover == GizmoHandle::None && rig.Last.Unavailable == GizmoUnavailable::NoView;
                rig.Frame(centre, true);
                rig.Frame(centre, false);
                check.Expect(idle && rig.Host.Events.empty(), std::string(row.Name) + ": nothing drawn or written");
            }

            rig.Input.Camera = nullptr;
            rig.Frame(centre, false);
            check.Expect(rig.Last.Unavailable == GizmoUnavailable::NoView && rig.Last.Primitives.empty(), "no camera at all");
            rig.Input.Camera = &rig.View.Camera;
            rig.Frame(centre, false);
            check.Expect(rig.Last.Hover == GizmoHandle::Center, "and everything works again once the view is valid");
        }

        // No selection and an entity behind the camera.
        {
            Rig rig;
            rig.Host.HideSelection = true;
            rig.Frame({ 400.0, 300.0 }, false);
            check.Expect(rig.Last.Unavailable == GizmoUnavailable::NoSelection && rig.Last.Primitives.empty(), "nothing selected");
            rig.Host.HideSelection = false;
            const DVec3 behind = rig.View.Axis(2) * -5.0;
            const SectorLocalPosition at { rig.CameraPosition.Sector,
                { rig.CameraPosition.Local.X + behind.X, rig.CameraPosition.Local.Y + behind.Y, rig.CameraPosition.Local.Z + behind.Z } };
            rig.Scene.SetEntityTransform(rig.Cube, at, {}, { 1.0f, 1.0f, 1.0f });
            for (const bool down : { false, true, false })
                rig.Frame({ 400.0, 300.0 }, down);
            check.Expect(rig.Last.Unavailable == GizmoUnavailable::NotVisible && rig.Last.Primitives.empty() && rig.Host.Events.empty(),
                "an entity behind the camera shows no handles and cannot be dragged");
        }

        // A zero-scale entity (a document that reports one) still translates; the Scene refuses to store it.
        {
            Rig rig;
            GizmoTransform reported = rig.CubeTransform();
            reported.Scale = { 0.0f, 0.0f, 0.0f };
            rig.Host.ReportedTransform = reported;
            const GizmoTransform stored = rig.CubeTransform();
            const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
            rig.Frame(arrow.At(0.9), false);
            check.Expect(rig.Last.Hover == GizmoHandle::AxisX, "the gizmo does not depend on the entity's scale");
            rig.Frame(arrow.At(0.9), true);
            rig.Frame(arrow.At(0.9, 1.0), true);
            check.Expect(rig.Host.Count(EventKind::ApplyRejected) > 0 && rig.Session.RejectedUpdateCount() > 0 && SameTransformBits(rig.CubeTransform(), stored),
                "the Scene rejects the zero scale; the stored transform is untouched and the rejection is counted");
            rig.Frame(arrow.At(0.9, 1.0), false);
            check.Expect(rig.Last.GestureEnded && rig.Host.History.EntryCount() == 0, "the drag ends cleanly without an entry");

            // Uniform scale from a zero scale reaches the minimum, which the Scene accepts.
            rig.Session.SetTool(TransformTool::Scale);
            const ScreenPoint centre = rig.View.Pixel(rig.Pivot());
            rig.Frame(centre, false);
            rig.Frame(centre, true);
            rig.Frame({ centre.X + 50.0, centre.Y - 30.0 }, true);
            const GizmoTransform now = rig.CubeTransform();
            check.Expect(now.Scale.X == 0.01f && now.Scale.Y == 0.01f && now.Scale.Z == 0.01f, "scaling up from zero lands on the 0.01 minimum");
            rig.Frame({ centre.X + 50.0, centre.Y - 30.0 }, false);
        }

        // A host that refuses writes: the drag runs, nothing changes, nothing is recorded.
        {
            Rig rig;
            const GizmoTransform start = rig.CubeTransform();
            rig.Host.RejectApply = true;
            const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
            DragArrow(rig, arrow, 2.0, 4);
            check.Expect(rig.Session.RejectedUpdateCount() == 5 && rig.Host.Count(EventKind::Apply) == 0 && SameTransformBits(rig.CubeTransform(), start)
                    && rig.Host.History.EntryCount() == 0 && rig.Last.GestureEnded, "every frame's write is refused (four moves and the release), one clean End, no entry");

            // Accepted, then refused: the last accepted transform stays at release.
            rig.Host.RejectApply = false;
            rig.Host.Events.clear();
            rig.Frame(arrow.At(0.9), false);
            rig.Frame(arrow.At(0.9), true);
            rig.Frame(arrow.At(0.9, 0.8), true);
            const GizmoTransform accepted = rig.CubeTransform();
            rig.Host.RejectApply = true;
            rig.Frame(arrow.At(0.9, 1.6), true);
            rig.Frame(arrow.At(0.9, 1.6), false);
            check.Expect(SameTransformBits(rig.CubeTransform(), accepted) && rig.Host.History.EntryCount() == 1, "the last accepted transform stays and is the one recorded");
            rig.Host.RejectApply = false;
        }

        // The selected entity is deleted mid-drag, or the selection moves to another entity.
        {
            Rig rig;
            const GizmoTransform other = ReadTransform(rig.Scene, rig.Cube2);
            const GizmoTransform start = rig.CubeTransform();
            const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
            rig.Frame(arrow.At(0.9), false);
            rig.Frame(arrow.At(0.9), true);
            rig.Frame(arrow.At(0.9, 1.0), true);
            rig.Host.Selected = rig.Cube2;
            rig.Frame(arrow.At(0.9, 1.5), true);
            check.Expect(rig.Last.GestureCancelled && SameTransformBits(rig.CubeTransform(), start) && SameTransformBits(ReadTransform(rig.Scene, rig.Cube2), other)
                    && rig.Host.History.EntryCount() == 0, "a selection change cancels, restores the original and leaves the other entity alone");

            rig.Host.Selected = rig.Cube;
            rig.Frame(arrow.At(0.9), false);
            rig.Frame(arrow.At(0.9), false);
            rig.Frame(arrow.At(0.9), true);
            rig.Frame(arrow.At(0.9, 1.0), true);
            rig.Scene.DestroyEntity(rig.Cube);
            rig.Frame(arrow.At(0.9, 1.5), true);
            check.Expect(rig.Last.GestureCancelled && rig.Host.Events.back().Kind == EventKind::Cancel && !rig.Host.History.GestureOpen()
                    && rig.Host.History.EntryCount() == 0 && rig.Last.Unavailable == GizmoUnavailable::NoSelection,
                "a deleted entity cancels: the restore is refused, the gesture still closes, nothing is recorded");
            rig.Frame(arrow.At(0.9, 1.5), false);
        }

        // Hostile pointer positions never produce a non-finite or non-canonical transform.
        {
            Rig rig;
            const Arrow arrow(rig, TransformTool::Translate, TransformSpace::World, 0);
            rig.Frame({ nan, nan }, false);
            check.Expect(rig.Last.Hover == GizmoHandle::None && !rig.Last.OwnsPointer, "a NaN pointer hovers nothing");
            rig.Frame({ nan, nan }, true);
            check.Expect(rig.Host.Events.empty(), "and cannot press");
            rig.Frame(arrow.At(0.9), false);
            rig.Frame(arrow.At(0.9), true);
            rig.Frame(arrow.At(0.9, 1.0), true);
            const GizmoTransform good = rig.CubeTransform();
            for (const ScreenPoint& hostile : { ScreenPoint { nan, 300.0 }, ScreenPoint { 400.0, nan }, ScreenPoint { inf, -inf }, ScreenPoint { 1e30, 1e30 },
                     ScreenPoint { -1e300, 1e300 }, ScreenPoint { 1e-300, 1e-300 } })
            {
                rig.Frame(hostile, true);
                const GizmoTransform now = rig.CubeTransform();
                check.Expect(IsFiniteTransform(now) && Engine::Math::IsCanonical(now.Position, rig.Policy) && rig.Last.Phase == Gizmo::GizmoPhase::Active,
                    "hostile pointer " + Fixed("%g", hostile.X) + "," + Fixed("%g", hostile.Y) + " keeps a finite canonical transform");
                for (const Gizmo::GizmoDrawPrimitive& primitive : rig.Last.Primitives)
                {
                    for (const ScreenPoint& point : primitive.Points)
                        check.Expect(std::isfinite(point.X) && std::isfinite(point.Y), "draw points stay finite");
                }
            }

            rig.Frame(arrow.At(0.9, 1.0), true);
            check.ExpectNear(rig.CubeTransform().Position.Local.X, good.Position.Local.X, 3e-4, "the drag recovers when the pointer returns");
            rig.Frame(arrow.At(0.9, 1.0), false);
            check.Expect(rig.Host.History.EntryCount() == 1, "one entry");
        }

        return check.Ok;
    }

    bool TestGizmoSessionDrawListUsesInjectedPalette()
    {
        Checker check { "palette" };
        const Gizmo::GizmoColor axisX { 1, 10, 10, 255 };
        const Gizmo::GizmoColor axisY { 10, 1, 10, 255 };
        const Gizmo::GizmoColor axisZ { 10, 10, 1, 255 };
        const Gizmo::GizmoColor centre { 20, 20, 20, 255 };
        const Gizmo::GizmoColor outline { 30, 30, 30, 200 };
        const Gizmo::GizmoColor hover { 40, 50, 60, 255 };
        const Gizmo::GizmoColor active { 70, 80, 90, 255 };
        const Gizmo::GizmoColor label { 100, 110, 120, 255 };
        Gizmo::GizmoPalette palette;
        palette.AxisX = axisX;
        palette.AxisY = axisY;
        palette.AxisZ = axisZ;
        palette.Center = centre;
        palette.Outline = outline;
        palette.Hover = hover;
        palette.Active = active;
        palette.Label = label;

        // Fills use the same hue at a lower alpha, so colours are compared by RGB.
        const auto sameHue = [](const Gizmo::GizmoColor& a, const Gizmo::GizmoColor& b) { return a.R == b.R && a.G == b.G && a.B == b.B; };
        const auto uses = [&](const GizmoSessionResult& result, const Gizmo::GizmoColor& color)
        {
            return std::any_of(result.Primitives.begin(), result.Primitives.end(),
                [&](const Gizmo::GizmoDrawPrimitive& primitive) { return sameHue(primitive.Color, color); });
        };
        const auto onlyHandleColours = [&](const GizmoSessionResult& result)
        {
            // Handles use the axis, centre and outline colours; the X/Y/Z letters are text in the label colour.
            return std::all_of(result.Primitives.begin(), result.Primitives.end(), [&](const Gizmo::GizmoDrawPrimitive& primitive)
            {
                const Gizmo::GizmoColor& c = primitive.Color;
                if (primitive.Kind == Gizmo::GizmoPrimitiveKind::Text)
                    return sameHue(c, label);
                return sameHue(c, axisX) || sameHue(c, axisY) || sameHue(c, axisZ) || sameHue(c, centre) || sameHue(c, outline);
            });
        };

        // Every tool draws only injected colours; hover and active use the selection tokens, nothing else does.
        for (const TransformTool tool : { TransformTool::Translate, TransformTool::Rotate, TransformTool::Scale })
        {
            Rig rig;
            rig.Session.SetPalette(palette);
            rig.Session.SetTool(tool);
            const TransformSpace space = TransformSpace::World;
            const Arrow arrow(rig, tool, space, 0);
            const Ring ring(rig, 0);
            const ScreenPoint onHandle = tool == TransformTool::Rotate ? ring.At(0.0) : arrow.At(0.9);

            rig.Frame({ 700.0, 60.0 }, false);
            check.Expect(!rig.Last.Primitives.empty() && onlyHandleColours(rig.Last) && uses(rig.Last, axisX) && uses(rig.Last, axisY) && uses(rig.Last, axisZ),
                "idle: all three axis colours and only handle colours");
            check.Expect(!uses(rig.Last, hover) && !uses(rig.Last, active), "idle: no selection-blue anywhere");

            rig.Frame(onHandle, false);
            check.Expect(rig.Last.Hover == GizmoHandle::AxisX && uses(rig.Last, hover) && !uses(rig.Last, active), "hover: the Hover token marks the handle");

            rig.Frame(onHandle, true);
            const ScreenPoint moved = tool == TransformTool::Rotate ? ring.At(25.0) : arrow.At(0.9, 0.8);
            rig.Frame(moved, true);
            check.Expect(rig.Last.Active == GizmoHandle::AxisX && uses(rig.Last, active) && !uses(rig.Last, hover), "drag: the Active token marks the handle");
            const bool readout = std::any_of(rig.Last.Primitives.begin(), rig.Last.Primitives.end(), [&](const Gizmo::GizmoDrawPrimitive& primitive)
            {
                return primitive.Kind == Gizmo::GizmoPrimitiveKind::Text && sameHue(primitive.Color, label) && !primitive.Text.empty();
            });
            check.Expect(readout, "drag: a numeric readout in the Label colour");
            bool finite = true;
            for (const Gizmo::GizmoDrawPrimitive& primitive : rig.Last.Primitives)
            {
                finite &= primitive.Thickness > 0.0f || primitive.Kind == Gizmo::GizmoPrimitiveKind::Text || primitive.Filled;
                for (const ScreenPoint& point : primitive.Points)
                    finite &= std::isfinite(point.X) && std::isfinite(point.Y);
            }
            check.Expect(finite, "drag: finite points and strokes");
            rig.Frame(moved, false);
        }

        // The Select tool has no axis colours anywhere.
        {
            Rig rig;
            rig.Session.SetPalette(palette);
            rig.Session.SetTool(TransformTool::Select);
            rig.Frame({ 400.0, 300.0 }, false);
            check.Expect(rig.Last.Primitives.empty(), "Select draws nothing");
        }

        // The DESIGN.md defaults.
        {
            GizmoSession session;
            const Gizmo::GizmoPalette& design = session.Palette();
            check.Expect(design.Hover == Gizmo::GizmoColor { 61, 97, 128, 255 }, "Hover is the Selection hover token rgba(61, 97, 128)");
            check.Expect(design.Active.R == 69 && design.Active.G == 133 && design.Active.B == 179 && design.Active.A == 255,
                "Active is the Docking preview colour rgb(69, 133, 179) at full alpha");

            // Hue (degrees) by the textbook HSV formula, independent of the implementation.
            const auto hue = [](const Gizmo::GizmoColor& c)
            {
                const double r = c.R / 255.0;
                const double g = c.G / 255.0;
                const double b = c.B / 255.0;
                const double high = std::max({ r, g, b });
                const double low = std::min({ r, g, b });
                const double delta = high - low;
                if (delta == 0.0)
                    return 0.0;
                double h = high == r ? std::fmod((g - b) / delta, 6.0) : high == g ? (b - r) / delta + 2.0 : (r - g) / delta + 4.0;
                h *= 60.0;
                return h < 0.0 ? h + 360.0 : h;
            };
            const auto hueGap = [&](const Gizmo::GizmoColor& a, const Gizmo::GizmoColor& b)
            {
                const double gap = std::abs(hue(a) - hue(b));
                return std::min(gap, 360.0 - gap);
            };
            for (const Gizmo::GizmoColor& axis : { design.AxisX, design.AxisY, design.AxisZ })
            {
                check.Expect(hueGap(axis, design.Hover) >= 40.0 && hueGap(axis, design.Active) >= 40.0,
                    "axis hue " + Fixed("%.0f", hue(axis)) + " is at least 40 degrees from the selection blues (" + Fixed("%.0f", hue(design.Hover)) + ", "
                        + Fixed("%.0f", hue(design.Active)) + ")");
            }

            check.Expect(hueGap(design.AxisX, design.AxisY) > 60.0 && hueGap(design.AxisY, design.AxisZ) > 60.0 && hueGap(design.AxisX, design.AxisZ) > 60.0,
                "the three axes are visibly different hues");
        }

        return check.Ok;
    }

    namespace
    {
        struct PropertyStats
        {
            size_t Frames = 0;
            size_t Begins = 0;
            size_t Ends = 0;
            size_t EntriesRecorded = 0;
            size_t Cancels = 0;
            size_t CancelsWithRestore = 0;
            size_t Refusals = 0;
            size_t BlockedPresses = 0;
            size_t RejectedWrites = 0;
        };

        const char* VerbForTool(TransformTool tool)
        {
            return tool == TransformTool::Rotate ? "Rotate" : tool == TransformTool::Scale ? "Scale" : "Move";
        }

        // One generated trace of pointer, key, command, camera, selection and
        // viewport events against a reference protocol model. Returns the first
        // violated invariant in `message`.
        bool RunGeneratedTrace(ChoiceStream& stream, std::string& message, PropertyStats& stats)
        {
            const double nan = std::numeric_limits<double>::quiet_NaN();
            const double inf = std::numeric_limits<double>::infinity();
            const auto chance = [&](size_t percent) { return stream.NextSize(0, 99) < percent; };
            // Disturbances (Esc, navigation, camera and selection changes) are rare in a calm trace and common in a chaotic one.
            const double calm[] = { 0.1, 0.4, 1.0 };
            const double chaos = calm[stream.NextSize(0, 2)];
            const auto rare = [&](double percent) { return static_cast<double>(stream.NextSize(0, 9999)) < percent * 100.0 * chaos; };
            const auto fail = [&](const std::string& text, size_t frame)
            {
                message = "frame " + std::to_string(frame) + ": " + text;
                return false;
            };

            // World: near the origin, far away, or right at the +E/2 sector boundary.
            const size_t world = stream.NextSize(0, 2);
            const Engine::Math::SectorIndex sector = world == 1 ? Engine::Math::SectorIndex { 987'654'321'000, -123'456'789'000, 555'555'555'000 }
                : Engine::Math::SectorIndex {};
            const DVec3 cameraLocal = world == 2 ? DVec3 { 2047.0, -4.25, 7.75 } : DVec3 { 10.5, -4.25, 7.75 };
            const Vec3 rotations[] = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 30.0f, 0.0f }, { 25.0f, -40.0f, 10.0f }, { 90.0f, 0.0f, 0.0f } };
            const Vec3 scales[] = { { 1.0f, 1.0f, 1.0f }, { 2.0f, 3.0f, 4.0f }, { 0.05f, 50.0f, 1.0f } };
            Rig rig(sector, rotations[stream.NextSize(0, 3)], scales[stream.NextSize(0, 2)],
                { static_cast<float>(RangeDouble(stream, -20.0, 20.0)), static_cast<float>(RangeDouble(stream, -40.0, 40.0)), 0.0f }, cameraLocal);
            rig.Session.SetSnapSettings({ chance(40), 4.0, 15.0, 0.1 });

            ViewportToolState model = rig.Session.State();
            bool down = false;
            bool open = false;
            u64 lastGestureId = 0;
            u64 openEntity = 0;
            TransformTool openTool = TransformTool::Translate;
            std::string openName;
            std::shared_ptr<const DocState> openDoc;
            size_t entriesAtOpen = 0;
            bool mustHoldLatch = false;
            ScreenPoint cursor { 400.0, 300.0 };
            const char* names[] = { "Cube", "Cube B", "X Y" };

            for (size_t frame = 0; frame < 90; ++frame)
            {
                ++stats.Frames;

                // ---- draw this frame's inputs (a fixed count of choices per frame) ----
                const size_t cursorMode = stream.NextSize(0, 9);
                const size_t candidate = stream.NextSize(0, 5);
                const double jitterX = RangeDouble(stream, -40.0, 40.0);
                const double jitterY = RangeDouble(stream, -40.0, 40.0);
                const double drift = RangeDouble(stream, -1.5, 1.5);
                const bool wantDown = down ? !chance(12) : chance(45);
                const bool ctrl = chance(12);
                const bool escape = rare(3);
                const bool secondary = rare(3);
                const bool middle = rare(2);
                const bool navigating = rare(2);
                const bool blocked = rare(2);
                const bool focused = !rare(1);
                const bool over = !rare(4);
                const size_t command = stream.NextSize(0, 99);
                const size_t commandKind = stream.NextSize(0, 5);
                const bool viaRegistry = chance(50);
                const size_t worldRoll = stream.NextSize(0, 99);
                const size_t world2 = rare(100) ? worldRoll : 99;
                const size_t selectionRoll = stream.NextSize(0, 99);
                const size_t selectionEvent = rare(100) ? selectionRoll : 99;
                const bool rejectWrites = rare(3);

                // ---- environment events ----
                if (world2 < 1)
                {
                    const Vec3 nudged { static_cast<float>(RangeDouble(stream, -20.0, 20.0)), static_cast<float>(RangeDouble(stream, -40.0, 40.0)), 0.0f };
                    rig.View.Build(rig.CameraPosition, rig.Policy, nudged, 60.0f, 0.1f, 2000.0f, rig.View.Rect);
                }
                else if (world2 < 2)
                {
                    const double width = RangeDouble(stream, 300.0, 1200.0);
                    rig.Input.Viewport = { 0.0, 0.0, width, RangeDouble(stream, 200.0, 900.0) };
                }
                else if (world2 < 3)
                {
                    rig.Input.Viewport = rig.View.Rect;
                }

                if (selectionEvent < 1)
                    rig.Host.HideSelection = !rig.Host.HideSelection;
                else if (selectionEvent < 2)
                    rig.Host.Selected = rig.Cube2;
                else if (selectionEvent < 3)
                    rig.Host.Selected = rig.Camera;
                else if (selectionEvent < 8)
                    rig.Host.Selected = rig.Cube;
                else if (selectionEvent < 9 && rig.Scene.IsEntityValid(rig.Cube2))
                    rig.Scene.DestroyEntity(rig.Cube2);
                else if (selectionEvent < 11)
                {
                    if (Engine::SceneEntity* entity = rig.Scene.TryGetEntity(rig.Cube))
                        entity->Name = names[selectionEvent % 3];
                }

                rig.Host.RejectApply = rejectWrites;

                // ---- tool state attempts ----
                const bool dragBefore = rig.Session.DragActive();
                if (dragBefore != open)
                    return fail("session drag state disagrees with the model before the frame", frame);
                if (command < 14)
                {
                    static constexpr const char* ids[] = { "viewport.tool.select", "viewport.tool.translate", "viewport.tool.rotate",
                        "viewport.tool.scale", "viewport.space.toggle", "viewport.snap.toggle" };
                    const ViewportToolState before = rig.Session.State();
                    ViewportToolState expected = before;
                    if (commandKind < 4)
                        expected.Tool = static_cast<TransformTool>(commandKind);
                    else if (commandKind == 4)
                        expected.Space = before.Space == TransformSpace::World ? TransformSpace::Local : TransformSpace::World;
                    else
                        expected.Snap.Enabled = !before.Snap.Enabled;

                    bool accepted = false;
                    if (viaRegistry)
                    {
                        const DispatchResult result = rig.Registry.Dispatch(ids[commandKind], CommandSource::Shortcut);
                        accepted = result.Executed();
                        if (!accepted && result.Status != DispatchStatus::Disabled)
                            return fail("a command was neither executed nor disabled: " + result.Reason, frame);
                    }
                    else if (commandKind < 4)
                        accepted = rig.Session.SetTool(expected.Tool).Ok();
                    else if (commandKind == 4)
                        accepted = rig.Session.ToggleSpace().Ok();
                    else
                        accepted = rig.Session.ToggleSnap().Ok();

                    if (accepted == dragBefore)
                        return fail(std::string("state change ") + (accepted ? "accepted" : "refused") + " with the drag " + (dragBefore ? "open" : "closed"), frame);
                    if (rig.Session.State() != (accepted ? expected : before))
                        return fail("the state after a change request is not the model's", frame);
                    if (!accepted)
                        ++stats.Refusals;
                    model = rig.Session.State();
                }
                else if (command < 16)
                {
                    // Invalid and valid snap settings: invalid never changes anything.
                    Gizmo::SnapSettings snap = rig.Session.State().Snap;
                    snap.RotateStepDegrees = chance(50) ? nan : 30.0;
                    const ViewportToolState before = rig.Session.State();
                    const ToolStateResult result = rig.Session.SetSnapSettings(snap);
                    if (std::isnan(snap.RotateStepDegrees) ? result.Status == ToolStateStatus::Applied : (result.Ok() == dragBefore))
                        return fail("snap settings request handled wrongly", frame);
                    if (!result.Ok() && rig.Session.State() != before)
                        return fail("a refused snap request changed the state", frame);
                    model = rig.Session.State();
                }

                if (model != rig.Session.State())
                    return fail("state drifted from the model", frame);

                // ---- the pointer ----
                const bool cubeThere = rig.Scene.IsEntityValid(rig.Cube);
                ScreenPoint target = cursor;
                if (cubeThere && rig.View.Camera.Valid)
                {
                    const TransformTool tool = rig.Session.State().Tool;
                    const TransformSpace space = rig.Session.State().Space;
                    const DVec3 pivot = rig.Pivot();
                    if (rig.View.Depth(pivot) > 0.3)
                    {
                        const int axis = static_cast<int>(candidate % 3);
                        const ScreenPoint handle = tool == TransformTool::Rotate ? Ring(rig, axis).At(0.0) : rig.OnArrow(tool, space, axis, 0.9);
                        const ScreenPoint centre = rig.View.Pixel(pivot);
                        const ScreenPoint choices[] = { handle, handle, { handle.X + drift * 30.0, handle.Y + drift * 30.0 }, centre,
                            { cursor.X + jitterX, cursor.Y + jitterY }, { cursor.X + drift, cursor.Y - drift } };
                        target = choices[candidate];
                    }
                }

                if (cursorMode == 0 && chance(40))
                    target = { RangeDouble(stream, -100.0, 1000.0), RangeDouble(stream, -100.0, 800.0) };
                else if (cursorMode == 1 && chance(8))
                    target = { nan, drift };
                else if (cursorMode == 2 && chance(5))
                    target = { inf, -inf };
                else if (cursorMode == 3 && chance(5))
                    target = { 1e30, -1e30 };
                if (std::isfinite(target.X) && std::isfinite(target.Y))
                    cursor = target;

                rig.Input.NavigationActive = navigating;
                rig.Input.InputBlocked = blocked;
                rig.Input.WindowFocused = focused;
                rig.Input.Pointer.SecondaryDown = secondary;
                rig.Input.Pointer.MiddleDown = middle;

                // ---- update ----
                const bool pressEdge = wantDown && !down;
                const bool selectionPresent = !rig.Host.HideSelection && rig.Scene.IsEntityValid(rig.Host.Selected);
                const bool selectedIsCamera = selectionPresent && rig.Host.Selected == rig.Camera;
                const std::string selectedName = selectionPresent ? rig.Scene.TryGetEntity(rig.Host.Selected)->Name : std::string();
                const auto docBefore = rig.Host.Capture();
                const size_t entriesBefore = rig.Host.History.EntryCount();
                const size_t eventsBefore = rig.Host.Events.size();
                const bool eligible = over && !navigating && !blocked && !secondary && !middle && focused
                    && std::isfinite(target.X) && std::isfinite(target.Y);
                const TransformTool tool = rig.Session.State().Tool;

                down = wantDown;
                rig.Frame(target, down, ctrl, escape, over);
                const GizmoSessionResult& result = rig.Last;

                // ---- the call grammar ----
                bool openNow = open;
                bool began = false;
                bool ended = false;
                bool cancelled = false;
                size_t applies = 0;
                for (size_t index = eventsBefore; index < rig.Host.Events.size(); ++index)
                {
                    const HostEvent& event = rig.Host.Events[index];
                    switch (event.Kind)
                    {
                    case EventKind::Begin:
                        if (openNow)
                            return fail("Begin while a gesture is open", frame);
                        if (event.Key.GestureId <= lastGestureId)
                            return fail("gesture ids do not increase", frame);
                        lastGestureId = event.Key.GestureId;
                        openEntity = event.Key.EntityId;
                        openNow = true;
                        began = true;
                        openTool = tool;
                        openName = selectedName;
                        openDoc = docBefore;
                        entriesAtOpen = entriesBefore;
                        if (event.Label != std::string(VerbForTool(tool)) + " " + selectedName)
                            return fail("label '" + event.Label + "' is not '" + VerbForTool(tool) + " " + selectedName + "'", frame);
                        break;
                    case EventKind::Apply:
                    case EventKind::ApplyRejected:
                    {
                        if (!openNow)
                            return fail("a write outside a gesture", frame);
                        ++applies;
                        const GizmoTransform* startOfGesture = nullptr;
                        for (const auto& [id, transform] : openDoc->Transforms)
                        {
                            if (id == event.Key.EntityId)
                                startOfGesture = &transform;
                        }

                        if (startOfGesture)
                        {
                            const bool sameRotation = SameBits(event.Transform.RotationDegrees, startOfGesture->RotationDegrees);
                            const bool sameScale = SameBits(event.Transform.Scale, startOfGesture->Scale);
                            const bool samePosition = SamePositionBits(event.Transform.Position, startOfGesture->Position);
                            // A restore on cancel is the start itself and satisfies every row.
                            if (openTool == TransformTool::Translate && !(sameRotation && sameScale))
                                return fail("a move changed rotation or scale", frame);
                            if (openTool == TransformTool::Rotate && !(samePosition && sameScale))
                                return fail("a rotate changed position or scale", frame);
                            if (openTool == TransformTool::Scale && !(samePosition && sameRotation))
                                return fail("a scale changed position or rotation", frame);
                            if (!IsFiniteTransform(event.Transform))
                                return fail("a non-finite transform was written", frame);
                        }
                        break;
                    }
                    case EventKind::End:
                    case EventKind::Cancel:
                        if (!openNow)
                            return fail("End or Cancel outside a gesture", frame);
                        openNow = false;
                        (event.Kind == EventKind::End ? ended : cancelled) = true;
                        break;
                    }
                }

                open = openNow;
                if (open != rig.Session.DragActive() || open != rig.Host.History.GestureOpen())
                    return fail("the open gesture disagrees across model, session and history", frame);
                if (began)
                {
                    ++stats.Begins;
                    if (!(pressEdge && eligible && selectionPresent && tool != TransformTool::Select && !(tool == TransformTool::Scale && selectedIsCamera)))
                        return fail("a gesture began while ineligible (press edge, eligible pointer, selection, tool)", frame);
                    if (!result.GestureBegan)
                        return fail("the result does not report the Begin", frame);
                }
                else if (result.GestureBegan)
                {
                    return fail("the result reports a Begin that did not happen", frame);
                }

                if (dragBefore && (navigating || blocked || !focused) && !cancelled)
                    return fail("navigation, a blocking state or focus loss did not end the open drag", frame);
                if (!eligible && !dragBefore && (result.Hover != GizmoHandle::None || began))
                    return fail("hover or Begin while the pointer was ineligible", frame);
                if (!eligible && pressEdge && !dragBefore)
                    ++stats.BlockedPresses;
                if (result.GestureEnded != ended || result.GestureCancelled != cancelled)
                    return fail("the result's end/cancel flags disagree with the host calls", frame);

                // ---- end and cancel semantics ----
                if (ended)
                {
                    ++stats.Ends;
                    const auto now = rig.Host.Capture();
                    const bool changed = !rig.Host.Adapter.Equal(*openDoc, *now);
                    const size_t expectedEntries = entriesAtOpen + (changed ? 1 : 0);
                    if (rig.Host.History.EntryCount() != expectedEntries)
                        return fail("entries " + std::to_string(rig.Host.History.EntryCount()) + " after a drag that " + (changed ? "changed" : "did not change")
                                + " the document (expected " + std::to_string(expectedEntries) + ")", frame);
                    if (changed)
                    {
                        ++stats.EntriesRecorded;
                        const std::string expectedLabel = std::string(VerbForTool(openTool)) + " " + openName;
                        if (rig.Host.History.TopUndo()->Label.Display() != expectedLabel)
                            return fail("recorded label '" + rig.Host.History.TopUndo()->Label.Display() + "' is not '" + expectedLabel + "'", frame);
                    }
                }

                if (cancelled)
                {
                    ++stats.Cancels;
                    if (rig.Host.History.EntryCount() != entriesAtOpen)
                        return fail("a cancel recorded an entry", frame);
                    const HostEvent& cancel = rig.Host.Events.back();
                    const bool entityExists = rig.Scene.IsEntityValid({ static_cast<Engine::EntityId>(cancel.Key.EntityId) });
                    const GizmoTransform* startOfGesture = nullptr;
                    for (const auto& [id, transform] : openDoc->Transforms)
                    {
                        if (id == cancel.Key.EntityId)
                            startOfGesture = &transform;
                    }

                    if (entityExists && startOfGesture && !rig.Host.RejectApply)
                    {
                        ++stats.CancelsWithRestore;
                        if (!SameTransformBits(cancel.Transform, *startOfGesture))
                            return fail("the gizmo did not restore the start transform before CancelGesture", frame);
                    }
                }

                // ---- scene sanity: only the gesture's entity may differ from the document at open ----
                if (open)
                {
                    const auto now = rig.Host.Capture();
                    for (size_t index = 0; index < openDoc->Transforms.size(); ++index)
                    {
                        const auto& [id, before] = openDoc->Transforms[index];
                        if (id == openEntity)
                            continue;
                        for (const auto& [nowId, nowTransform] : now->Transforms)
                        {
                            if (nowId == id && !SameTransformBits(nowTransform, before))
                                return fail("an entity other than the gesture's changed during the drag", frame);
                        }
                    }
                }

                for (const Engine::SceneEntity& entity : rig.Scene.GetEntities())
                {
                    const GizmoTransform stored = ReadTransform(rig.Scene, entity.EntityHandle);
                    if (!IsFiniteTransform(stored) || !Engine::Math::IsCanonical(stored.Position, rig.Policy))
                        return fail("a stored transform is not finite and canonical", frame);
                }

                // ---- pointer ownership latch ----
                if (pressEdge)
                {
                    if (began)
                        mustHoldLatch = true;
                    else if (result.PressStartedOnGizmo)
                    {
                        if (result.Hover == GizmoHandle::None && result.Active == GizmoHandle::None)
                            return fail("a press claimed the gizmo without a handle under it", frame);
                        mustHoldLatch = true;
                    }
                    else
                        mustHoldLatch = false;
                }

                if (result.PressStartedOnGizmo != mustHoldLatch)
                    return fail(std::string("PressStartedOnGizmo is ") + (result.PressStartedOnGizmo ? "set" : "clear") + " but the model says "
                            + (mustHoldLatch ? "set" : "clear"), frame);
                if (!down)
                    mustHoldLatch = false;
                if ((result.Hover != GizmoHandle::None || result.Active != GizmoHandle::None || result.PressStartedOnGizmo) && !result.OwnsPointer)
                    return fail("hover, active or a gizmo press without OwnsPointer", frame);
                if (result.OwnsPointer && result.Hover == GizmoHandle::None && result.Active == GizmoHandle::None && !result.PressStartedOnGizmo)
                    return fail("OwnsPointer without a reason", frame);

                // ---- presentation ----
                if (result.Phase == Gizmo::GizmoPhase::Active)
                {
                    if (result.Cursor != GizmoCursor::Grabbing || result.Status.rfind(VerbForTool(openTool), 0) != 0 || !open)
                        return fail("an active drag has the wrong cursor or status: '" + result.Status + "'", frame);
                }
                else if (result.Hover != GizmoHandle::None)
                {
                    if (result.Cursor != GizmoCursor::Hand || result.Status.empty())
                        return fail("hover without hand cursor and status", frame);
                }
                else if (result.Cursor != GizmoCursor::Default || !result.Status.empty())
                {
                    return fail("idle with a cursor or status", frame);
                }

                for (const Gizmo::GizmoDrawPrimitive& primitive : result.Primitives)
                {
                    for (const ScreenPoint& point : primitive.Points)
                    {
                        if (!std::isfinite(point.X) || !std::isfinite(point.Y))
                            return fail("a non-finite draw point", frame);
                    }
                }

                stats.RejectedWrites = static_cast<size_t>(rig.Session.RejectedUpdateCount());
                (void)applies;
            }

            // Every trace ends with a released button, so nothing may remain open.
            rig.Input.Pointer.SecondaryDown = rig.Input.Pointer.MiddleDown = false;
            rig.Input.NavigationActive = rig.Input.InputBlocked = false;
            rig.Input.WindowFocused = true;
            rig.Frame(cursor, false);
            if (rig.Session.DragActive() || rig.Host.History.GestureOpen())
                return fail("a gesture is still open after the final release", 90);
            return true;
        }
    }

    bool TestGizmoSessionGeneratedTracesKeepInvariants()
    {
        Checker check { "generated" };
        PropertyStats stats;
        const bool ok = Spiral::Tests::RunNamedProperty("gizmo-session", "TestGizmoSessionGeneratedTracesKeepInvariants", "SPIRAL_GIZMO_SESSION", 250,
            [&](ChoiceStream& stream, std::string& message) { return RunGeneratedTrace(stream, message, stats); });
        check.Expect(ok, "a generated trace violated a session invariant (see the counterexample above)");

        // A generator that never reaches the interesting states proves nothing: require coverage unless one trace is being replayed
        // (the floors are half the minimum observed over 60 seeds).
        if (ok && !std::getenv("SPIRAL_GIZMO_SESSION_REPLAY"))
        {
            std::cout << "gizmo session generated coverage: frames=" << stats.Frames << " begins=" << stats.Begins << " ends=" << stats.Ends
                << " entries=" << stats.EntriesRecorded << " cancels=" << stats.Cancels << " restoredCancels=" << stats.CancelsWithRestore
                << " refusedStateChanges=" << stats.Refusals << " blockedPresses=" << stats.BlockedPresses << '\n';
            check.Expect(stats.Begins >= 150 && stats.Ends >= 80 && stats.EntriesRecorded >= 70 && stats.Cancels >= 20 && stats.CancelsWithRestore >= 20
                    && stats.Refusals >= 80 && stats.BlockedPresses >= 5,
                "the generated traces must reach drags, entries, cancels, refused changes and blocked presses");
        }

        return check.Ok;
    }
}
