#pragma once

#include "History/HistoryNaming.h"

#include "Engine/Assets/AssetRegistry.h"
#include "Engine/Assets/MaterialAsset.h"
#include "Engine/Core/Base.h"
#include "Engine/Renderer/ColorPipelineSettings.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <array>

// The whole project state one undo snapshot holds. It lives next to the
// EditorLayer (the composition root that owns Scene, AssetRegistry and
// MaterialLibrary); Editor/src/History knows nothing about these types and
// only sees this struct through IHistoryStateAdapter.
struct EditorHistoryState
{
    Engine::Scene Scene { "History Scene" };
    Engine::AssetRegistry AssetRegistry;
    Engine::MaterialLibrary MaterialLibrary;
    Engine::Entity SelectedEntity;
    std::array<double, 3> CameraPosition {};
    std::array<float, 3> CameraRotation {};
    float CameraFovDegrees = 60.0f;
    float CameraNearClip = 0.1f;
    float CameraFarClip = 100.0f;
    Engine::RendererColorPipelineSettings ProjectColorPipelineSettings;
    // The Editor's special entities, tracked by stable id and never re-found by
    // their editable name after a restore.
    Engine::Entity PrototypeMeshEntity;
    Engine::Entity DirectionalLightEntity;
    Engine::Entity PlayerStartEntity;
    // Counts the entries recorded so far that edited the viewport camera. A
    // restore keeps the live navigation pose unless the target snapshot has a
    // different epoch, i.e. unless an entry between here and there edited it.
    Engine::u64 CameraEpoch = 0;
};

// Heap bytes the snapshot keeps alive, estimated from entity, asset and
// material row counts and string capacity. It does not walk allocator state:
// the budget needs a stable monotonic figure, not an exact one.
Engine::u64 EstimateEditorHistoryStateBytes(const EditorHistoryState& state);

// Whether the two snapshots hold the same project content (Scene, asset
// registry, materials, colour pipeline). Selection, the navigation mirror
// floats and the epoch are not content. Conservative: any doubt, including a
// NaN, answers false so an edit is recorded rather than lost.
bool EditorHistoryStatesEqual(const EditorHistoryState& first, const EditorHistoryState& second);

// Whether the main camera entity, its transform, or its camera component
// differ: the entry that separates these two snapshots edited the camera.
bool EditorHistoryCameraEdited(const EditorHistoryState& before, const EditorHistoryState& after);

// Labels for edits that arrive as whole values (typed controls, project settings).
// Exactly one changed property names it ("Edit Light.Color of Sun", "Move Cube");
// several changed properties of one component name the component ("Edit Light of
// Sun", "Edit Transform of Cube"). Nothing changed uses the component wording too.
EditorHistory::HistoryLabel DescribeTransformEdit(std::string_view target,
    const Engine::Math::SectorLocalPosition& beforePosition, const Engine::TransformComponent& before,
    const Engine::Math::SectorLocalPosition& afterPosition, const Engine::TransformComponent& after,
    EditorHistory::HistorySource source);
EditorHistory::HistoryLabel DescribeLightEdit(std::string_view target, const Engine::LightComponent& before,
    const Engine::LightComponent& after, EditorHistory::HistorySource source);
EditorHistory::HistoryLabel DescribeColorPipelineEdit(const Engine::RendererColorPipelineSettings& before,
    const Engine::RendererColorPipelineSettings& after, EditorHistory::HistorySource source);
EditorHistory::HistoryLabel DescribeMeshRendererFlagsEdit(std::string_view target, bool beforeVisible,
    bool beforeCastsShadows, bool afterVisible, bool afterCastsShadows, EditorHistory::HistorySource source);
