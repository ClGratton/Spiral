#include "EditorHistoryStateTests.h"

#include "EditorHistoryState.h"
#include "History/HistoryNaming.h"
#include "History/HistoryStore.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using namespace EditorHistory;

    // Failure hypotheses, oracles, and non-claims for the whole file:
    // - A label must read like the user's action ("Move Cube", "Edit Light.Color
    //   of Sun"). The oracle is a hand-written table of every EditProperty and
    //   every Edit menu state, written from the wording the owner specified,
    //   not generated from the implementation.
    // - EditorHistoryStatesEqual must never call two different project states
    //   equal (that would silently drop an edit from the history). The oracle is
    //   a mutation table: one mutation per component/material/registry field,
    //   each of which must break equality, plus the fields that must NOT
    //   (selection, navigation mirror, epoch, special-entity ids).
    // - Tier: fast, pure (Scene and registry values only); no ImGui, GPU or disk.
    // - Not claimed: that the estimate matches allocator behaviour, or anything
    //   about the Editor's live restore.

    struct Checker
    {
        const char* Suite;
        bool Ok = true;

        void Expect(bool condition, const std::string& message)
        {
            if (!condition)
            {
                std::cerr << "Editor history state test failed [" << Suite << "]: " << message << '\n';
                Ok = false;
            }
        }
    };

    struct NamingState
    {
        int Value = 0;
    };

    class NamingAdapter final : public IHistoryStateAdapter<NamingState>
    {
    public:
        bool Restore(const std::shared_ptr<const NamingState>&) override { return true; }
        Engine::u64 EstimateBytes(const NamingState&) const override { return 16; }
        bool Equal(const NamingState&, const NamingState&) const override { return false; }
    };

    EditorHistoryState MakeBaseState()
    {
        EditorHistoryState state;
        state.Scene = Engine::Scene("Equality Scene");
        const Engine::Entity cube = state.Scene.CreateEntity("Cube");
        const Engine::Entity sun = state.Scene.CreateEntity("Sun");
        const Engine::Entity spare = state.Scene.CreateEntity("Spare Camera");
        state.Scene.AddLightComponent(sun, Engine::LightComponent {});
        Engine::CameraComponent spareCamera;
        spareCamera.Primary = false;
        state.Scene.AddCameraComponent(spare, spareCamera);
        Engine::MeshRendererComponent renderer;
        renderer.MeshAsset = 11;
        renderer.MaterialAsset = 12;
        renderer.MeshName = "Cube Mesh";
        state.Scene.AddMeshRendererComponent(cube, renderer);
        state.Scene.SetEntityWorldPosition(cube, { 1.0, 2.0, 3.0 });

        state.AssetRegistry.RegisterAsset(Engine::AssetType::Mesh, "meshes/cube.mesh", "Cube Mesh");
        const Engine::AssetHandle material =
            state.AssetRegistry.RegisterAsset(Engine::AssetType::Material, "materials/cube.spiralmat", "Cube Material");
        Engine::MaterialAsset asset;
        asset.Name = "Cube Material";
        state.MaterialLibrary.Set(material, asset);
        state.SelectedEntity = cube;
        state.PrototypeMeshEntity = cube;
        state.DirectionalLightEntity = sun;
        state.PlayerStartEntity = spare;
        return state;
    }

    Engine::AssetHandle MaterialHandleOf(const EditorHistoryState& state)
    {
        for (const Engine::AssetMetadata& metadata : state.AssetRegistry.GetAssets())
        {
            if (metadata.Type == Engine::AssetType::Material)
                return metadata.Handle;
        }
        return Engine::kInvalidAssetHandle;
    }

    Engine::Entity Find(const EditorHistoryState& state, const char* name)
    {
        return state.Scene.FindEntityByName(name);
    }
}

