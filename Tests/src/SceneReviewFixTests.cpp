#include "SceneReviewFixTests.h"

#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
    using namespace Engine;
    using Clock = std::chrono::steady_clock;

    bool Check(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "Scene review-fix test failed: " << message << '\n';
        return condition;
    }

    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    constexpr float kInf = std::numeric_limits<float>::infinity();

    // A unique directory that removes itself.
    class TempDirectory
    {
    public:
        TempDirectory()
        {
            static std::atomic<std::uint64_t> sequence { 0 };
            m_Path = std::filesystem::temp_directory_path()
                / ("SceneReviewFix-" + std::to_string(static_cast<std::uint64_t>(Clock::now().time_since_epoch().count()))
                    + "-" + std::to_string(sequence.fetch_add(1)));
            std::error_code error;
            std::filesystem::create_directories(m_Path, error);
        }

        ~TempDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(m_Path, error);
        }

        TempDirectory(const TempDirectory&) = delete;
        TempDirectory& operator=(const TempDirectory&) = delete;

        std::filesystem::path File(std::string_view name) const { return m_Path / std::string(name); }

    private:
        std::filesystem::path m_Path;
    };

    void WriteText(const std::filesystem::path& path, std::string_view text)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    }

    // A version-5 scene with the default world grid and the supplied entity
    // section. The camera entity is entity 1.
    std::string SceneText(std::string_view entityRecords, std::string_view nextEntityId = "10",
        std::string_view mainCameraEntity = "1", std::string_view mainCameraFields = {})
    {
        std::ostringstream text;
        text << "SpiralScene 5\nName \"Review\"\n\n"
            << "[WorldGrid]\nVersion 1\nSectorExtent 4096\nOriginHysteresis 1024\nOriginMode ExactCamera\n\n"
            << "[MainCamera]\nPrimary true\n"
            << (mainCameraFields.empty() ? std::string_view("VerticalFovDegrees 60\n") : mainCameraFields)
            << "NearClip 0.1\nFarClip 100\nBackgroundColor 0.1 0.2 0.3\n\n"
            << "[Entities]\nNextEntityId " << nextEntityId << "\nMainCameraEntity " << mainCameraEntity << "\n"
            << entityRecords;
        return text.str();
    }

    constexpr std::string_view kValidCameraEntity =
        "Entity 1 \"Camera\"\n"
        "Transform 1 0 0 0 0 0 -3 0 0 0 1 1 1\n"
        "Camera 1 true 60 0.1 100 0.1 0.2 0.3\n";

    // True when loading `text` is rejected and the destination Scene is exactly
    // what it was before (name and entity count of a sentinel).
    bool LoadIsRejectedAtomically(const TempDirectory& directory, std::string_view label, const std::string& text)
    {
        const std::filesystem::path path = directory.File("candidate.scene");
        WriteText(path, text);
        Scene destination("Sentinel");
        destination.CreateEntity("Keep");
        const size_t entities = destination.GetEntities().size();
        const bool loaded = Scene::LoadFromFile(path, destination);
        return Check(!loaded, std::string(label) + " must be rejected")
            && Check(destination.GetName() == "Sentinel" && destination.GetEntities().size() == entities,
                std::string(label) + " must leave the destination untouched");
    }

    bool SameEntity(const SceneEntity& a, const SceneEntity& b)
    {
        const auto sameVec = [](const Math::Vec3& x, const Math::Vec3& y)
        {
            return x.X == y.X && x.Y == y.Y && x.Z == y.Z;
        };
        const Math::SectorLocalPosition& pa = a.Transform.GetPosition();
        const Math::SectorLocalPosition& pb = b.Transform.GetPosition();
        if (!(a.EntityHandle == b.EntityHandle) || a.Name != b.Name
            || !(pa.Sector == pb.Sector)
            || pa.Local.X != pb.Local.X || pa.Local.Y != pb.Local.Y || pa.Local.Z != pb.Local.Z
            || !sameVec(a.Transform.RotationDegrees, b.Transform.RotationDegrees)
            || !sameVec(a.Transform.Scale, b.Transform.Scale)
            || a.Camera.has_value() != b.Camera.has_value()
            || a.Light.has_value() != b.Light.has_value()
            || a.MeshRenderer.has_value() != b.MeshRenderer.has_value())
        {
            return false;
        }
        if (a.Camera)
        {
            if (a.Camera->Primary != b.Camera->Primary
                || a.Camera->Projection.VerticalFovDegrees != b.Camera->Projection.VerticalFovDegrees
                || a.Camera->Projection.NearClip != b.Camera->Projection.NearClip
                || a.Camera->Projection.FarClip != b.Camera->Projection.FarClip
                || !sameVec(a.Camera->BackgroundColor, b.Camera->BackgroundColor))
                return false;
        }
        if (a.Light)
        {
            if (a.Light->Type != b.Light->Type || !sameVec(a.Light->Color, b.Light->Color)
                || a.Light->PhotometricValue != b.Light->PhotometricValue
                || a.Light->PhotometricUnit != b.Light->PhotometricUnit
                || a.Light->Range != b.Light->Range
                || a.Light->InnerConeDegrees != b.Light->InnerConeDegrees
                || a.Light->OuterConeDegrees != b.Light->OuterConeDegrees
                || a.Light->CastsShadows != b.Light->CastsShadows)
                return false;
        }
        if (a.MeshRenderer)
        {
            if (a.MeshRenderer->MeshAsset != b.MeshRenderer->MeshAsset
                || a.MeshRenderer->MaterialAsset != b.MeshRenderer->MaterialAsset
                || a.MeshRenderer->MeshName != b.MeshRenderer->MeshName
                || a.MeshRenderer->Visible != b.MeshRenderer->Visible
                || a.MeshRenderer->CastsShadows != b.MeshRenderer->CastsShadows)
                return false;
        }
        return true;
    }

    // Every invariant Scene enforces on authoring, checked from the outside.
    bool EntitySatisfiesInvariants(const SceneEntity& entity)
    {
        const Math::Vec3& scale = entity.Transform.Scale;
        const Math::Vec3& rotation = entity.Transform.RotationDegrees;
        constexpr float smallest = std::numeric_limits<float>::min();
        const bool transformOk = std::isfinite(rotation.X) && std::isfinite(rotation.Y) && std::isfinite(rotation.Z)
            && scale.X >= smallest && scale.Y >= smallest && scale.Z >= smallest
            && std::isfinite(scale.X) && std::isfinite(scale.Y) && std::isfinite(scale.Z)
            && std::isfinite(1.0f / scale.X) && std::isfinite(1.0f / scale.Y) && std::isfinite(1.0f / scale.Z);
        const bool cameraOk = !entity.Camera
            || (scale.X == 1.0f && scale.Y == 1.0f && scale.Z == 1.0f && IsValidCameraComponent(*entity.Camera));
        return transformOk && cameraOk && (!entity.Light || IsValidLightComponent(*entity.Light));
    }
}

