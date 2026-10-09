// EditorLayer members for undo and redo: the adapter between the pure history
// store and live project state, capture/restore, gesture-coalesced Inspector
// edits, the Edit menu, the History panel, the status text, and the shortcut
// poll. The store (Editor/src/History) never sees Scene, AssetRegistry or
// MaterialLibrary; this file is where the two meet.
#include "EditorLayer.h"

#include "History/ShortcutDispatch.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace
{
    using EditorHistory::HistoryLabel;
    using EditorHistory::HistoryResult;
    using EditorHistory::HistoryStatus;

    // How long the menu-bar status text shows an announcement.
    constexpr Engine::u64 kStatusDurationMs = 4000;
    constexpr ImU32 kSelectionColor = IM_COL32(51, 79, 107, 255);
    constexpr ImU32 kSelectionHoverColor = IM_COL32(61, 97, 128, 255);

    bool ParseUnsignedOption(std::string_view text, Engine::u64& outValue)
    {
        if (text.empty() || text.size() > 20)
            return false;
        Engine::u64 value = 0;
        for (const char character : text)
        {
            if (character < '0' || character > '9')
                return false;
            const Engine::u64 digit = static_cast<Engine::u64>(character - '0');
            if (value > (std::numeric_limits<Engine::u64>::max() - digit) / 10)
                return false;
            value = value * 10 + digit;
        }
        outValue = value;
        return true;
    }
}

// ---------------------------------------------------------------- adapter

bool EditorLayer::HistoryAdapter::Restore(const HistorySnapshot& snapshot)
{
    // Keep the navigation camera unless an entry between the live state and the
    // snapshot edited it.
    return m_Owner.RestoreHistoryState(*snapshot, snapshot->CameraEpoch == m_Owner.m_CameraEpoch);
}

Engine::u64 EditorLayer::HistoryAdapter::EstimateBytes(const HistoryState& state) const
{
    return EstimateEditorHistoryStateBytes(state);
}

bool EditorLayer::HistoryAdapter::Equal(const HistoryState& first, const HistoryState& second) const
{
    return EditorHistoryStatesEqual(first, second);
}

void EditorLayer::ApplyHistoryCommandLine(const Engine::ApplicationCommandLineArgs& args)
{
    m_HistoryConfig = {};
    const std::string_view budget = args.GetOptionValue("--editor-history-budget-bytes");
    const std::string_view entries = args.GetOptionValue("--editor-history-max-entries");
    if (budget.empty() && entries.empty())
        return;
    Engine::u64 value = 0;
    if (!budget.empty())
    {
        if (!ParseUnsignedOption(budget, value))
            throw std::runtime_error("Invalid --editor-history-budget-bytes; expected an unsigned integer");
        m_HistoryConfig.BudgetBytes = value;
    }
    if (!entries.empty())
    {
        if (!ParseUnsignedOption(entries, value) || value == 0 || value > 100000)
            throw std::runtime_error("Invalid --editor-history-max-entries; expected 1 to 100000");
        m_HistoryConfig.MaximumEntries = static_cast<size_t>(value);
    }
    // The store has no config setter by design; nothing has been recorded yet.
    m_HistoryStore = std::make_unique<HistoryStoreType>(m_HistoryAdapter, m_HistoryConfig);
}

// ---------------------------------------------------------------- capture and restore

std::shared_ptr<EditorLayer::HistoryState> EditorLayer::CaptureHistorySnapshot() const
{
    ++m_HistorySnapshotCaptures;
    auto state = std::make_shared<HistoryState>();
    state->Scene = m_ActiveScene;
    state->AssetRegistry = m_AssetRegistry;
    state->MaterialLibrary = m_MaterialLibrary;
    state->SelectedEntity = m_SelectedEntity;
    state->CameraPosition = m_CameraPosition;
    state->CameraRotation = m_CameraRotation;
    state->CameraFovDegrees = m_CameraFovDegrees;
    state->CameraNearClip = m_CameraNearClip;
    state->CameraFarClip = m_CameraFarClip;
    state->ProjectColorPipelineSettings = m_ProjectColorPipelineSettings;
    state->PrototypeMeshEntity = m_PrototypeMeshEntity;
    state->DirectionalLightEntity = m_DirectionalLightEntity;
    state->PlayerStartEntity = m_PlayerStartEntity;
    state->CameraEpoch = m_CameraEpoch;
    return state;
}

