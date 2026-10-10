#include "UiViewportTests.h"

#include "TestSupport/PropertyRunner.h"

#include "Engine/Renderer/PresentationSerialLedger.h"
#include "Engine/Renderer/UiViewportPolicy.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace SpiralTests
{
    namespace
    {
        using namespace Engine;
        using Ledger = PresentationSerialLedger;
        using Fence = Ledger::FenceState;

        // Behavioral contract: a detached OS window is enabled only when requested,
        // supported by renderer + platform + backends, and the surface offers a
        // present mode that cannot block the process (MAILBOX or IMMEDIATE); and the
        // presentation serial that retires UI textures accounts for every fence of
        // every swapchain that drew in that ImGui frame.
        //
        // Failure hypotheses: a FIFO (or FIFO_RELAXED/unknown) mode slipping into the
        // allowed set; a denial row reporting the wrong reason or a later reason
        // masking an earlier one; the completed serial ignoring a detached
        // swapchain's pending fence (texture freed while a detached draw still reads
        // it); a serial lost or double-issued when the main submission is skipped,
        // when a viewport is destroyed, or when a swapchain is rebuilt.
        //
        // Oracles: hand tables written from the contract; a set-membership
        // formulation of the mode table; an exhaustive enumeration of the decision
        // input space against a boolean conjunction; and for the ledger a
        // brute-force model that keeps every submission ever made and recomputes the
        // completed serial from scratch. Generated cases replay from
        // SPIRAL_UI_VIEWPORT_SEED / SPIRAL_UI_VIEWPORT_REPLAY.
        //
        // Tier: fast, in-process. Not claimed: any GPU, driver, compositor or
        // real-fence behaviour.

        struct Checker
        {
            const char* Suite;
            bool Ok = true;

            void Expect(bool condition, const std::string& message)
            {
                if (!condition)
                {
                    std::cerr << "UI viewport test failed [" << Suite << "]: " << message << '\n';
                    Ok = false;
                }
            }
        };

        bool RunProperty(std::string_view name, const Spiral::Tests::Property& property)
        {
            return Spiral::Tests::RunNamedProperty("ui-viewport", name, "SPIRAL_UI_VIEWPORT", 500, property);
        }

        std::string ModesToString(const std::vector<int>& modes)
        {
            std::string text = "{";
            for (int mode : modes)
                text += std::to_string(mode) + ",";
            return text + "}";
        }

        constexpr int kImmediate = UiViewportPresentModeValue::Immediate;
        constexpr int kMailbox = UiViewportPresentModeValue::Mailbox;
        constexpr int kFifo = UiViewportPresentModeValue::Fifo;
        constexpr int kFifoRelaxed = UiViewportPresentModeValue::FifoRelaxed;
        constexpr int kSharedDemandRefresh = 1000111000;
        constexpr int kFifoLatestReady = 1000361000;
    }

    bool TestUiViewportPresentModeSelection()
    {
        Checker check { "present-mode" };

        // Hand table. Rows: supported modes -> selected mode. The surface this
        // feature was measured on offers FIFO and IMMEDIATE (plus a latest-ready
        // mode): it must select IMMEDIATE, never FIFO.
        struct Row
        {
            std::vector<int> Supported;
            UiViewportSecondaryMode Expected;
        };
        const Row rows[] = {
            { {}, UiViewportSecondaryMode::None },
            { { kFifo }, UiViewportSecondaryMode::None },
            { { kFifo, kFifoRelaxed }, UiViewportSecondaryMode::None },
            { { kFifo, kImmediate, kFifoLatestReady }, UiViewportSecondaryMode::Immediate },
            { { kFifo, kMailbox }, UiViewportSecondaryMode::Mailbox },
            { { kMailbox, kImmediate, kFifo }, UiViewportSecondaryMode::Mailbox },
            { { kImmediate }, UiViewportSecondaryMode::Immediate },
            { { kSharedDemandRefresh, kFifoRelaxed }, UiViewportSecondaryMode::None },
        };
        for (const Row& row : rows)
            check.Expect(SelectUiViewportSecondaryMode(row.Supported) == row.Expected,
                "selection for " + ModesToString(row.Supported));

        // Resolution rows: backend choice x supported set.
        struct ResolveRow
        {
            int BackendChosen;
            std::vector<int> Supported;
            UiViewportSecondaryAction Action;
            int Mode;
        };
        const ResolveRow resolveRows[] = {
            { kImmediate, { kFifo, kImmediate }, UiViewportSecondaryAction::Keep, kImmediate },
            { kMailbox, { kFifo, kMailbox, kImmediate }, UiViewportSecondaryAction::Keep, kMailbox },
            { kFifo, { kFifo, kImmediate }, UiViewportSecondaryAction::Override, kImmediate },
            { kFifo, { kFifo, kMailbox, kImmediate }, UiViewportSecondaryAction::Override, kMailbox },
            { kFifoRelaxed, { kFifoRelaxed, kMailbox }, UiViewportSecondaryAction::Override, kMailbox },
            { kFifo, { kFifo }, UiViewportSecondaryAction::Skip, kFifo },
            { kFifo, {}, UiViewportSecondaryAction::Skip, kFifo },
            // A backend choice the surface does not offer is not kept.
            { kMailbox, { kFifo, kImmediate }, UiViewportSecondaryAction::Override, kImmediate },
            { kMailbox, { kFifo }, UiViewportSecondaryAction::Skip, kMailbox },
        };
        for (const ResolveRow& row : resolveRows)
        {
            const UiViewportSecondaryResolution resolution = ResolveUiViewportSecondaryMode(row.BackendChosen, row.Supported);
            check.Expect(resolution.Action == row.Action && (row.Action == UiViewportSecondaryAction::Skip || resolution.Mode == row.Mode),
                "resolution for backend " + std::to_string(row.BackendChosen) + " in " + ModesToString(row.Supported));
        }

        check.Expect(IsUiViewportSecondaryPresentModeAllowed(kMailbox) && IsUiViewportSecondaryPresentModeAllowed(kImmediate)
                && !IsUiViewportSecondaryPresentModeAllowed(kFifo) && !IsUiViewportSecondaryPresentModeAllowed(kFifoRelaxed)
                && !IsUiViewportSecondaryPresentModeAllowed(kSharedDemandRefresh) && !IsUiViewportSecondaryPresentModeAllowed(kFifoLatestReady)
                && !IsUiViewportSecondaryPresentModeAllowed(-1),
            "allowed set is exactly MAILBOX and IMMEDIATE");

        // Generated: arbitrary subsets of the known modes plus unknown values. The
        // oracle is plain membership of 0 or 1; FIFO presence must never matter.
        const int universe[] = { kImmediate, kMailbox, kFifo, kFifoRelaxed, kSharedDemandRefresh, kFifoLatestReady, 77 };
        const bool generated = RunProperty("present-mode-membership", [&](Spiral::Tests::ChoiceStream& stream, std::string& message)
        {
            std::vector<int> supported;
            for (int mode : universe)
                if (stream.NextBool())
                    supported.push_back(mode);
            const int backendChosen = universe[stream.NextSize(0, std::size(universe) - 1)];
            const bool hasMailbox = std::count(supported.begin(), supported.end(), kMailbox) != 0;
            const bool hasImmediate = std::count(supported.begin(), supported.end(), kImmediate) != 0;

            const UiViewportSecondaryMode selected = SelectUiViewportSecondaryMode(supported);
            const UiViewportSecondaryMode expected = hasMailbox ? UiViewportSecondaryMode::Mailbox
                : hasImmediate ? UiViewportSecondaryMode::Immediate : UiViewportSecondaryMode::None;
            if (selected != expected)
            {
                message = "selection mismatch for " + ModesToString(supported);
                return false;
            }
            std::vector<int> withFifo = supported;
            withFifo.push_back(kFifo);
            if (SelectUiViewportSecondaryMode(withFifo) != selected)
            {
                message = "adding FIFO changed the selection for " + ModesToString(supported);
                return false;
            }
            const UiViewportSecondaryResolution resolution = ResolveUiViewportSecondaryMode(backendChosen, supported);
            if ((resolution.Action == UiViewportSecondaryAction::Skip) != (!hasMailbox && !hasImmediate))
            {
                message = "skip does not match availability for " + ModesToString(supported);
                return false;
            }
            if (resolution.Action != UiViewportSecondaryAction::Skip && !IsUiViewportSecondaryPresentModeAllowed(resolution.Mode))
            {
                message = "a blocking mode was kept or selected: " + std::to_string(resolution.Mode) + " for " + ModesToString(supported);
                return false;
            }
            if (resolution.Action == UiViewportSecondaryAction::Keep && resolution.Mode != backendChosen)
            {
                message = "keep changed the backend mode";
                return false;
            }
            return true;
        });
        return check.Ok && generated;
    }

    bool TestUiViewportCapabilityDecision()
    {
        Checker check { "capability" };

        const auto supportedInput = []
        {
            UiViewportCapabilityInput input;
            input.Requested = true;
            input.Renderer = UiViewportRendererKind::Vulkan;
            input.Platform = UiViewportPlatformKind::X11;
            input.PlatformBackendHasViewports = true;
            input.RendererBackendHasViewports = true;
            input.SurfaceModesKnown = true;
            input.SurfacePresentModes = { kFifo, kImmediate };
            return input;
        };
        const auto expectReason = [&](const UiViewportCapabilityInput& input, UiViewportReason reason, const char* label)
        {
            const UiViewportDecision decision = DecideUiViewports(input);
            check.Expect(decision.Reason == reason && decision.Enabled == (reason == UiViewportReason::Enabled),
                std::string(label) + " expected " + ToString(reason) + " got " + ToString(decision.Reason));
        };

        UiViewportCapabilityInput input = supportedInput();
        expectReason(input, UiViewportReason::Enabled, "baseline X11 Vulkan IMMEDIATE");
        input.Platform = UiViewportPlatformKind::Win32;
        expectReason(input, UiViewportReason::Enabled, "Win32 Vulkan");

        input = supportedInput();
        input.Requested = false;
        expectReason(input, UiViewportReason::NotRequested, "not requested");
        input.Renderer = UiViewportRendererKind::None; // later conditions must not mask the first one
        input.Platform = UiViewportPlatformKind::Wayland;
        expectReason(input, UiViewportReason::NotRequested, "not requested masks everything else");

        input = supportedInput();
        input.Renderer = UiViewportRendererKind::None;
        expectReason(input, UiViewportReason::NativeRendererUnavailable, "OpenGL2 fallback");
        input.Platform = UiViewportPlatformKind::Wayland;
        expectReason(input, UiViewportReason::NativeRendererUnavailable, "renderer before platform");

        input = supportedInput();
        input.Renderer = UiViewportRendererKind::D3D12;
        input.Platform = UiViewportPlatformKind::Win32;
        expectReason(input, UiViewportReason::RendererNotImplemented, "D3D12");

        input = supportedInput();
        input.Platform = UiViewportPlatformKind::Wayland;
        expectReason(input, UiViewportReason::PlatformWayland, "Wayland");
        for (UiViewportPlatformKind platform : { UiViewportPlatformKind::Unknown, UiViewportPlatformKind::Cocoa, UiViewportPlatformKind::Null })
        {
            input = supportedInput();
            input.Platform = platform;
            expectReason(input, UiViewportReason::PlatformUnsupported, ToString(platform));
        }

        input = supportedInput();
        input.PlatformBackendHasViewports = false;
        input.RendererBackendHasViewports = false;
        expectReason(input, UiViewportReason::PlatformBackendLacksViewports, "platform flag before renderer flag");
        input.PlatformBackendHasViewports = true;
        expectReason(input, UiViewportReason::RendererBackendLacksViewports, "renderer flag");

        input = supportedInput();
        input.SurfaceModesKnown = false;
        input.SurfacePresentModes = {};
        expectReason(input, UiViewportReason::PresentModesUnknown, "unknown modes");
        // Known flag is authoritative: a stale list without the flag is still unknown.
        input.SurfacePresentModes = { kImmediate };
        expectReason(input, UiViewportReason::PresentModesUnknown, "modes without the known flag");

        input = supportedInput();
        input.SurfacePresentModes = { kFifo };
        expectReason(input, UiViewportReason::NoNonBlockingPresentMode, "FIFO only");
        input.SurfacePresentModes = { kFifo, kFifoRelaxed, kFifoLatestReady };
        expectReason(input, UiViewportReason::NoNonBlockingPresentMode, "FIFO family only");
        input.SurfacePresentModes = {};
        expectReason(input, UiViewportReason::NoNonBlockingPresentMode, "known but empty");
        input.SurfacePresentModes = { kMailbox };
        expectReason(input, UiViewportReason::Enabled, "MAILBOX only");

        // Every reason has a stable id and a user-visible sentence, all distinct.
        std::set<std::string> ids;
        std::set<std::string> sentences;
        for (int value = 0; value <= static_cast<int>(UiViewportReason::RendererHandlersUnavailable); ++value)
        {
            const UiViewportReason reason = static_cast<UiViewportReason>(value);
            check.Expect(std::string(ToString(reason)) != "unknown" && std::string(Describe(reason)).size() > 20, "reason text for " + std::to_string(value));
            ids.insert(ToString(reason));
            sentences.insert(Describe(reason));
        }
        const size_t reasonCount = static_cast<size_t>(UiViewportReason::RendererHandlersUnavailable) + 1;
        check.Expect(ids.size() == reasonCount && sentences.size() == reasonCount, "reason ids and sentences are distinct");

        // Exhaustive enumeration against a conjunction oracle: enabled exactly when
        // every condition holds, and never otherwise.
        const std::vector<std::vector<int>> modeSets = { {}, { kFifo }, { kImmediate }, { kMailbox }, { kFifo, kImmediate }, { kFifoRelaxed, kFifoLatestReady } };
        size_t rows = 0;
        for (int requested = 0; requested < 2; ++requested)
            for (int renderer = 0; renderer < 3; ++renderer)
                for (int platform = 0; platform < 6; ++platform)
                    for (int platformFlag = 0; platformFlag < 2; ++platformFlag)
                        for (int rendererFlag = 0; rendererFlag < 2; ++rendererFlag)
                            for (int known = 0; known < 2; ++known)
                                for (const std::vector<int>& modes : modeSets)
                                {
                                    UiViewportCapabilityInput row;
                                    row.Requested = requested != 0;
                                    row.Renderer = static_cast<UiViewportRendererKind>(renderer);
                                    row.Platform = static_cast<UiViewportPlatformKind>(platform);
                                    row.PlatformBackendHasViewports = platformFlag != 0;
                                    row.RendererBackendHasViewports = rendererFlag != 0;
                                    row.SurfaceModesKnown = known != 0;
                                    row.SurfacePresentModes = modes;
                                    const bool hasNonBlocking = std::count(modes.begin(), modes.end(), kMailbox) != 0
                                        || std::count(modes.begin(), modes.end(), kImmediate) != 0;
                                    const bool expectedEnabled = requested != 0 && row.Renderer == UiViewportRendererKind::Vulkan
                                        && (row.Platform == UiViewportPlatformKind::X11 || row.Platform == UiViewportPlatformKind::Win32)
                                        && platformFlag != 0 && rendererFlag != 0 && known != 0 && hasNonBlocking;
                                    const UiViewportDecision decision = DecideUiViewports(row);
                                    ++rows;
                                    check.Expect(decision.Enabled == expectedEnabled && (decision.Reason == UiViewportReason::Enabled) == decision.Enabled,
                                        "enumerated row " + std::to_string(rows) + " gave " + ToString(decision.Reason));
                                }
        check.Expect(rows == 2u * 3u * 6u * 2u * 2u * 2u * modeSets.size(), "enumeration covered the whole input space");
        return check.Ok;
    }

    bool TestPresentationSerialLedgerHandCases()
    {
        Checker check { "ledger-hand" };
        std::map<std::pair<u32, u32>, Fence> fences;
        const auto probe = [&](const Ledger::Key& key)
        {
            const auto found = fences.find({ key.Owner, key.Slot });
            return found == fences.end() ? Fence::Complete : found->second;
        };

        // Main-only frames behave like the previous one-serial-per-submission model.
        {
            Ledger ledger;
            fences.clear();
            ledger.BeginFrame();
            check.Expect(ledger.Submitted() == 0 && ledger.PollCompleted(probe) == 0, "empty ledger");
            check.Expect(ledger.Track({ 0, 0 }) == 1 && ledger.Submitted() == 1, "first frame serial");
            fences[{ 0, 0 }] = Fence::Pending;
            check.Expect(ledger.PollCompleted(probe) == 0, "pending main fence holds serial 1");
            fences[{ 0, 0 }] = Fence::Complete;
            check.Expect(ledger.PollCompleted(probe) == 1 && ledger.InFlightCount() == 0, "signalled main fence completes serial 1 and is dropped");
        }

        // The hazard this ledger exists for: the main fence is done but the detached
        // window's fence of the same frame is not. Main-only accounting would say the
        // frame is complete and let a texture be freed under the detached draw.
        {
            Ledger ledger;
            fences.clear();
            ledger.BeginFrame();
            ledger.Track({ 0, 1 });
            fences[{ 0, 1 }] = Fence::Complete;
            check.Expect(ledger.Track({ 77, 0 }) == 1, "detached submission joins the frame serial");
            fences[{ 77, 0 }] = Fence::Pending;
            check.Expect(ledger.Submitted() == 1, "joining does not issue a second serial");
            check.Expect(ledger.PeekCompletedForOwnerOnly(0, probe) == 1, "main-only view claims the frame is complete");
            check.Expect(ledger.PollCompleted(probe) == 0, "detached pending fence holds the frame serial");
            fences[{ 77, 0 }] = Fence::Complete;
            check.Expect(ledger.PollCompleted(probe) == 1, "frame completes when the detached fence signals");
        }

        // An older frame's pending detached fence blocks every later serial.
        {
            Ledger ledger;
            fences.clear();
            ledger.BeginFrame();
            ledger.Track({ 0, 0 });
            ledger.Track({ 5, 0 });
            fences[{ 5, 0 }] = Fence::Pending;
            ledger.BeginFrame();
            ledger.Track({ 0, 1 });
            check.Expect(ledger.Submitted() == 2, "second frame serial");
            fences[{ 0, 1 }] = Fence::Complete;
            check.Expect(ledger.PollCompleted(probe) == 0, "frame 1 detached fence blocks frame 2 completion");
            fences[{ 5, 0 }] = Fence::Complete;
            check.Expect(ledger.PollCompleted(probe) == 2, "both complete");
        }

        // The main submission was skipped (acquire out of date): the first detached
        // submission allocates the frame serial, and the bound a texture released
        // earlier in that frame took (Submitted + 1) equals it.
        {
            Ledger ledger;
            fences.clear();
            ledger.BeginFrame();
            const u64 boundWhileOpen = ledger.Submitted() + 1;
            check.Expect(!ledger.FrameSerialAllocated(), "no serial before a submission");
            check.Expect(ledger.Track({ 9, 2 }) == boundWhileOpen, "detached-only frame carries the predicted serial");
        }
        // Main first: same equality.
        {
            Ledger ledger;
            ledger.BeginFrame();
            ledger.Track({ 0, 0 });
            ledger.BeginFrame();
            const u64 boundWhileOpen = ledger.Submitted() + 1;
            check.Expect(ledger.Track({ 0, 1 }) == boundWhileOpen && ledger.Track({ 4, 0 }) == boundWhileOpen, "main-first frame carries the predicted serial");
        }

        // A frame without any submission consumes no serial.
        {
            Ledger ledger;
            ledger.BeginFrame();
            ledger.BeginFrame();
            ledger.BeginFrame();
            check.Expect(ledger.Submitted() == 0, "empty frames issue no serial");
        }

        // Destroying a viewport (after the device wait) releases its entries; a
        // rebuilt main swapchain does the same for owner 0; Clear keeps the counter.
        {
            Ledger ledger;
            fences.clear();
            ledger.BeginFrame();
            ledger.Track({ 0, 0 });
            ledger.Track({ 3, 0 });
            ledger.Track({ 3, 1 });
            fences[{ 3, 0 }] = Fence::Pending;
            fences[{ 3, 1 }] = Fence::Pending;
            fences[{ 0, 0 }] = Fence::Complete;
            check.Expect(ledger.PollCompleted(probe) == 0, "viewport fences hold");
            ledger.ForgetOwner(3);
            check.Expect(ledger.InFlightCount() == 0 && ledger.PollCompleted(probe) == 1, "destroyed viewport no longer holds");
            ledger.BeginFrame();
            ledger.Track({ 0, 0 });
            ledger.Clear();
            check.Expect(ledger.Submitted() == 2 && ledger.InFlightCount() == 0 && ledger.PollCompleted(probe) == 2, "clear keeps serials monotonic");
            ledger.BeginFrame();
            check.Expect(ledger.Track({ 0, 0 }) == 3, "serial continues after clear");
        }

        // Re-tracking a key (its previous fence was waited on by the backend)
        // replaces the older serial instead of accumulating.
        {
            Ledger ledger;
            fences.clear();
            ledger.BeginFrame();
            ledger.Track({ 0, 0 });
            ledger.BeginFrame();
            ledger.Track({ 0, 0 });
            check.Expect(ledger.InFlightCount() == 1, "key replaced");
            fences[{ 0, 0 }] = Fence::Pending;
            check.Expect(ledger.PollCompleted(probe) == 1, "only the newest submission on a key holds");
        }
        return check.Ok;
    }

    namespace
    {
        struct ReferenceSubmission
        {
            u64 Serial = 0;
            int Fence = 0;
            u64 Frame = 0;
        };

        struct PendingRelease
        {
            u64 Bound = 0;
            size_t SubmissionsBefore = 0;
            u64 Frame = 0;
        };

        // Brute-force model: remembers every submission ever made, with its fence, and
        // recomputes the completed serial from scratch.
        struct ReferenceWorld
        {
            std::vector<ReferenceSubmission> All;
            std::vector<bool> Signalled;
            std::map<std::pair<u32, u32>, int> KeyFence;
            u64 Serial = 0;
            bool FrameHasSubmission = false;
            u64 FrameId = 0;

            u64 Completed() const
            {
                u64 oldestPending = 0;
                for (const ReferenceSubmission& submission : All)
                    if (!Signalled[static_cast<size_t>(submission.Fence)])
                        oldestPending = oldestPending == 0 ? submission.Serial : std::min(oldestPending, submission.Serial);
                return oldestPending == 0 ? Serial : oldestPending - 1;
            }
        };

        size_t g_HazardWitnesses = 0;
    }

    bool TestPresentationSerialLedgerMatchesReferenceModel()
    {
        Checker check { "ledger-model" };
        g_HazardWitnesses = 0;

        const bool ok = RunProperty("ledger-reference-model", [&](Spiral::Tests::ChoiceStream& stream, std::string& message)
        {
            Ledger ledger;
            ReferenceWorld world;
            std::vector<PendingRelease> releases;
            bool frameOpen = false;
            u64 lastCompleted = 0;
            const auto probe = [&](const Ledger::Key& key)
            {
                const auto found = world.KeyFence.find({ key.Owner, key.Slot });
                return found == world.KeyFence.end() || world.Signalled[static_cast<size_t>(found->second)] ? Fence::Complete : Fence::Pending;
            };
            const auto fail = [&](std::string text)
            {
                message = std::move(text);
                return false;
            };

            const auto submit = [&](u32 owner, u32 slot) -> bool
            {
                // The backends wait a key's previous fence before reusing the image;
                // the model honours that contract by signalling it first.
                if (const auto previous = world.KeyFence.find({ owner, slot }); previous != world.KeyFence.end())
                    world.Signalled[static_cast<size_t>(previous->second)] = true;
                const int fenceId = static_cast<int>(world.Signalled.size());
                world.Signalled.push_back(false);
                world.KeyFence[{ owner, slot }] = fenceId;
                const u64 returned = ledger.Track({ owner, slot });
                if (!world.FrameHasSubmission)
                {
                    world.FrameHasSubmission = true;
                    ++world.Serial;
                }
                if (returned != world.Serial)
                    return false;
                world.All.push_back({ world.Serial, fenceId, world.FrameId });
                return true;
            };
            const auto signalSome = [&]
            {
                const size_t count = stream.NextSize(0, 3);
                for (size_t index = 0; index < count && !world.KeyFence.empty(); ++index)
                {
                    auto it = world.KeyFence.begin();
                    std::advance(it, static_cast<std::ptrdiff_t>(stream.NextSize(0, world.KeyFence.size() - 1)));
                    world.Signalled[static_cast<size_t>(it->second)] = true;
                }
            };
            const auto destroyOwner = [&](u32 owner)
            {
                for (auto it = world.KeyFence.begin(); it != world.KeyFence.end();)
                {
                    if (it->first.first == owner)
                    {
                        world.Signalled[static_cast<size_t>(it->second)] = true; // the backend waits for the device first
                        it = world.KeyFence.erase(it);
                    }
                    else
                        ++it;
                }
                ledger.ForgetOwner(owner);
            };
            // Returns an error text, or empty.
            const auto verify = [&]() -> std::string
            {
                const u64 expected = world.Completed();
                const u64 actual = ledger.PollCompleted(probe);
                if (actual != expected)
                    return "completed serial " + std::to_string(actual) + " but the model says " + std::to_string(expected);
                if (actual < lastCompleted)
                    return "completed serial moved backwards";
                lastCompleted = actual;
                if (ledger.Submitted() != world.Serial)
                    return "submitted serial " + std::to_string(ledger.Submitted()) + " but the model says " + std::to_string(world.Serial);
                if (actual < ledger.PeekCompletedForOwnerOnly(0, probe))
                    ++g_HazardWitnesses;
                for (const PendingRelease& release : releases)
                {
                    for (size_t index = 0; index < world.All.size(); ++index)
                    {
                        const ReferenceSubmission& submission = world.All[index];
                        const bool couldReference = index < release.SubmissionsBefore || submission.Frame == release.Frame;
                        if (!couldReference)
                            continue;
                        if (submission.Serial > release.Bound)
                            return "release bound " + std::to_string(release.Bound) + " does not cover submission serial " + std::to_string(submission.Serial);
                        if (actual >= release.Bound && !world.Signalled[static_cast<size_t>(submission.Fence)])
                            return "release bound " + std::to_string(release.Bound) + " considered complete while a covered fence is pending (serial "
                                + std::to_string(submission.Serial) + ")";
                    }
                }
                return {};
            };

            const size_t frames = stream.NextSize(3, 14);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                ledger.BeginFrame();
                world.FrameHasSubmission = false;
                ++world.FrameId;
                frameOpen = true;

                // UI building: texture releases take the bridge's open-frame bound.
                const size_t openReleases = stream.NextSize(0, 2);
                for (size_t index = 0; index < openReleases; ++index)
                    releases.push_back({ ledger.Submitted() + (frameOpen ? 1u : 0u), world.All.size(), world.FrameId });
                signalSome();

                if (stream.NextSize(0, 9) != 0)
                {
                    if (!submit(0, static_cast<u32>(frame % 3)))
                        return fail("main submission returned the wrong serial");
                }
                frameOpen = false;
                if (std::string text = verify(); !text.empty())
                    return fail(text);

                const size_t detached = stream.NextSize(0, 3);
                for (size_t index = 0; index < detached; ++index)
                    if (!submit(static_cast<u32>(stream.NextSize(1, 3)), static_cast<u32>(stream.NextSize(0, 2))))
                        return fail("detached submission returned the wrong serial");
                if (std::string text = verify(); !text.empty())
                    return fail(text);

                signalSome();
                if (stream.NextSize(0, 7) == 0)
                    destroyOwner(static_cast<u32>(stream.NextSize(1, 3)));
                if (stream.NextSize(0, 14) == 0)
                    destroyOwner(0); // main swapchain rebuilt
                if (stream.NextSize(0, 1) == 0)
                    releases.push_back({ ledger.Submitted(), world.All.size(), 0 }); // released between frames: bound is Submitted
                if (std::string text = verify(); !text.empty())
                    return fail(text);
            }

            // Everything finishes: all serials complete and every release is free.
            for (auto& [key, fence] : world.KeyFence)
                world.Signalled[static_cast<size_t>(fence)] = true;
            if (std::string text = verify(); !text.empty())
                return fail(text);
            if (ledger.PollCompleted(probe) != ledger.Submitted() || ledger.InFlightCount() != 0)
                return fail("ledger did not drain after every fence signalled");
            return true;
        });
        // The generator must reach the interesting state: some polls where a
        // detached fence, not the main one, held the completed serial back.
        check.Expect(g_HazardWitnesses > 0, "generated campaign never produced a detached-held completed serial");
        return check.Ok && ok;
    }
}