namespace SpiralTests
{
    // Behavior: LoadFromFile enforces the transform, camera and id invariants
    // SetEntityTransform, AddCameraComponent and RestoreEntity enforce, whatever
    // order the records arrive in, and leaves its destination untouched when it
    // rejects. Ids and counters are plain unsigned decimals (no "-1" wrap).
    // Oracle: hostile inputs taken from the review (zero/negative/denormal scale,
    // a camera that precedes its non-unit Transform, FOV 0/180, negative ids), the
    // independent EntitySatisfiesInvariants predicate on accepted input, and
    // valid controls that must still load.
    bool TestSceneLoadRejectsUnrenderableTransformsCamerasAndWrappedIds()
    {
        TempDirectory directory;
        bool ok = true;

        const auto withMesh = [](std::string_view transformFields)
        {
            return std::string(kValidCameraEntity)
                + "Entity 2 \"Mesh\"\nTransform 2 0 0 0 0 0 0 " + std::string(transformFields) + "\n";
        };
        ok &= LoadIsRejectedAtomically(directory, "zero and negative scale", SceneText(withMesh("0 0 0 0 -1 0")));
        ok &= LoadIsRejectedAtomically(directory, "zero scale", SceneText(withMesh("0 0 0 0 0 0")));
        ok &= LoadIsRejectedAtomically(directory, "denormal scale (its reciprocal overflows)", SceneText(withMesh("0 0 0 1e-39 1 1")));
        ok &= LoadIsRejectedAtomically(directory, "non-finite rotation", SceneText(withMesh("nan 0 0 1 1 1")));
        ok &= LoadIsRejectedAtomically(directory, "infinite rotation", SceneText(withMesh("inf 0 0 1 1 1")));
        ok &= LoadIsRejectedAtomically(directory, "camera before its non-unit-scale Transform",
            SceneText("Entity 1 \"Camera\"\nCamera 1 true 60 0.1 100 0.1 0.2 0.3\nTransform 1 0 0 0 0 0 0 0 0 0 5 5 5\n"));
        ok &= LoadIsRejectedAtomically(directory, "camera after its non-unit-scale Transform",
            SceneText("Entity 1 \"Camera\"\nTransform 1 0 0 0 0 0 0 0 0 0 5 5 5\nCamera 1 true 60 0.1 100 0.1 0.2 0.3\n"));

        for (const char* projection : { "0 0.1 100", "180 0.1 100", "-10 0.1 100", "60 0 100", "60 -1 100", "60 5 5", "60 5 1",
                 "nan 0.1 100", "60 0.1 inf", "1e-45 0.1 100" })
        {
            ok &= LoadIsRejectedAtomically(directory, std::string("camera projection ") + projection,
                SceneText("Entity 1 \"Camera\"\nTransform 1 0 0 0 0 0 0 0 0 0 1 1 1\nCamera 1 true " + std::string(projection)
                    + " 0.1 0.2 0.3\n"));
        }
        ok &= LoadIsRejectedAtomically(directory, "non-finite camera background",
            SceneText("Entity 1 \"Camera\"\nTransform 1 0 0 0 0 0 0 0 0 0 1 1 1\nCamera 1 true 60 0.1 100 nan 0.2 0.3\n"));
        ok &= LoadIsRejectedAtomically(directory, "invalid [MainCamera] values with no main camera entity",
            SceneText("Entity 3 \"Plain\"\nTransform 3 0 0 0 0 0 0 0 0 0 1 1 1\n", "10", "0", "VerticalFovDegrees 0\n"));

        // Wrapped ids: extraction into an unsigned type accepts "-1".
        ok &= LoadIsRejectedAtomically(directory, "negative NextEntityId", SceneText(kValidCameraEntity, "-1"));
        ok &= LoadIsRejectedAtomically(directory, "negative entity id", SceneText("Entity -1 \"Bad\"\n"));
        ok &= LoadIsRejectedAtomically(directory, "negative Transform id",
            SceneText(std::string(kValidCameraEntity) + "Transform -1 0 0 0 0 0 0 0 0 0 1 1 1\n"));
        ok &= LoadIsRejectedAtomically(directory, "negative MainCameraEntity", SceneText(kValidCameraEntity, "10", "-1"));
        ok &= LoadIsRejectedAtomically(directory, "entity id beyond 32 bits", SceneText("Entity 4294967296 \"Bad\"\n"));
        ok &= LoadIsRejectedAtomically(directory, "non-decimal entity id", SceneText("Entity 0x10 \"Bad\"\n"));
        ok &= LoadIsRejectedAtomically(directory, "a line longer than the loader's bound",
            SceneText(std::string(kValidCameraEntity) + "Entity 2 \"" + std::string(2 * 1024 * 1024, 'x') + "\"\n"));

        // Valid controls must still load and satisfy the invariants.
        const std::filesystem::path controlPath = directory.File("control.scene");
        WriteText(controlPath, SceneText(std::string(kValidCameraEntity)
            + "Entity 2 \"Mesh\"\nTransform 2 0 0 0 1 2 3 10 20 30 0.5 2 1e-30\n"));
        Scene control;
        ok &= Check(Scene::LoadFromFile(controlPath, control), "the valid control scene loads");
        for (const SceneEntity& entity : control.GetEntities())
            ok &= Check(EntitySatisfiesInvariants(entity), "every loaded entity satisfies the invariants");
        return ok;
    }