bool EditorLayer::RestoreHistoryState(const HistoryState& state, bool preserveViewportCamera)
{
    if (!Engine::IsValidRendererColorPipelineSettings(state.ProjectColorPipelineSettings)
        || !Engine::Renderer::SetColorPipelineSettings(state.ProjectColorPipelineSettings))
    {
        Engine::Log::Error("History restore rejected invalid project color pipeline settings");
        return false;
    }

    // The navigation camera is not part of an edit. Only the same main camera entity
    // can carry the live pose over; any difference means the snapshot is a camera edit.
    preserveViewportCamera = preserveViewportCamera
        && state.Scene.GetMainCameraEntity() == m_ActiveScene.GetMainCameraEntity()
        && state.Scene.GetMainCameraEntity().IsValid();

    m_ActiveScene = state.Scene;
    m_AssetRegistry = state.AssetRegistry;
    m_MaterialLibrary = state.MaterialLibrary;
    Engine::Renderer::PublishArtifactResolvers(m_AssetRegistry, m_MaterialLibrary);
    m_SelectedEntity = state.SelectedEntity;
    m_ProjectColorPipelineSettings = state.ProjectColorPipelineSettings;
    // The special entities are tracked by id and travel with the snapshot; they are
    // never re-found by their editable names.
    m_PrototypeMeshEntity = m_ActiveScene.IsEntityValid(state.PrototypeMeshEntity) ? state.PrototypeMeshEntity : Engine::Entity {};
    m_DirectionalLightEntity = m_ActiveScene.IsEntityValid(state.DirectionalLightEntity) ? state.DirectionalLightEntity : Engine::Entity {};
    m_PlayerStartEntity = m_ActiveScene.IsEntityValid(state.PlayerStartEntity) ? state.PlayerStartEntity : Engine::Entity {};
    // An empty recorded selection (cleared by a viewport click or Esc) stays empty; only
    // a selection that no longer exists falls back.
    if (m_SelectedEntity && !m_ActiveScene.IsEntityValid(m_SelectedEntity))
        m_SelectedEntity = m_PrototypeMeshEntity ? m_PrototypeMeshEntity : m_ActiveScene.GetMainCameraEntity();

    if (preserveViewportCamera)
    {
        // m_Camera* still hold the live pose; write it into the restored scene.
        ApplyEditorCameraStateToScene();
    }
    else
    {
        m_CameraPosition = state.CameraPosition;
        m_CameraRotation = state.CameraRotation;
        m_CameraFovDegrees = state.CameraFovDegrees;
        m_CameraNearClip = state.CameraNearClip;
        m_CameraFarClip = state.CameraFarClip;
        SyncEditorCameraStateFromMainCamera(true);
    }
    m_CameraEpoch = state.CameraEpoch;
    ResetFusionNavigationPivotFromSelectionOrScene();
    m_AssetWatcher.SyncRegistry(m_AssetRegistry);
    return true;
}

// ---------------------------------------------------------------- recording

namespace
{
    // The epoch a state stamped as "after" carries: one more than Before when the
    // two states differ in the main camera.
    Engine::u64 AfterEpoch(const EditorHistoryState& before, const EditorHistoryState& after)
    {
        return before.CameraEpoch + (EditorHistoryCameraEdited(before, after) ? 1 : 0);
    }
}

EditorLayer::HistorySnapshot EditorLayer::CaptureBeforeSnapshot()
{
    FlushEditGesture();
    return CaptureHistorySnapshot();
}

HistoryResult EditorLayer::RecordHistory(HistoryLabel label, const HistorySnapshot& before)
{
    HistoryResult result;
    if (!before)
    {
        result.Status = HistoryStatus::InvalidArgument;
        return result;
    }
    std::shared_ptr<HistoryState> after = CaptureHistorySnapshot();
    if (EditorHistoryStatesEqual(*before, *after))
    {
        result.Status = HistoryStatus::NoChange;
        result.Head = History().HeadRevision();
        return result;
    }
    after->CameraEpoch = AfterEpoch(*before, *after);
    result = History().Record(std::move(label), before, after);
    if (result.Status == HistoryStatus::Recorded)
        m_CameraEpoch = after->CameraEpoch;
    AnnounceRecordResult(result);
    return result;
}