namespace SpiralTests
{
    bool TestHistoryNamingTablesAndMenuText()
    {
        Checker check { "naming" };

        struct Row
        {
            EditProperty Property;
            const char* Target;
            const char* Display;
        };
        // Written by hand from the owner's wording, never derived from the code.
        const Row rows[] = {
            { EditProperty::EntityName, "Cube", "Rename Cube" },
            { EditProperty::TransformPosition, "Cube", "Move Cube" },
            { EditProperty::TransformRotation, "Cube", "Rotate Cube" },
            { EditProperty::TransformScale, "Cube", "Scale Cube" },
            { EditProperty::CameraPrimary, "Cam", "Edit Camera.Primary of Cam" },
            { EditProperty::CameraVerticalFov, "Cam", "Edit Camera.Vertical FOV of Cam" },
            { EditProperty::CameraNearClip, "Cam", "Edit Camera.Near Clip of Cam" },
            { EditProperty::CameraFarClip, "Cam", "Edit Camera.Far Clip of Cam" },
            { EditProperty::CameraBackgroundColor, "Cam", "Edit Camera.Background Color of Cam" },
            { EditProperty::LightType, "Sun", "Edit Light.Type of Sun" },
            { EditProperty::LightColor, "Sun", "Edit Light.Color of Sun" },
            { EditProperty::LightPhotometricValue, "Sun", "Edit Light.Intensity of Sun" },
            { EditProperty::LightRange, "Sun", "Edit Light.Range of Sun" },
            { EditProperty::LightInnerCone, "Sun", "Edit Light.Inner Cone of Sun" },
            { EditProperty::LightOuterCone, "Sun", "Edit Light.Outer Cone of Sun" },
            { EditProperty::LightCastsShadows, "Sun", "Edit Light.Casts Shadows of Sun" },
            { EditProperty::MeshRendererMeshName, "Cube", "Edit Mesh Renderer.Mesh Name of Cube" },
            { EditProperty::MeshRendererMeshAsset, "Cube", "Edit Mesh Renderer.Mesh Asset of Cube" },
            { EditProperty::MeshRendererMaterialAsset, "Cube", "Edit Mesh Renderer.Material Asset of Cube" },
            { EditProperty::MeshRendererVisible, "Cube", "Edit Mesh Renderer.Visible of Cube" },
            { EditProperty::MeshRendererCastsShadows, "Cube", "Edit Mesh Renderer.Casts Shadows of Cube" },
            { EditProperty::MaterialName, "Stone", "Edit Material.Name of Stone" },
            { EditProperty::MaterialShadingModel, "Stone", "Edit Material.Shading Model of Stone" },
            { EditProperty::MaterialAlphaMode, "Stone", "Edit Material.Alpha Mode of Stone" },
            { EditProperty::MaterialTwoSided, "Stone", "Edit Material.Two Sided of Stone" },
            { EditProperty::MaterialBaseColor, "Stone", "Edit Material.Base Color of Stone" },
            { EditProperty::MaterialMetallic, "Stone", "Edit Material.Metallic of Stone" },
            { EditProperty::MaterialRoughness, "Stone", "Edit Material.Roughness of Stone" },
            { EditProperty::MaterialNormalScale, "Stone", "Edit Material.Normal Scale of Stone" },
            { EditProperty::MaterialOcclusionStrength, "Stone", "Edit Material.Occlusion Strength of Stone" },
            { EditProperty::MaterialEmissiveColor, "Stone", "Edit Material.Emissive Color of Stone" },
            { EditProperty::MaterialEmissiveStrength, "Stone", "Edit Material.Emissive Strength of Stone" },
            { EditProperty::MaterialAlphaCutoff, "Stone", "Edit Material.Alpha Cutoff of Stone" },
            { EditProperty::MaterialTextureBaseColor, "Stone", "Edit Material.Base Color Texture of Stone" },
            { EditProperty::MaterialTextureNormal, "Stone", "Edit Material.Normal Texture of Stone" },
            { EditProperty::MaterialTextureOrm, "Stone", "Edit Material.Orm Texture of Stone" },
            { EditProperty::MaterialTextureEmissive, "Stone", "Edit Material.Emissive Texture of Stone" },
            { EditProperty::MaterialTextureOpacity, "Stone", "Edit Material.Opacity Texture of Stone" },
            { EditProperty::MaterialTextureCallistoControl, "Stone", "Edit Material.Callisto Control Texture of Stone" },
            { EditProperty::MaterialSamplerBaseColor, "Stone", "Edit Material.Base Color Sampling of Stone" },
            { EditProperty::MaterialSamplerNormal, "Stone", "Edit Material.Normal Sampling of Stone" },
            { EditProperty::MaterialSamplerOrm, "Stone", "Edit Material.Orm Sampling of Stone" },
            { EditProperty::MaterialSamplerEmissive, "Stone", "Edit Material.Emissive Sampling of Stone" },
            { EditProperty::MaterialSamplerOpacity, "Stone", "Edit Material.Opacity Sampling of Stone" },
            { EditProperty::MaterialSamplerCallistoControl, "Stone", "Edit Material.Callisto Control Sampling of Stone" },
            { EditProperty::MaterialDiffuseFresnel, "Stone", "Edit Material.Diffuse Fresnel of Stone" },
            { EditProperty::MaterialRetroreflection, "Stone", "Edit Material.Retroreflection of Stone" },
            { EditProperty::MaterialDiffuseFalloff, "Stone", "Edit Material.Diffuse Falloff of Stone" },
            { EditProperty::MaterialRetroreflectionFalloff, "Stone", "Edit Material.Retroreflection Falloff of Stone" },
            { EditProperty::MaterialSmoothTerminator, "Stone", "Edit Material.Smooth Terminator of Stone" },
            { EditProperty::ColorExposureMode, "Project", "Edit Color Pipeline.Exposure Mode of Project" },
            { EditProperty::ColorManualExposure, "Project", "Edit Color Pipeline.Manual EV100 of Project" },
            { EditProperty::ColorAperture, "Project", "Edit Color Pipeline.Aperture of Project" },
            { EditProperty::ColorShutter, "Project", "Edit Color Pipeline.Shutter of Project" },
            { EditProperty::ColorIso, "Project", "Edit Color Pipeline.ISO of Project" },
            { EditProperty::ColorSaturation, "Project", "Edit Color Pipeline.Saturation of Project" },
            { EditProperty::ColorContrast, "Project", "Edit Color Pipeline.Contrast of Project" },
        };
        // The table covers every property exactly once: None is unnamed and Count is the size.
        check.Expect(sizeof(rows) / sizeof(rows[0]) == static_cast<size_t>(EditProperty::Count) - 1,
            "the table must name every EditProperty except None");
        for (size_t index = 0; index < sizeof(rows) / sizeof(rows[0]); ++index)
        {
            const Row& row = rows[index];
            check.Expect(static_cast<size_t>(row.Property) == index + 1, "table order follows the enum");
            const HistoryLabel label = MakeEditLabel(row.Property, row.Target);
            check.Expect(label.Display() == row.Display,
                std::string("label for property ") + std::to_string(index + 1) + " was \"" + label.Display() + "\"");
            check.Expect(label.Source == HistorySource::User, "default source is user");
            check.Expect(MakeEditLabel(row.Property, row.Target, HistorySource::Agent).Source == HistorySource::Agent,
                "agent source is carried");
        }
        check.Expect(MakeEditLabel(EditProperty::None, "x").Display() == "Edit x", "None falls back to a plain edit");

        check.Expect(MakeTransformEditLabel("Cube", true, false, false).Display() == "Move Cube", "move only");
        check.Expect(MakeTransformEditLabel("Cube", false, true, false).Display() == "Rotate Cube", "rotate only");
        check.Expect(MakeTransformEditLabel("Cube", false, false, true).Display() == "Scale Cube", "scale only");
        check.Expect(MakeTransformEditLabel("Cube", true, true, false).Display() == "Edit Transform of Cube", "two parts");
        check.Expect(MakeTransformEditLabel("Cube", false, false, false).Display() == "Edit Transform of Cube", "no part");
        check.Expect(MakeComponentEditLabel("Light", "Sun").Display() == "Edit Light of Sun", "component edit");
        check.Expect(MakeAddComponentLabel("Camera", "Cube").Display() == "Add Camera to Cube", "add component");
        check.Expect(MakeCreateLabel("Entity 2").Display() == "Create Entity 2", "create");
        check.Expect(MakeDeleteLabel("Cube").Display() == "Delete Cube", "delete");
        check.Expect(MakePlaceLabel("Rock").Display() == "Place Rock", "place");
        check.Expect(MakeEditLabel(EditProperty::TransformPosition, std::string(100, 'x')).Target.size() == 32,
            "a long target is shortened to 32 code points by the label sanitiser");

        // Edit menu states, driven through a real store so the availability values are the store's.
        NamingAdapter adapter;
        HistoryStore<NamingState> store(adapter);
        auto snapshot = [](int value) { return std::make_shared<const NamingState>(NamingState { value }); };

        HistoryCommandText undo = DescribeUndoCommand(store.UndoAvailability(), store.TopUndo(), store.BaseReason());
        check.Expect(!undo.Enabled && undo.Label == "Undo (nothing to undo)" && undo.Reason == "Undo unavailable: nothing to undo",
            "empty history: " + undo.Label + " / " + undo.Reason);
        HistoryCommandText redo = DescribeRedoCommand(store.RedoAvailability(), store.TopRedo());
        check.Expect(!redo.Enabled && redo.Label == "Redo (nothing to redo)" && redo.Reason == "Redo unavailable: nothing to redo",
            "empty redo: " + redo.Label + " / " + redo.Reason);

        store.Record(MakeEditLabel(EditProperty::TransformPosition, "Cube"), snapshot(0), snapshot(1));
        store.Record(MakeEditLabel(EditProperty::EntityName, "Light"), snapshot(1), snapshot(2));
        undo = DescribeUndoCommand(store.UndoAvailability(), store.TopUndo(), store.BaseReason());
        check.Expect(undo.Enabled && undo.Label == "Undo: Rename Light" && undo.Reason.empty(), "undo names the last action");
        redo = DescribeRedoCommand(store.RedoAvailability(), store.TopRedo());
        check.Expect(!redo.Enabled, "nothing to redo after a record");
        store.Undo();
        redo = DescribeRedoCommand(store.RedoAvailability(), store.TopRedo());
        check.Expect(redo.Enabled && redo.Label == "Redo: Rename Light" && redo.Reason.empty(), "redo names the undone action");
        undo = DescribeUndoCommand(store.UndoAvailability(), store.TopUndo(), store.BaseReason());
        check.Expect(undo.Enabled && undo.Label == "Undo: Move Cube", "undo now names the earlier action");

        store.Barrier(MakeHistoryLabel("Import Fab Asset", "", HistorySource::System), "Fab import changed the project");
        undo = DescribeUndoCommand(store.UndoAvailability(), store.TopUndo(), store.BaseReason());
        check.Expect(!undo.Enabled && undo.Label == "Undo (blocked: Import Fab Asset)"
                && undo.Reason == "Undo unavailable: Fab import changed the project",
            "barrier: " + undo.Label + " / " + undo.Reason);

        store.BeginGesture({ 1, 2, 3 }, MakeEditLabel(EditProperty::LightColor, "Sun"), snapshot(7));
        undo = DescribeUndoCommand(store.UndoAvailability(), store.TopUndo(), store.BaseReason());
        redo = DescribeRedoCommand(store.RedoAvailability(), store.TopRedo());
        check.Expect(!undo.Enabled && undo.Reason == "Undo unavailable: finish the current edit", "gesture blocks undo");
        check.Expect(!redo.Enabled && redo.Reason == "Redo unavailable: finish the current edit", "gesture blocks redo");
        store.EndGesture({ 1, 2, 3 }, snapshot(8));

        // Dropped history: a one-entry cap forces eviction, and the base row explains why undo ends there.
        NamingAdapter smallAdapter;
        HistoryConfig tiny;
        tiny.MaximumEntries = 1;
        HistoryStore<NamingState> small(smallAdapter, tiny);
        small.Record(MakeEditLabel(EditProperty::TransformPosition, "A"), snapshot(0), snapshot(1));
        small.Record(MakeEditLabel(EditProperty::TransformPosition, "B"), snapshot(1), snapshot(2));
        small.Undo();
        undo = DescribeUndoCommand(small.UndoAvailability(), small.TopUndo(), small.BaseReason());
        check.Expect(!undo.Enabled && undo.Reason.rfind("Undo unavailable: earlier history was dropped", 0) == 0,
            "dropped history explains itself: " + undo.Reason);
        return check.Ok;
    }