    // Behavior: SaveToFile refuses, before touching the destination, any scene
    // the loader would not accept; the previous file survives byte for byte.
    // Oracle: the bytes of the last good file, and a successful save after the
    // bad value is repaired (so the refusal was about that value alone).
    bool TestSceneSaveRefusesEverythingTheLoaderRejectsAndKeepsTheLastGoodFile()
    {
        TempDirectory directory;
        const std::filesystem::path path = directory.File("scene.scene");
        bool ok = true;

        Scene scene("Saved");
        const Entity mesh = scene.CreateEntity("Mesh");
        scene.AddMeshRendererComponent(mesh, {});
        const Entity lamp = scene.CreateEntity("Lamp");
        scene.AddLightComponent(lamp, {});
        const Entity second = scene.CreateEntity("Second Camera");
        CameraComponent camera;
        camera.Primary = false;
        scene.AddCameraComponent(second, camera);
        ok &= Check(scene.SaveToFile(path), "the healthy scene saves");
        std::string lastGood = ReadText(path);

        struct Case
        {
            const char* Label;
            void (*Break)(Scene&, Entity mesh, Entity camera);
            void (*Repair)(Scene&, Entity mesh, Entity camera);
        };
        const Case cases[] = {
            { "NaN rotation",
                [](Scene& s, Entity m, Entity) { s.TryGetTransform(m)->RotationDegrees.X = kNaN; },
                [](Scene& s, Entity m, Entity) { s.TryGetTransform(m)->RotationDegrees.X = 0.0f; } },
            { "infinite rotation",
                [](Scene& s, Entity m, Entity) { s.TryGetTransform(m)->RotationDegrees.Y = kInf; },
                [](Scene& s, Entity m, Entity) { s.TryGetTransform(m)->RotationDegrees.Y = 0.0f; } },
            { "zero scale",
                [](Scene& s, Entity m, Entity) { s.TryGetTransform(m)->Scale.Z = 0.0f; },
                [](Scene& s, Entity m, Entity) { s.TryGetTransform(m)->Scale.Z = 1.0f; } },
            { "denormal scale",
                [](Scene& s, Entity m, Entity) { s.TryGetTransform(m)->Scale.X = 1e-39f; },
                [](Scene& s, Entity m, Entity) { s.TryGetTransform(m)->Scale.X = 1.0f; } },
            { "NaN scale",
                [](Scene& s, Entity m, Entity) { s.TryGetTransform(m)->Scale.Y = kNaN; },
                [](Scene& s, Entity m, Entity) { s.TryGetTransform(m)->Scale.Y = 1.0f; } },
            { "non-unit camera scale",
                [](Scene& s, Entity, Entity c) { s.TryGetTransform(c)->Scale = { 2.0f, 2.0f, 2.0f }; },
                [](Scene& s, Entity, Entity c) { s.TryGetTransform(c)->Scale = { 1.0f, 1.0f, 1.0f }; } },
            { "zero field of view",
                [](Scene& s, Entity, Entity c) { s.TryGetCameraComponent(c)->Projection.VerticalFovDegrees = 0.0f; },
                [](Scene& s, Entity, Entity c) { s.TryGetCameraComponent(c)->Projection.VerticalFovDegrees = 60.0f; } },
            { "NaN far clip",
                [](Scene& s, Entity, Entity c) { s.TryGetCameraComponent(c)->Projection.FarClip = kNaN; },
                [](Scene& s, Entity, Entity c) { s.TryGetCameraComponent(c)->Projection.FarClip = 100.0f; } },
            { "NaN camera background",
                [](Scene& s, Entity, Entity c) { s.TryGetCameraComponent(c)->BackgroundColor.Y = kNaN; },
                [](Scene& s, Entity, Entity c) { s.TryGetCameraComponent(c)->BackgroundColor.Y = 0.5f; } },
            { "newline in an entity name",
                [](Scene& s, Entity m, Entity) { s.TryGetEntity(m)->Name = "Mesh\nEntity 99 \"Injected\""; },
                [](Scene& s, Entity m, Entity) { s.TryGetEntity(m)->Name = "Mesh"; } },
            { "carriage return in a mesh name",
                [](Scene& s, Entity m, Entity) { s.TryGetMeshRendererComponent(m)->MeshName = "a\rb"; },
                [](Scene& s, Entity m, Entity) { s.TryGetMeshRendererComponent(m)->MeshName = "ab"; } },
        };
        for (const Case& item : cases)
        {
            item.Break(scene, mesh, second);
            ok &= Check(!scene.SaveToFile(path), std::string("saving a scene with ") + item.Label + " must fail");
            ok &= Check(ReadText(path) == lastGood, std::string(item.Label) + " must leave the last good file untouched");
            item.Repair(scene, mesh, second);
            ok &= Check(scene.SaveToFile(path), std::string("saving after repairing ") + item.Label);
            lastGood = ReadText(path);
            Scene reloaded;
            ok &= Check(Scene::LoadFromFile(path, reloaded), std::string("the file saved after repairing ") + item.Label + " loads");
        }

        Scene newlineName("two\nlines");
        ok &= Check(!newlineName.SaveToFile(directory.File("name.scene")), "a scene name with a line break cannot be saved");
        return ok;
    }