void EditorLayer::AnnounceRecordResult(const HistoryResult& result)
{
    if (result.Status != HistoryStatus::Recorded)
    {
        if (result.Status == HistoryStatus::GestureOpen)
            Engine::Log::Error("History record refused: an edit gesture is still open");
        return;
    }
    std::string text;
    if (result.RedoDiscarded != 0)
        text = EditorHistory::FormatRedoDiscardedNotice(result.RedoDiscarded);
    if (result.Evicted.Entries() != 0)
    {
        m_LastEvictionNotice = EditorHistory::FormatEvictionNotice(result.Evicted.ForBudget,
            result.Evicted.ForEntryCap, History().Config().BudgetBytes, History().Config().MaximumEntries);
        if (!text.empty())
            text += "; ";
        text += "Earlier history dropped";
    }
    AnnounceHistory(text);
}

bool EditorLayer::DiscreteEdit(const HistoryLabel& label, const std::function<bool()>& apply)
{
    const HistorySnapshot before = CaptureBeforeSnapshot();
    if (!apply())
        return false;
    return RecordHistory(label, before).Status == HistoryStatus::Recorded;
}

// ---------------------------------------------------------------- gestures

bool EditorLayer::TrackedEdit(Engine::u64 entityKey, EditorHistory::EditProperty property, bool edited,
    const std::function<HistoryLabel()>& makeLabel, const std::function<bool()>& apply)
{
    ProbeWidget(property);
    EditorHistory::EditItemFrame frame;
    frame.Key = { static_cast<Engine::u64>(ImGui::GetItemID()), entityKey, EditorHistory::ToPropertyId(property) };
    frame.Activated = ImGui::IsItemActivated();
    frame.Edited = edited;
    frame.Deactivated = ImGui::IsItemDeactivated();
    frame.DeactivatedAfterEdit = ImGui::IsItemDeactivatedAfterEdit();
    if (!frame.Activated && !frame.Edited && !frame.Deactivated && !frame.DeactivatedAfterEdit)
        return false;

    // A widget that became active while another gesture was still open (the
    // text field is closed one frame later) had its activation rejected. Its value
    // was not applied, so the live state is still the pre-edit state: open the
    // gesture on the first edited frame that finds the tracker free.
    const bool released = frame.Deactivated || frame.DeactivatedAfterEdit;
    if (frame.Edited && !frame.Activated && !released && !m_EditGestureTracker.Open() && ImGui::IsItemActive())
        frame.Activated = true;

    const EditorHistory::EditGestureResult gesture = m_EditGestureTracker.OnItem(frame);
    bool applied = false;
    switch (gesture.Action)
    {
    case EditorHistory::EditGestureAction::None:
        if (edited && m_EditGestureTracker.Open() && m_EditGestureTracker.ActiveKey() == frame.Key)
        {
            applied = apply();
            History().UpdateGesture(frame.Key);
        }
        break;
    case EditorHistory::EditGestureAction::Begin:
    {
        m_EditGestureBefore = CaptureHistorySnapshot();
        const HistoryResult began = History().BeginGesture(frame.Key, makeLabel(), m_EditGestureBefore);
        if (began.Status != HistoryStatus::Ok)
        {
            // The store refused (a typed mutation holds a transaction): drop the gesture.
            m_EditGestureTracker.Reset();
            m_EditGestureBefore.reset();
            break;
        }
        if (edited)
        {
            applied = apply();
            History().UpdateGesture(frame.Key);
        }
        break;
    }
    case EditorHistory::EditGestureAction::Discrete:
    {
        const HistorySnapshot before = CaptureBeforeSnapshot();
        applied = apply();
        if (applied)
            RecordHistory(makeLabel(), before);
        break;
    }
    case EditorHistory::EditGestureAction::End:
        if (edited)
            applied = apply();
        FinishGesture(gesture);
        break;
    case EditorHistory::EditGestureAction::Cancel:
    case EditorHistory::EditGestureAction::Rejected:
        break;
    }
    return applied;
}