    bool TestEditorHistoryStateEqualityNamesEveryField()
    {
        Checker check { "equality" };
        const EditorHistoryState base = MakeBaseState();
        check.Expect(EditorHistoryStatesEqual(base, base), "a state equals itself");
        {
            EditorHistoryState copy = base;
            check.Expect(EditorHistoryStatesEqual(base, copy), "a copy equals the original");
        }

        struct Mutation
        {
            const char* Name;
            std::function<void(EditorHistoryState&)> Apply;
        };
        const auto cube = [](EditorHistoryState& state) { return state.Scene.TryGetEntity(Find(state, "Cube")); };
        const auto sun = [](EditorHistoryState& state) { return state.Scene.TryGetEntity(Find(state, "Sun")); };
        const auto material = [](EditorHistoryState& state) { return state.MaterialLibrary.Get(MaterialHandleOf(state)); };

        const std::vector<Mutation> mustDiffer = {
            { "entity name", [&](EditorHistoryState& s) { cube(s)->Name = "Cube2"; } },
            { "position x", [&](EditorHistoryState& s) { s.Scene.SetEntityWorldPositionAxis(Find(s, "Cube"), 0, 1.5); } },
            { "position y", [&](EditorHistoryState& s) { s.Scene.SetEntityWorldPositionAxis(Find(s, "Cube"), 1, 2.5); } },
            { "position z", [&](EditorHistoryState& s) { s.Scene.SetEntityWorldPositionAxis(Find(s, "Cube"), 2, 3.5); } },
            { "position sector", [&](EditorHistoryState& s) { s.Scene.SetEntityWorldPositionAxis(Find(s, "Cube"), 0, 9000.0); } },
            { "rotation", [&](EditorHistoryState& s) { cube(s)->Transform.RotationDegrees.Y = 5.0f; } },
            { "scale", [&](EditorHistoryState& s) { cube(s)->Transform.Scale.Z = 2.0f; } },
            { "entity added", [&](EditorHistoryState& s) { s.Scene.CreateEntity("Extra"); } },
            { "entity removed", [&](EditorHistoryState& s) { s.Scene.DestroyEntity(Find(s, "Cube")); } },
            { "scene name", [&](EditorHistoryState& s) { s.Scene = Engine::Scene("Other"); } },
            { "main camera fov", [&](EditorHistoryState& s) {
                  Engine::CameraComponent camera = s.Scene.GetMainCamera();
                  camera.Projection.VerticalFovDegrees = 70.0f;
                  s.Scene.SetMainCamera(camera); } },
            { "main camera near", [&](EditorHistoryState& s) {
                  Engine::CameraComponent camera = s.Scene.GetMainCamera();
                  camera.Projection.NearClip = 0.2f;
                  s.Scene.SetMainCamera(camera); } },
            { "main camera far", [&](EditorHistoryState& s) {
                  Engine::CameraComponent camera = s.Scene.GetMainCamera();
                  camera.Projection.FarClip = 200.0f;
                  s.Scene.SetMainCamera(camera); } },
            { "main camera background", [&](EditorHistoryState& s) {
                  Engine::CameraComponent camera = s.Scene.GetMainCamera();
                  camera.BackgroundColor.Z = 0.9f;
                  s.Scene.SetMainCamera(camera); } },
            { "spare camera primary", [&](EditorHistoryState& s) {
                  s.Scene.TryGetEntity(Find(s, "Spare Camera"))->Camera->Primary = true; } },
            { "main camera entity", [&](EditorHistoryState& s) { s.Scene.SetMainCameraEntity(Find(s, "Spare Camera")); } },
            { "camera component removed", [&](EditorHistoryState& s) { s.Scene.RemoveCameraComponent(Find(s, "Spare Camera")); } },
            { "light type", [&](EditorHistoryState& s) {
                  Engine::LightComponent light = *sun(s)->Light;
                  light.Type = Engine::LightType::Point;
                  light.PhotometricUnit = Engine::LightPhotometricUnit::Lumens;
                  light.PhotometricValue = 1000.0;
                  s.Scene.AddLightComponent(Find(s, "Sun"), light); } },
            { "light color", [&](EditorHistoryState& s) { sun(s)->Light->Color.X = 0.5f; } },
            { "light value", [&](EditorHistoryState& s) { sun(s)->Light->PhotometricValue += 1.0; } },
            { "light range", [&](EditorHistoryState& s) { sun(s)->Light->Range += 1.0f; } },
            { "light inner cone", [&](EditorHistoryState& s) { sun(s)->Light->InnerConeDegrees += 1.0f; } },
            { "light outer cone", [&](EditorHistoryState& s) { sun(s)->Light->OuterConeDegrees += 1.0f; } },
            { "light shadows", [&](EditorHistoryState& s) { sun(s)->Light->CastsShadows = !sun(s)->Light->CastsShadows; } },
            { "light removed", [&](EditorHistoryState& s) { s.Scene.RemoveLightComponent(Find(s, "Sun")); } },
            { "mesh asset", [&](EditorHistoryState& s) { cube(s)->MeshRenderer->MeshAsset = 99; } },
            { "mesh material asset", [&](EditorHistoryState& s) { cube(s)->MeshRenderer->MaterialAsset = 99; } },
            { "mesh name", [&](EditorHistoryState& s) { cube(s)->MeshRenderer->MeshName = "Other"; } },
            { "mesh visible", [&](EditorHistoryState& s) { cube(s)->MeshRenderer->Visible = false; } },
            { "mesh shadows", [&](EditorHistoryState& s) { cube(s)->MeshRenderer->CastsShadows = false; } },
            { "mesh renderer removed", [&](EditorHistoryState& s) { s.Scene.RemoveMeshRendererComponent(Find(s, "Cube")); } },
            { "registry asset name", [&](EditorHistoryState& s) { s.AssetRegistry.SetAssetName(MaterialHandleOf(s), "Renamed"); } },
            { "registry asset added", [&](EditorHistoryState& s) {
                  s.AssetRegistry.RegisterAsset(Engine::AssetType::Texture, "textures/a.png", "A"); } },
            { "registry asset removed", [&](EditorHistoryState& s) { s.AssetRegistry.RemoveAsset(s.AssetRegistry.GetAssets().front().Handle); } },
            { "registry base path", [&](EditorHistoryState& s) { s.AssetRegistry.SetCookedArtifactBasePath("some/where"); } },
            { "material name", [&](EditorHistoryState& s) { material(s)->Name = "Other"; } },
            { "material shading", [&](EditorHistoryState& s) { material(s)->ShadingModel = Engine::MaterialShadingModel::Unlit; } },
            { "material alpha mode", [&](EditorHistoryState& s) { material(s)->AlphaMode = Engine::MaterialAlphaMode::Blend; } },
            { "material two sided", [&](EditorHistoryState& s) { material(s)->TwoSided = true; } },
            { "material base color", [&](EditorHistoryState& s) { material(s)->BaseColor.Y = 0.1f; } },
            { "material metallic", [&](EditorHistoryState& s) { material(s)->Metallic = 0.7f; } },
            { "material roughness", [&](EditorHistoryState& s) { material(s)->Roughness = 0.9f; } },
            { "material normal scale", [&](EditorHistoryState& s) { material(s)->NormalScale = 2.0f; } },
            { "material occlusion", [&](EditorHistoryState& s) { material(s)->OcclusionStrength = 0.5f; } },
            { "material emissive color", [&](EditorHistoryState& s) { material(s)->EmissiveColor.X = 0.5f; } },
            { "material emissive strength", [&](EditorHistoryState& s) { material(s)->EmissiveStrength = 3.0f; } },
            { "material alpha cutoff", [&](EditorHistoryState& s) { material(s)->AlphaCutoff = 0.25f; } },
            { "material diffuse fresnel", [&](EditorHistoryState& s) { material(s)->DiffuseFresnelIntensity = 2.0f; } },
            { "material retroreflection", [&](EditorHistoryState& s) { material(s)->RetroreflectionIntensity = 2.0f; } },
            { "material diffuse falloff", [&](EditorHistoryState& s) { material(s)->DiffuseFresnelFalloff = 0.1f; } },
            { "material retro falloff", [&](EditorHistoryState& s) { material(s)->RetroreflectionFalloff = 0.1f; } },
            { "material smooth terminator", [&](EditorHistoryState& s) { material(s)->SmoothTerminator = 0.2f; } },
            { "material texture base color", [&](EditorHistoryState& s) { material(s)->Textures.BaseColor = 5; } },
            { "material texture normal", [&](EditorHistoryState& s) { material(s)->Textures.Normal = 5; } },
            { "material texture orm", [&](EditorHistoryState& s) { material(s)->Textures.Orm = 5; } },
            { "material texture emissive", [&](EditorHistoryState& s) { material(s)->Textures.Emissive = 5; } },
            { "material texture opacity", [&](EditorHistoryState& s) { material(s)->Textures.Opacity = 5; } },
            { "material texture callisto", [&](EditorHistoryState& s) { material(s)->Textures.CallistoControl = 5; } },
            { "material sampler base color", [&](EditorHistoryState& s) { material(s)->Samplers.BaseColor = Engine::MaterialTextureSampler::PointClamp; } },
            { "material sampler normal", [&](EditorHistoryState& s) { material(s)->Samplers.Normal = Engine::MaterialTextureSampler::PointClamp; } },
            { "material sampler orm", [&](EditorHistoryState& s) { material(s)->Samplers.Orm = Engine::MaterialTextureSampler::PointClamp; } },
            { "material sampler emissive", [&](EditorHistoryState& s) { material(s)->Samplers.Emissive = Engine::MaterialTextureSampler::PointClamp; } },
            { "material sampler opacity", [&](EditorHistoryState& s) { material(s)->Samplers.Opacity = Engine::MaterialTextureSampler::PointClamp; } },
            { "material sampler callisto", [&](EditorHistoryState& s) { material(s)->Samplers.CallistoControl = Engine::MaterialTextureSampler::PointClamp; } },
            { "color manual exposure", [&](EditorHistoryState& s) { s.ProjectColorPipelineSettings.ManualExposureEV100 = 1.0; } },
            { "color saturation", [&](EditorHistoryState& s) { s.ProjectColorPipelineSettings.PostToneMapSaturation = 1.5; } },
            { "color contrast", [&](EditorHistoryState& s) { s.ProjectColorPipelineSettings.PostToneMapContrast = 1.5; } },
            { "color exposure mode", [&](EditorHistoryState& s) {
                  s.ProjectColorPipelineSettings.ExposureMode = Engine::RendererExposureMode::CameraCalibration; } },
            { "color aperture", [&](EditorHistoryState& s) { s.ProjectColorPipelineSettings.CameraApertureFNumber = 8.0; } },
            { "color shutter", [&](EditorHistoryState& s) { s.ProjectColorPipelineSettings.CameraShutterSeconds = 0.5; } },
            { "color iso", [&](EditorHistoryState& s) { s.ProjectColorPipelineSettings.CameraISO = 800.0; } },
        };
        for (const Mutation& mutation : mustDiffer)
        {
            EditorHistoryState changed = base;
            mutation.Apply(changed);
            check.Expect(!EditorHistoryStatesEqual(base, changed), std::string("mutation must break equality: ") + mutation.Name);
            check.Expect(!EditorHistoryStatesEqual(changed, base), std::string("equality must be symmetric: ") + mutation.Name);
        }

        const std::vector<Mutation> mustStayEqual = {
            { "selection", [](EditorHistoryState& s) { s.SelectedEntity = {}; } },
            { "camera mirror position", [](EditorHistoryState& s) { s.CameraPosition = { 4.0, 5.0, 6.0 }; } },
            { "camera mirror rotation", [](EditorHistoryState& s) { s.CameraRotation = { 4.0f, 5.0f, 6.0f }; } },
            { "camera mirror projection", [](EditorHistoryState& s) { s.CameraFovDegrees = 90.0f; s.CameraNearClip = 1.0f; s.CameraFarClip = 9.0f; } },
            { "camera epoch", [](EditorHistoryState& s) { s.CameraEpoch = 41; } },
            { "special entity ids", [](EditorHistoryState& s) { s.PrototypeMeshEntity = {}; s.DirectionalLightEntity = {}; s.PlayerStartEntity = {}; } },
        };
        for (const Mutation& mutation : mustStayEqual)
        {
            EditorHistoryState changed = base;
            mutation.Apply(changed);
            check.Expect(EditorHistoryStatesEqual(base, changed), std::string("not project content: ") + mutation.Name);
        }

        // NaN never equals anything, itself included: the answer must be "different" so the edit is recorded.
        EditorHistoryState nan = base;
        nan.Scene.TryGetEntity(Find(nan, "Cube"))->Transform.RotationDegrees.X = std::numeric_limits<float>::quiet_NaN();
        check.Expect(!EditorHistoryStatesEqual(nan, nan), "a NaN field is never reported equal");

        // A material that exists in only one library is a difference in either direction.
        EditorHistoryState withoutMaterial = base;
        withoutMaterial.MaterialLibrary = Engine::MaterialLibrary {};
        check.Expect(!EditorHistoryStatesEqual(base, withoutMaterial) && !EditorHistoryStatesEqual(withoutMaterial, base),
            "a missing library material is a difference");
        return check.Ok;
    }