    // Behavior: a camera projection that cannot produce finite, orientation
    // preserving matrices is rejected by every producer, and BuildCameraView never
    // marks such a view valid.
    // Oracle: the independent projection arithmetic (finite elements, positive
    // focal scales) on the views that are produced.
    bool TestCameraProjectionValidationReachesEveryProducer()
    {
        bool ok = true;
        const auto projection = [](float fov, float nearClip, float farClip)
        {
            CameraProjection result;
            result.VerticalFovDegrees = fov;
            result.NearClip = nearClip;
            result.FarClip = farClip;
            return result;
        };
        ok &= Check(IsValidCameraProjection(projection(60.0f, 0.1f, 100.0f)), "the default projection is valid");
        ok &= Check(IsValidCameraProjection(projection(0.5f, 0.01f, 1.0e6f)) && IsValidCameraProjection(projection(179.0f, 1.0f, 2.0f)),
            "extreme but finite projections are valid");
        for (const CameraProjection& bad : { projection(0.0f, 0.1f, 100.0f), projection(180.0f, 0.1f, 100.0f),
                 projection(-1.0f, 0.1f, 100.0f), projection(200.0f, 0.1f, 100.0f), projection(kNaN, 0.1f, 100.0f),
                 projection(60.0f, 0.0f, 100.0f), projection(60.0f, -1.0f, 100.0f), projection(60.0f, 5.0f, 5.0f),
                 projection(60.0f, 5.0f, 1.0f), projection(60.0f, 0.1f, kInf), projection(60.0f, kNaN, 100.0f),
                 projection(1.0e-45f, 0.1f, 100.0f) })
        {
            ok &= Check(!IsValidCameraProjection(bad),
                "projection " + std::to_string(bad.VerticalFovDegrees) + "/" + std::to_string(bad.NearClip)
                    + "/" + std::to_string(bad.FarClip) + " is rejected");
            const CameraView view = BuildCameraView({}, {}, bad, 16.0f / 9.0f, {});
            ok &= Check(!view.Valid, "BuildCameraView never marks a view with an invalid projection valid");
        }

        const CameraView good = BuildCameraView({}, {}, projection(60.0f, 0.1f, 100.0f), 16.0f / 9.0f, {});
        ok &= Check(good.Valid, "a valid projection produces a valid view");
        bool finite = true;
        for (const float value : good.Projection.Values)
            finite &= std::isfinite(value);
        ok &= Check(finite && good.Projection.Values[0] > 0.0f && good.Projection.Values[5] > 0.0f,
            "the produced projection is finite with positive focal scales");

        Scene scene;
        const Entity entity = scene.CreateEntity("Camera");
        CameraComponent invalid;
        invalid.Projection.VerticalFovDegrees = 0.0f;
        invalid.Projection.NearClip = -1.0f;
        invalid.Projection.FarClip = -5.0f;
        ok &= Check(scene.AddCameraComponent(entity, invalid) == nullptr && !scene.TryGetCameraComponent(entity),
            "AddCameraComponent rejects an invalid camera and changes nothing");
        const CameraComponent before = scene.GetMainCamera();
        ok &= Check(!scene.SetMainCamera(invalid) && scene.GetMainCamera().Projection.VerticalFovDegrees == before.Projection.VerticalFovDegrees,
            "SetMainCamera rejects an invalid camera and keeps the cached one");
        CameraComponent tweaked = before;
        tweaked.Projection.VerticalFovDegrees = 45.0f;
        ok &= Check(scene.SetMainCamera(tweaked) && scene.GetMainCamera().Projection.VerticalFovDegrees == 45.0f,
            "SetMainCamera accepts a valid camera");

        SceneEntity restored;
        restored.EntityHandle = Entity { 500 };
        restored.Name = "Restored";
        restored.Camera = invalid;
        ok &= Check(!scene.RestoreEntity(restored), "RestoreEntity rejects an invalid camera");

        const Entity mesh = scene.CreateEntity("Mesh");
        ok &= Check(!scene.SetEntityTransform(mesh, {}, {}, { 1e-39f, 1.0f, 1.0f }),
            "SetEntityTransform rejects a denormal scale whose reciprocal overflows");
        ok &= Check(scene.SetEntityTransform(mesh, {}, {}, { std::numeric_limits<float>::min(), 1.0f, 1.0f }),
            "SetEntityTransform accepts the smallest normal scale");
        return ok;
    }