void EditorLayer::ProbeWidget(EditorHistory::EditProperty property)
{
    if (m_WidgetProbe)
        m_WidgetProbe(property, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

void EditorLayer::FinishGesture(const EditorHistory::EditGestureResult& gesture)
{
    const HistorySnapshot before = std::move(m_EditGestureBefore);
    m_EditGestureBefore.reset();
    if (!before)
    {
        // No Before to compare against (the begin was refused): restore nothing, close.
        History().CancelGesture();
        return;
    }
    std::shared_ptr<HistoryState> after;
    if (gesture.Edited)
    {
        after = CaptureHistorySnapshot();
        after->CameraEpoch = AfterEpoch(*before, *after);
    }
    // An unchanged gesture ends with Before as its own After, which the store reads
    // as "same state" without comparing anything.
    const HistoryResult result = History().EndGesture(gesture.Key, after ? HistorySnapshot(after) : before);
    if (result.Status == HistoryStatus::Recorded && after)
        m_CameraEpoch = after->CameraEpoch;
    AnnounceRecordResult(result);
}

void EditorLayer::FlushEditGesture()
{
    if (m_EditGestureTracker.Open())
    {
        const EditorHistory::EditGestureResult gesture = m_EditGestureTracker.OnFrameEnd(false);
        if (gesture.Action == EditorHistory::EditGestureAction::End)
            FinishGesture(gesture);
    }
    if (History().GestureOpen())
    {
        // Defensive: a store gesture with no tracker gesture would block every record.
        History().CancelGesture();
        m_EditGestureBefore.reset();
    }
}

void EditorLayer::EndOfFrameEditGestureFlush()
{
    if (!m_EditGestureTracker.Open())
        return;
    const EditorHistory::EditGestureResult gesture = m_EditGestureTracker.OnFrameEnd(ImGui::IsAnyItemActive());
    if (gesture.Action == EditorHistory::EditGestureAction::End)
        FinishGesture(gesture);
}

void EditorLayer::ResetHistoryForProject(HistoryLabel baseLabel)
{
    m_EditGestureTracker.Reset();
    m_EditGestureBefore.reset();
    History().Reset(std::move(baseLabel));
    m_CameraEpoch = 0;
    m_LastEvictionNotice.clear();
}

void EditorLayer::InstallHistoryBarrier(const std::string& label, const std::string& reason)
{
    FlushEditGesture();
    History().Barrier(EditorHistory::MakeHistoryLabel(label, "", EditorHistory::HistorySource::System), reason);
    m_LastEvictionNotice.clear();
    AnnounceHistory("Undo history cleared: " + reason);
}

// ---------------------------------------------------------------- undo and redo

bool EditorLayer::Undo()
{
    FlushEditGesture();
    const HistoryResult result = History().Undo();
    AnnounceHistory(result.Succeeded() ? result.Announcement : UndoCommandText().Reason);
    return result.Succeeded();
}

bool EditorLayer::Redo()
{
    FlushEditGesture();
    const HistoryResult result = History().Redo();
    AnnounceHistory(result.Succeeded() ? result.Announcement : RedoCommandText().Reason);
    return result.Succeeded();
}

bool EditorLayer::JumpToHistoryRow(size_t row)
{
    FlushEditGesture();
    const HistoryResult result = History().JumpToRow(row);
    AnnounceHistory(result.Announcement);
    return result.Succeeded();
}

void EditorLayer::AnnounceHistory(const std::string& text)
{
    if (text.empty())
        return;
    m_LastHistoryAnnouncement = text;
    m_ConsoleLines.emplace_back(text);
    m_StatusExpiresAtMs = SpiralEditor::SteadyMillisecondClock() + kStatusDurationMs;
}

std::string EditorLayer::HistoryStatusText() const
{
    return SpiralEditor::SteadyMillisecondClock() < m_StatusExpiresAtMs ? m_LastHistoryAnnouncement : std::string();
}

EditorHistory::HistoryCommandText EditorLayer::UndoCommandText() const
{
    return EditorHistory::DescribeUndoCommand(History().UndoAvailability(), History().TopUndo(), History().BaseReason());
}

EditorHistory::HistoryCommandText EditorLayer::RedoCommandText() const
{
    return EditorHistory::DescribeRedoCommand(History().RedoAvailability(), History().TopRedo());
}

// ---------------------------------------------------------------- default entities

void EditorLayer::AdoptDefaultEntitiesFromLoadedScene()
{
    m_PrototypeMeshEntity = m_ActiveScene.FindEntityByName("Prototype Mesh");
    m_DirectionalLightEntity = m_ActiveScene.FindEntityByName("Directional Light");
    m_PlayerStartEntity = m_ActiveScene.FindEntityByName("Player Start");
}

// ---------------------------------------------------------------- Edit menu

void EditorLayer::DrawEditMenu()
{
    if (!ImGui::BeginMenu("Edit"))
        return;

    const EditorHistory::HistoryCommandText undo = UndoCommandText();
    if (ImGui::MenuItem(undo.Label.c_str(), "Ctrl+Z", false, undo.Enabled))
        Undo();
    if (!undo.Enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", undo.Reason.c_str());

    const EditorHistory::HistoryCommandText redo = RedoCommandText();
    if (ImGui::MenuItem(redo.Label.c_str(), "Ctrl+Y", false, redo.Enabled))
        Redo();
    if (!redo.Enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", redo.Reason.c_str());

    ImGui::Separator();
    ImGui::MenuItem("History", nullptr, &m_PanelVisible[PanelHistory]);
    const bool canClear = History().EntryCount() != 0;
    if (ImGui::MenuItem("Clear History...", nullptr, false, canClear))
        m_ShowClearHistoryPopup = true;
    if (!canClear && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("There is no history to clear");
    ImGui::EndMenu();
}

void EditorLayer::DrawClearHistoryPopup()
{
    if (m_ShowClearHistoryPopup)
    {
        ImGui::OpenPopup("Clear History");
        m_ShowClearHistoryPopup = false;
    }
    if (!ImGui::BeginPopupModal("Clear History", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::Text("Remove all %zu history entries?", History().EntryCount());
    ImGui::TextDisabled("This cannot be undone. The project itself is not changed.");
    if (ImGui::Button("Clear History"))
    {
        FlushEditGesture();
        const size_t cleared = History().EntryCount();
        History().Reset(EditorHistory::MakeHistoryLabel("History cleared", "", EditorHistory::HistorySource::System));
        m_LastEvictionNotice.clear();
        AnnounceHistory("History cleared (" + std::to_string(cleared) + (cleared == 1 ? " entry)" : " entries)"));
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void EditorLayer::DrawHistoryStatus()
{
    const std::string text = HistoryStatusText();
    if (text.empty())
        return;
    const float width = ImGui::CalcTextSize(text.c_str()).x;
    const float cursor = ImGui::GetWindowWidth() - width - ImGui::GetStyle().WindowPadding.x * 2.0f;
    if (cursor > ImGui::GetCursorPosX())
        ImGui::SetCursorPosX(cursor);
    ImGui::TextUnformatted(text.c_str());
}

// ---------------------------------------------------------------- History panel

void EditorLayer::DrawHistoryPanel()
{
    if (!BeginClosablePanel(PanelHistory, "History"))
        return;

    const EditorHistory::HistoryStore<HistoryState>& history = History();
    const std::string used = EditorHistory::FormatHistoryBytes(history.UsedBytes());
    const std::string budget = EditorHistory::FormatHistoryBytes(history.Config().BudgetBytes);
    ImGui::Text("%zu %s, %s of %s", history.EntryCount(), history.EntryCount() == 1 ? "entry" : "entries",
        used.c_str(), budget.c_str());
    ImGui::SameLine();
    const bool gestureOpen = history.GestureOpen();
    ImGui::BeginDisabled(history.EntryCount() == 0 || gestureOpen);
    if (ImGui::SmallButton("Clear..."))
        m_ShowClearHistoryPopup = true;
    ImGui::EndDisabled();
    if (!m_LastEvictionNotice.empty())
        ImGui::TextWrapped("%s", m_LastEvictionNotice.c_str());
    ImGui::Separator();

    const std::vector<EditorHistory::HistoryRow> rows = history.Rows();
    ImGui::BeginDisabled(gestureOpen);
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
        | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("HistoryRows", 4, tableFlags))
    {
        ImGui::TableSetupScrollFreeze(0, 0);
        ImGui::TableSetupColumn("##marker", ImGuiTableColumnFlags_WidthFixed, 18.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableHeadersRow();
        size_t clickedRow = rows.size();
        for (size_t reverse = 0; reverse < rows.size(); ++reverse)
        {
            const size_t index = rows.size() - 1 - reverse;
            const EditorHistory::HistoryRow& row = rows[index];
            ImGui::PushID(static_cast<int>(index));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(row.Current ? ">" : "");
            ImGui::TableSetColumnIndex(1);
            std::string text = row.Display;
            if (!row.Applied)
                text += " (undone)";
            if (!row.Applied)
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushStyleColor(ImGuiCol_Header, kSelectionColor);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kSelectionHoverColor);
            if (ImGui::Selectable(text.c_str(), row.Current, ImGuiSelectableFlags_SpanAllColumns))
                clickedRow = index;
            ImGui::PopStyleColor(2);
            if (m_HistoryRowProbe)
                m_HistoryRowProbe(index, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (!row.Applied)
                ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(2);
            if (row.Label.Source != EditorHistory::HistorySource::User)
                ImGui::TextUnformatted(EditorHistory::HistorySourceName(row.Label.Source));
            ImGui::TableSetColumnIndex(3);
            if (!row.IsBase)
                ImGui::TextUnformatted(EditorHistory::FormatHistoryBytes(row.Bytes).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (clickedRow < rows.size() && !rows[clickedRow].Current)
            JumpToHistoryRow(clickedRow);
    }
    ImGui::EndDisabled();
    ImGui::End();
}

// ---------------------------------------------------------------- shortcuts

bool EditorLayer::IsHistoryModalOpen() const
{
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

void EditorLayer::PollHistoryShortcuts()
{
    const ImGuiIO& io = ImGui::GetIO();
    EditorHistory::ShortcutContext context;
    context.WindowFocused = m_WindowFocused;
    context.BrowserOwnsKeyboard = m_FabBrowser.WantsKeyboard();
    context.TextInputActive = io.WantTextInput;
    context.ModalOpen = IsHistoryModalOpen();
    context.DragActive = m_EditGestureTracker.Open() || History().GestureOpen() || ImGui::IsAnyItemActive();

    const struct
    {
        ImGuiKey Key;
        EditorHistory::ShortcutKey Mapped;
    } keys[] = { { ImGuiKey_Z, EditorHistory::ShortcutKey::Z }, { ImGuiKey_Y, EditorHistory::ShortcutKey::Y } };
    for (const auto& entry : keys)
    {
        const bool initialPress = ImGui::IsKeyPressed(entry.Key, false);
        const bool repeatPress = !initialPress && ImGui::IsKeyPressed(entry.Key, true);
        if (!initialPress && !repeatPress)
            continue;
        EditorHistory::ShortcutChord chord;
        chord.Key = entry.Mapped;
        chord.Ctrl = io.KeyCtrl;
        chord.Shift = io.KeyShift;
        chord.Alt = io.KeyAlt;
        chord.Repeat = repeatPress;
        const EditorHistory::ShortcutDecision decision = EditorHistory::ResolveShortcut(chord, context);
        if (decision.Action == EditorHistory::ShortcutAction::Undo)
            Undo();
        else if (decision.Action == EditorHistory::ShortcutAction::Redo)
            Redo();
        else if (decision.Reason == EditorHistory::ShortcutReason::ModalOpen
            || decision.Reason == EditorHistory::ShortcutReason::DragActive)
            AnnounceHistory(EditorHistory::DescribeShortcutBlock(decision.Reason));
    }
}

// ---------------------------------------------------------------- typed control

bool EditorLayer::ActionNeedsIdleHistory(EditorMaterialControlAction action)
{
    switch (action)
    {
    case EditorMaterialControlAction::SelectEntityPatchMaterialSurface:
    case EditorMaterialControlAction::SelectEntity:
    case EditorMaterialControlAction::SetEntityTransform:
    case EditorMaterialControlAction::SetTypedLight:
    case EditorMaterialControlAction::SetProjectColorPipeline:
    case EditorMaterialControlAction::SetViewportMainCameraPose:
    case EditorMaterialControlAction::SetMeshRendererFlags:
    case EditorMaterialControlAction::UndoHistory:
    case EditorMaterialControlAction::RedoHistory:
    case EditorMaterialControlAction::CommitFabImport:
    case EditorMaterialControlAction::PlaceMeshAsset:
    case EditorMaterialControlAction::SetEntityMeshRendererAssets:
        return true;
    default:
        return false;
    }
}

void EditorLayer::FillHistoryReceiptBlock(EditorHistoryControlReceipt& block, bool includeRows) const
{
    const HistoryStoreType& history = History();
    block = {};
    block.RevisionBefore = history.HeadRevision();
    block.Cursor = history.Cursor();
    block.UndoDepth = history.UndoDepth();
    block.RedoDepth = history.RedoDepth();
    block.EntryCount = history.EntryCount();
    block.GestureOpen = history.GestureOpen();
    block.BaseIsBarrier = history.BaseIsBarrier();
    block.UsedBytes = history.UsedBytes();
    block.BudgetBytes = history.Config().BudgetBytes;
    block.MaximumEntries = history.Config().MaximumEntries;
    block.EvictedEntries = history.Evicted().Entries;
    block.EvictedBytes = history.Evicted().Bytes;
    block.EvictionEvents = history.Evicted().Events;
    const EditorHistory::HistoryCommandText undo = UndoCommandText();
    const EditorHistory::HistoryCommandText redo = RedoCommandText();
    block.UndoEnabled = undo.Enabled;
    block.RedoEnabled = redo.Enabled;
    block.UndoLabel = undo.Label;
    block.UndoReason = undo.Reason;
    block.RedoLabel = redo.Label;
    block.RedoReason = redo.Reason;
    block.Announcement = m_LastHistoryAnnouncement;
    block.EvictionNotice = m_LastEvictionNotice;
    if (!includeRows)
        return;
    const std::vector<EditorHistory::HistoryRow> rows = history.Rows();
    block.RowTotal = rows.size();
    const size_t emitted = std::min(rows.size(), EditorMaterialControlMailbox::MaximumHistoryRows);
    block.Rows.reserve(emitted);
    for (size_t reverse = 0; reverse < emitted; ++reverse)
    {
        const size_t index = rows.size() - 1 - reverse;
        const EditorHistory::HistoryRow& row = rows[index];
        EditorHistoryRowReceipt out;
        out.Index = index;
        out.Revision = row.Revision;
        out.Display = row.Display;
        out.Verb = row.Label.Verb;
        out.Target = row.Label.Target;
        out.Source = EditorHistory::HistorySourceName(row.Label.Source);
        out.Bytes = row.Bytes;
        out.Applied = row.Applied;
        out.Current = row.Current;
        out.Base = row.IsBase;
        out.Barrier = row.IsBarrier;
        block.Rows.push_back(std::move(out));
    }
}

EditorMaterialControlTransaction EditorLayer::ExecuteEditorMaterialControlRequest(
    const EditorMaterialControlRequest& request, Engine::u64 frame)
{
    const Engine::u64 headBefore = History().HeadRevision();
    EditorMaterialControlTransaction transaction = ExecuteEditorMaterialControlRequestCore(request, frame);
    if (!transaction.Mutating)
    {
        // A request that finished inside the handler (every read, every rejection, the
        // Fab commit) reports the history as it ended. A staged mutation keeps the block
        // it was staged with, because its receipt is written before it commits.
        FillHistoryReceiptBlock(transaction.Receipt.History,
            request.Action == EditorMaterialControlAction::InspectHistory && transaction.Receipt.Succeeded);
        transaction.Receipt.History.RevisionBefore = headBefore;
        transaction.Receipt.History.RevisionAfter = History().HeadRevision();
    }
    return transaction;
}

EditorMaterialControlTransaction EditorLayer::ExecuteHistoryControlRequest(
    const EditorMaterialControlRequest& request, EditorMaterialControlTransaction transaction)
{
    EditorMaterialControlReceipt& receipt = transaction.Receipt;
    const auto reject = [&transaction, &receipt](std::string reason)
    {
        receipt.Succeeded = false;
        receipt.Reason = std::move(reason);
        return std::move(transaction);
    };

    if (request.Action == EditorMaterialControlAction::InspectHistory)
    {
        receipt.Succeeded = true;
        receipt.Reason = "ok";
        receipt.Effect = "ReadOnly";
        receipt.PostconditionVerified = true;
        return transaction;
    }

    const bool undo = request.Action == EditorMaterialControlAction::UndoHistory;
    if (!request.HasExpectedHistoryRevision || request.ExpectedHistoryRevision != History().HeadRevision())
        return reject("history_revision_mismatch");
    const EditorHistory::HistoryAvailability availability = undo ? History().UndoAvailability() : History().RedoAvailability();
    if (!availability.Enabled)
    {
        switch (availability.Block)
        {
        case EditorHistory::HistoryBlock::NothingToUndo: return reject("nothing_to_undo");
        case EditorHistory::HistoryBlock::NothingToRedo: return reject("nothing_to_redo");
        case EditorHistory::HistoryBlock::UndoBarrier: return reject("undo_barrier");
        default: return reject("history_gesture_open");
        }
    }

    // Row i is the project after i applied entries (row 0 the base), so the head
    // after one step is the revision of the neighbouring row.
    const std::vector<EditorHistory::HistoryRow> rows = History().Rows();
    const size_t cursor = History().Cursor();
    const size_t targetRow = undo ? cursor - 1 : cursor + 1;
    if (targetRow >= rows.size())
        return reject("history_revision_mismatch");
    const EditorHistory::HistoryEntryInfo* top = undo ? History().TopUndo() : History().TopRedo();
    if (!top)
        return reject("history_revision_mismatch");
    const std::string expectedAnnouncement = std::string(undo ? "Undo: " : "Redo: ") + top->Label.Display();
    const Engine::u64 predictedHead = rows[targetRow].Revision;
    const size_t undoDepthBefore = History().UndoDepth();
    const size_t redoDepthBefore = History().RedoDepth();

    struct HistoryRollback
    {
        HistorySnapshot State;
        HistoryStoreType::Mark Mark;
        Engine::u64 HeadRevision = 0;
        Engine::u64 CameraEpoch = 0;
        bool FusionPivotValid = false;
        Engine::Math::DVec3 FusionPivot;
        bool ViewportDiscontinuousRelocationPending = false;
        bool MutationStarted = false;
    };
    auto rollback = std::make_shared<HistoryRollback>();
    rollback->State = CaptureHistorySnapshot();
    rollback->Mark = History().SaveMark();
    rollback->HeadRevision = History().HeadRevision();
    rollback->CameraEpoch = m_CameraEpoch;
    rollback->FusionPivotValid = m_FusionNavigationPivotValid;
    rollback->FusionPivot = m_FusionNavigationPivot;
    rollback->ViewportDiscontinuousRelocationPending = m_ViewportDiscontinuousRelocationPending;
    if (!rollback->Mark.Valid)
        return reject("history_gesture_open");

    receipt.Succeeded = true;
    receipt.Reason = "ok";
    receipt.Effect = undo ? "HistoryUndone" : "HistoryRedone";
    receipt.Recovery = undo ? "RedoHistory" : "UndoHistory";
    receipt.UndoDepthAfter = undo ? undoDepthBefore - 1 : undoDepthBefore + 1;
    receipt.RedoDepthAfter = undo ? redoDepthBefore + 1 : redoDepthBefore - 1;
    receipt.History.RevisionAfter = predictedHead;
    receipt.History.Announcement = expectedAnnouncement;
    receipt.PostconditionVerified = true;
    transaction.Mutating = true;
    transaction.Commit = [this, undo, rollback, expectedAnnouncement, predictedHead, undoDepthBefore, redoDepthBefore](
                             std::string& error)
    {
        rollback->MutationStarted = true;
        const bool moved = undo ? Undo() : Redo();
        if (!moved)
        {
            error = "history_restore_failed";
            return false;
        }
        if (History().HeadRevision() != predictedHead || m_LastHistoryAnnouncement != expectedAnnouncement
            || History().UndoDepth() != (undo ? undoDepthBefore - 1 : undoDepthBefore + 1)
            || History().RedoDepth() != (undo ? redoDepthBefore + 1 : redoDepthBefore - 1))
        {
            error = "history_postcondition_mismatch";
            return false;
        }
        return true;
    };
    transaction.Rollback = [this, rollback](EditorMaterialControlReceipt& rolledBack)
    {
        const bool restored = !rollback->MutationStarted || RestoreHistoryStateExact(*rollback->State);
        const bool historyRestored = History().LoadMark(rollback->Mark);
        m_CameraEpoch = rollback->CameraEpoch;
        m_FusionNavigationPivotValid = rollback->FusionPivotValid;
        m_FusionNavigationPivot = rollback->FusionPivot;
        m_ViewportDiscontinuousRelocationPending = rollback->ViewportDiscontinuousRelocationPending;
        rolledBack.UndoDepthAfter = History().UndoDepth();
        rolledBack.RedoDepthAfter = History().RedoDepth();
        rolledBack.SelectedEntityIdAfter = m_SelectedEntity.Id;
        FillHistoryReceiptBlock(rolledBack.History, false);
        rolledBack.History.RevisionAfter = rolledBack.History.RevisionBefore;
        return restored && historyRestored && History().HeadRevision() == rollback->HeadRevision
            && m_SelectedEntity == rollback->State->SelectedEntity;
    };
    return transaction;
}