    bool TestEditorHistoryStateCameraEditAndEstimate()
    {
        Checker check { "camera-estimate" };
        const EditorHistoryState base = MakeBaseState();

        EditorHistoryState moved = base;
        moved.Scene.SetEntityWorldPosition(Find(moved, "Cube"), { 9.0, 9.0, 9.0 });
        check.Expect(!EditorHistoryCameraEdited(base, moved), "moving a cube is not a camera edit");

        EditorHistoryState navigated = base;
        navigated.Scene.SetEntityWorldPosition(navigated.Scene.GetMainCameraEntity(), { 5.0, 0.0, 0.0 });
        check.Expect(EditorHistoryCameraEdited(base, navigated), "moving the main camera is a camera edit");

        EditorHistoryState rotated = base;
        rotated.Scene.TryGetEntity(rotated.Scene.GetMainCameraEntity())->Transform.RotationDegrees.Y = 12.0f;
        check.Expect(EditorHistoryCameraEdited(base, rotated), "rotating the main camera is a camera edit");

        EditorHistoryState zoomed = base;
        Engine::CameraComponent camera = zoomed.Scene.GetMainCamera();
        camera.Projection.VerticalFovDegrees = 40.0f;
        zoomed.Scene.SetMainCamera(camera);
        check.Expect(EditorHistoryCameraEdited(base, zoomed), "changing the main camera lens is a camera edit");

        EditorHistoryState switched = base;
        switched.Scene.SetMainCameraEntity(Find(switched, "Spare Camera"));
        check.Expect(EditorHistoryCameraEdited(base, switched), "switching the main camera is a camera edit");

        EditorHistoryState spare = base;
        spare.Scene.SetEntityWorldPosition(Find(spare, "Spare Camera"), { 7.0, 7.0, 7.0 });
        check.Expect(!EditorHistoryCameraEdited(base, spare), "a non-main camera is ordinary content");
        check.Expect(!EditorHistoryCameraEdited(base, base), "identical states did not edit the camera");

        // Estimate: monotonic in what the snapshot holds, identical for a copy.
        const Engine::u64 baseBytes = EstimateEditorHistoryStateBytes(base);
        check.Expect(baseBytes >= sizeof(EditorHistoryState), "the estimate includes the state object");
        // A copy is what the history stores; copying again must not change the figure (capacity follows size).
        const EditorHistoryState firstCopy = base;
        const EditorHistoryState secondCopy = firstCopy;
        check.Expect(EstimateEditorHistoryStateBytes(firstCopy) == EstimateEditorHistoryStateBytes(secondCopy),
            "a copy of a copy estimates the same");
        EditorHistoryState many = base;
        for (int index = 0; index < 1000; ++index)
            many.Scene.CreateEntity("Entity " + std::to_string(index));
        const Engine::u64 manyBytes = EstimateEditorHistoryStateBytes(many);
        check.Expect(manyBytes >= baseBytes + 1000 * sizeof(Engine::SceneEntity), "1000 entities add at least their row size");
        EditorHistoryState longNames = base;
        longNames.Scene.TryGetEntity(Find(longNames, "Cube"))->Name = std::string(4096, 'n');
        check.Expect(EstimateEditorHistoryStateBytes(longNames) >= baseBytes + 4000, "a long name adds its heap bytes");
        EditorHistoryState moreAssets = base;
        for (int index = 0; index < 100; ++index)
            moreAssets.AssetRegistry.RegisterAsset(Engine::AssetType::Texture, "textures/" + std::to_string(index) + ".png");
        check.Expect(EstimateEditorHistoryStateBytes(moreAssets) >= baseBytes + 100 * sizeof(Engine::AssetMetadata),
            "100 registry rows add at least their row size");
        EditorHistoryState moreMaterials = base;
        const Engine::AssetHandle extra = moreMaterials.AssetRegistry.RegisterAsset(
            Engine::AssetType::Material, "materials/extra.spiralmat", "Extra");
        moreMaterials.MaterialLibrary.Set(extra, Engine::MaterialAsset {});
        check.Expect(EstimateEditorHistoryStateBytes(moreMaterials) >= baseBytes + sizeof(Engine::MaterialAsset),
            "a material in the library adds at least its size");
        return check.Ok;
    }
}