    // Behavior: removing the main camera's component leaves the Scene without a
    // main camera, and that saved state reloads as saved instead of silently
    // promoting another camera.
    // Oracle: the designation before Save versus after Load, with a second
    // camera present that a promoting loader would pick.
    bool TestSceneMainCameraAbsenceSurvivesSaveAndReload()
    {
        TempDirectory directory;
        const std::filesystem::path path = directory.File("main.scene");
        bool ok = true;

        Scene scene;
        const Entity original = scene.GetMainCameraEntity();
        const Entity other = scene.CreateEntity("Other Camera");
        CameraComponent otherCamera;
        otherCamera.Projection.VerticalFovDegrees = 33.0f;
        otherCamera.Primary = true;
        ok &= Check(scene.AddCameraComponent(other, otherCamera) != nullptr, "second camera added");
        ok &= Check(scene.GetMainCameraEntity() == original, "the existing main camera is kept");

        ok &= Check(scene.RemoveCameraComponent(original) && !scene.GetMainCameraEntity().IsValid(),
            "removing the main camera's component leaves no main camera");
        ok &= Check(scene.SaveToFile(path), "scene without a main camera saves");
        Scene reloaded;
        ok &= Check(Scene::LoadFromFile(path, reloaded), "scene without a main camera loads");
        ok &= Check(!reloaded.GetMainCameraEntity().IsValid(),
            "reload keeps 'no main camera' instead of promoting the other camera");
        ok &= Check(reloaded.TryGetCameraComponent(other) != nullptr, "the other camera still exists after reload");
        ok &= Check(reloaded.GetMainCamera().Projection.VerticalFovDegrees == scene.GetMainCamera().Projection.VerticalFovDegrees,
            "the cached main camera values survive the round trip");

        // The designated case still round-trips as designated, and a file that never
        // names a main camera (legacy) still promotes the first camera.
        ok &= Check(scene.SetMainCameraEntity(other) && scene.SaveToFile(path), "designated main camera saves");
        Scene designated;
        ok &= Check(Scene::LoadFromFile(path, designated) && designated.GetMainCameraEntity() == other,
            "a designated main camera reloads as designated");
        WriteText(path, SceneText(std::string(kValidCameraEntity)));
        std::string legacy = ReadText(path);
        const size_t key = legacy.find("MainCameraEntity 1\n");
        ok &= Check(key != std::string::npos, "fixture has the key");
        if (key != std::string::npos)
            legacy.erase(key, std::string("MainCameraEntity 1\n").size());
        WriteText(path, legacy);
        Scene promoted;
        ok &= Check(Scene::LoadFromFile(path, promoted) && promoted.GetMainCameraEntity() == Entity { 1 },
            "a file without a MainCameraEntity key promotes its first camera");
        return ok;
    }

    // Behavior: the next-id counter saturates at the largest id and CreateEntity /
    // CloneEntity then fail without consuming an id, so ids are never reused and
    // an invalid entity is never returned silently from a wrapped counter.
    // Oracle: id uniqueness across a created batch plus the exact failure result.
    bool TestSceneEntityIdCounterSaturatesInsteadOfWrapping()
    {
        constexpr EntityId maxId = std::numeric_limits<EntityId>::max();
        bool ok = true;

        TempDirectory directory;
        const std::filesystem::path path = directory.File("limit.scene");
        WriteText(path, SceneText(std::string(kValidCameraEntity) + "Entity 4294967295 \"Last\"\nTransform 4294967295 0 0 0 0 0 0 0 0 0 1 1 1\n",
            "4294967295"));
        Scene scene;
        ok &= Check(Scene::LoadFromFile(path, scene), "a scene holding the largest id loads");
        ok &= Check(scene.IsEntityValid(Entity { maxId }), "the largest id resolves");
        for (int index = 0; index < 4; ++index)
        {
            const Entity created = scene.CreateEntity("after the limit");
            ok &= Check(!created.IsValid(), "CreateEntity at an exhausted counter fails instead of wrapping");
        }
        ok &= Check(!scene.CloneEntity(Entity { 1 }, "copy").IsValid(), "CloneEntity at an exhausted counter fails");
        ok &= Check(scene.GetEntities().size() == 2, "no entity was created");

        Scene second;
        SceneEntity entity;
        entity.EntityHandle = Entity { maxId };
        entity.Name = "restored limit";
        ok &= Check(second.RestoreEntity(entity), "restore with the largest id");
        const Entity beside = second.CreateEntity("beside");
        ok &= Check(beside.IsValid() && beside.Id != maxId, "creation continues below the restored largest id");

        // Exhaust the counter through the public API: the largest id has no
        // successor, so the counter reaching it ends allocation for good.
        SceneEntity nearLimit;
        nearLimit.EntityHandle = Entity { maxId - 1 };
        nearLimit.Name = "near limit";
        ok &= Check(second.RestoreEntity(nearLimit), "restore with the second-largest id");
        ok &= Check(!second.CreateEntity("after").IsValid() && !second.CreateEntity("after").IsValid(),
            "the counter stays exhausted across repeated attempts");
        ok &= Check(!second.CloneEntity(Entity { maxId }, "copy").IsValid(), "CloneEntity fails once the id space is exhausted");
        ok &= Check(second.IsEntityValid(Entity { maxId }) && !second.IsEntityValid({}), "lookups stay exact at the limit");
        return ok;
    }

    // Behavior: entity lookup is a constant-time id index, so loading and
    // creating scale linearly with the entity count.
    // Oracle: the time ratio between a 4x larger input and the base input.
    // Linear work gives about 4; the former linear-scan lookup gives about 16.
    bool TestSceneLoadAndLookupScaleLinearly()
    {
        constexpr size_t small = 12000;
        constexpr size_t large = 48000;
        const auto buildText = [](size_t count)
        {
            std::ostringstream records;
            records << kValidCameraEntity;
            for (size_t index = 0; index < count; ++index)
            {
                const size_t id = index + 2;
                records << "Entity " << id << " \"e\"\nTransform " << id << " 0 0 0 1 2 3 0 0 0 1 1 1\n"
                    << "MeshRenderer " << id << " 1 2 \"m\" true true\n";
            }
            return SceneText(records.str(), std::to_string(count + 10));
        };

        TempDirectory directory;
        const auto timeLoad = [&](size_t count)
        {
            const std::filesystem::path path = directory.File("scale-" + std::to_string(count) + ".scene");
            WriteText(path, buildText(count));
            double best = std::numeric_limits<double>::max();
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                Scene scene;
                const auto start = Clock::now();
                const bool loaded = Scene::LoadFromFile(path, scene);
                const double milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                if (!loaded || scene.GetEntities().size() != count + 1)
                    return -1.0;
                best = std::min(best, milliseconds);
            }
            return best;
        };
        const auto timeCreate = [](size_t count)
        {
            Scene scene;
            const auto start = Clock::now();
            for (size_t index = 0; index < count; ++index)
                scene.CreateEntity("e");
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        };

        const double loadSmall = timeLoad(small);
        const double loadLarge = timeLoad(large);
        const double createSmall = std::max(timeCreate(small), 0.05);
        const double createLarge = timeCreate(large);
        std::cout << "SceneScaleV1 loadMs(" << small << ")=" << loadSmall << " loadMs(" << large << ")=" << loadLarge
            << " createMs(" << small << ")=" << createSmall << " createMs(" << large << ")=" << createLarge << '\n';

        bool ok = Check(loadSmall > 0.0 && loadLarge > 0.0, "the generated scenes load");
        // A generous bound between the linear 4x and the quadratic 16x.
        ok &= Check(loadLarge < loadSmall * 10.0,
            "load time scales sub-quadratically (" + std::to_string(loadSmall) + " ms -> " + std::to_string(loadLarge) + " ms)");
        ok &= Check(createLarge < createSmall * 10.0 + 20.0,
            "creation scales sub-quadratically (" + std::to_string(createSmall) + " ms -> " + std::to_string(createLarge) + " ms)");
        return ok;
    }

    // Behavior: for generated scenes (random ids, names with escapes and line
    // breaks, hostile transform/camera/light values written through the public
    // mutable fields), SaveToFile either fails and leaves the previous file
    // untouched, or the file it wrote loads back to exactly the same entities,
    // main camera designation and invariants.
    // Oracle: save-implies-load round trip with a field-wise equality that does
    // not use the Scene validators, plus an independent invariant predicate.
    // Replay: each case is the seed printed on failure (std::mt19937).
    bool TestSceneGeneratedPopulationsSaveOnlyWhatLoadsBack()
    {
        TempDirectory directory;
        const std::filesystem::path path = directory.File("generated.scene");
        size_t saved = 0;
        size_t refused = 0;

        for (std::uint32_t seed = 1; seed <= 300; ++seed)
        {
            std::mt19937 random(seed);
            const auto pick = [&](size_t count) { return static_cast<size_t>(random() % count); };
            const auto hostileFloat = [&](float normal)
            {
                switch (pick(14))
                {
                    case 0: return kNaN;
                    case 1: return kInf;
                    case 2: return -kInf;
                    case 3: return 0.0f;
                    case 4: return -1.0f;
                    case 5: return 1.0e-39f;
                    case 6: return std::numeric_limits<float>::max();
                    default: return normal;
                }
            };
            const auto normalFloat = [&](float low, float high)
            {
                return low + (high - low) * static_cast<float>(random() % 100001) / 100000.0f;
            };
            const auto makeName = [&]()
            {
                // Quotes, backslashes, tabs, DEL and UTF-8 bytes must round-trip. A
                // line break cannot be framed, so it appears rarely and the save
                // must then be refused.
                static const char alphabet[] = "abc XYZ\"\\\t\x7f\xc3\xa9[]=";
                std::string name;
                const size_t length = pick(8);
                for (size_t index = 0; index < length; ++index)
                    name.push_back(alphabet[pick(sizeof(alphabet) - 1)]);
                if (pick(25) == 0)
                    name.push_back(pick(2) == 0 ? '\n' : '\r');
                return name;
            };

            Scene scene(pick(6) == 0 ? makeName() : "Generated");
            const size_t extra = 1 + pick(6);
            std::vector<Entity> entities { scene.GetMainCameraEntity() };
            for (size_t index = 0; index < extra; ++index)
            {
                SceneEntity candidate;
                candidate.EntityHandle = Entity { pick(4) == 0 ? static_cast<EntityId>(0xFFFFFFF0u + pick(16)) : static_cast<EntityId>(2 + pick(1000)) };
                candidate.Name = makeName();
                candidate.Transform.RotationDegrees = { normalFloat(-360.0f, 360.0f), normalFloat(-360.0f, 360.0f), normalFloat(-360.0f, 360.0f) };
                candidate.Transform.Scale = { normalFloat(0.01f, 5.0f), normalFloat(0.01f, 5.0f), normalFloat(0.01f, 5.0f) };
                if (pick(2) == 0)
                {
                    CameraComponent camera;
                    camera.Primary = pick(2) == 0;
                    camera.Projection.VerticalFovDegrees = normalFloat(10.0f, 120.0f);
                    camera.Projection.NearClip = normalFloat(0.01f, 2.0f);
                    camera.Projection.FarClip = camera.Projection.NearClip + normalFloat(1.0f, 5000.0f);
                    candidate.Transform.Scale = { 1.0f, 1.0f, 1.0f };
                    candidate.Camera = camera;
                }
                if (pick(2) == 0)
                {
                    LightComponent light;
                    light.Type = static_cast<LightType>(pick(3));
                    light.PhotometricUnit = GetLightPhotometricUnit(light.Type);
                    light.PhotometricValue = light.Type == LightType::Directional ? 1000.0 + pick(5000) : 100.0 + pick(5000);
                    light.Range = light.Type == LightType::Directional ? 0.0f : 1.0f + pick(50);
                    candidate.Light = light;
                }
                if (pick(2) == 0)
                {
                    MeshRendererComponent mesh;
                    mesh.MeshAsset = pick(100);
                    mesh.MaterialAsset = pick(100);
                    mesh.MeshName = makeName();
                    mesh.Visible = pick(2) == 0;
                    mesh.CastsShadows = pick(2) == 0;
                    candidate.MeshRenderer = mesh;
                }
                if (scene.RestoreEntity(candidate))
                    entities.push_back(candidate.EntityHandle);
            }

            // Hostile values written through the public mutable fields, which the
            // authoring API cannot validate.
            const size_t hostileEdits = pick(3);
            for (size_t edit = 0; edit < hostileEdits; ++edit)
            {
                const Entity target = entities[pick(entities.size())];
                switch (pick(5))
                {
                    case 0: scene.TryGetTransform(target)->RotationDegrees.X = hostileFloat(1.0f); break;
                    case 1: scene.TryGetTransform(target)->Scale.Y = hostileFloat(1.0f); break;
                    case 2:
                        if (CameraComponent* camera = scene.TryGetCameraComponent(target))
                            camera->Projection.VerticalFovDegrees = hostileFloat(60.0f);
                        break;
                    case 3:
                        if (CameraComponent* camera = scene.TryGetCameraComponent(target))
                            camera->BackgroundColor.Z = hostileFloat(0.5f);
                        break;
                    default:
                        if (pick(2) == 0)
                            scene.RemoveCameraComponent(target);
                        else
                            scene.TryGetEntity(target)->Name += "\n";
                        break;
                }
            }

            WriteText(path, "previous contents");
            const std::string before = ReadText(path);
            const bool saveSucceeded = scene.SaveToFile(path);
            const std::string context = " (seed " + std::to_string(seed) + ")";
            if (!saveSucceeded)
            {
                ++refused;
                if (!Check(ReadText(path) == before, "a refused save must leave the destination untouched" + context))
                    return false;
                continue;
            }

            ++saved;
            Scene loaded;
            if (!Check(Scene::LoadFromFile(path, loaded), "everything SaveToFile writes must load back" + context))
                return false;
            if (!Check(loaded.GetName() == scene.GetName() && loaded.GetEntities().size() == scene.GetEntities().size(),
                    "the reloaded scene has the same name and entity count" + context)
                || !Check(loaded.GetMainCameraEntity() == scene.GetMainCameraEntity(),
                    "the main camera designation survives the round trip" + context))
                return false;
            for (size_t index = 0; index < scene.GetEntities().size(); ++index)
            {
                if (!Check(SameEntity(scene.GetEntities()[index], loaded.GetEntities()[index]),
                        "entity " + std::to_string(index) + " round-trips exactly" + context)
                    || !Check(EntitySatisfiesInvariants(loaded.GetEntities()[index]),
                        "a loaded entity satisfies the scene invariants" + context))
                    return false;
            }
        }

        std::cout << "SceneGeneratedRoundTripV1 seeds=300 saved=" << saved << " refused=" << refused << '\n';
        return Check(saved > 20 && refused > 20, "the generator reaches both accepted and refused saves");
    }
}
