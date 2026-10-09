#include "BrowserPanelTests.h"

#include "BrowserPanelCore.h"
#include "Engine/Events/ApplicationEvent.h"
#include "Engine/Events/KeyEvent.h"
#include "Engine/Events/MouseEvent.h"
#include "TestSupport/FakeBrowserSurface.h"
#include "TestSupport/GeneratedTest.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace Fab;
    using Bytes = std::vector<Engine::u8>;
    using Engine::u32;
    using Engine::u64;
    using Engine::u8;
    using Engine::UiTextureError;
    using Engine::UiTextureHandle;

    // Failure hypotheses, oracles, and non-claims for the whole file:
    // - The units under test are the pure halves of the Fab panel: the lifecycle
    //   state machine, the frame uploader (dirty-region retry, replace-then-swap
    //   resize), the view-size debounce, the download queue, and the input
    //   routing glue. The panel's ImGui drawing, the dlopen loader, and the
    //   Renderer statics are NOT exercised here.
    // - Oracles are independent of the code under test: a fake UI-texture
    //   provider that owns its own texel memory and validates every update's
    //   rectangle, pitch, and readable-byte count; a per-pixel BGRA to RGBA
    //   reference; a call log recorded by a decorator around FakeBrowserSurface;
    //   and a hand-written table for the debounce arithmetic.
    // - The generated property is deterministic (fixed seed), replayable through
    //   SPIRAL_FAB_PANEL_SEED / SPIRAL_FAB_PANEL_REPLAY, and prints the seed, the
    //   original and minimised choice traces, and a counterexample JSON in the
    //   system temp directory. Tier: fast, in-process, no GPU, no CEF, no ImGui
    //   context, no network, no real clock.
    // - Not claimed: real GPU texture behaviour or backpressure timing, live CEF
    //   frames, GLFW/ImGui event delivery and hit-testing, DPI, IME, clipboard.

    struct Checker
    {
        const char* Suite;
        bool Ok = true;

        void Expect(bool condition, const std::string& message)
        {
            if (!condition)
            {
                std::cerr << "Fab browser panel test failed [" << Suite << "]: " << message << '\n';
                Ok = false;
            }
        }
    };

    bool Contains(const std::string& text, std::string_view needle)
    {
        return text.find(needle) != std::string::npos;
    }

    // ---- independent pixel oracle -----------------------------------------------------

    Bytes ReferenceRgba(const Bytes& bgra, u32 width, u32 height)
    {
        Bytes rgba(static_cast<size_t>(width) * height * 4);
        for (u32 y = 0; y < height; ++y)
        {
            for (u32 x = 0; x < width; ++x)
            {
                const size_t index = (static_cast<size_t>(y) * width + x) * 4;
                rgba[index + 0] = bgra[index + 2];
                rgba[index + 1] = bgra[index + 1];
                rgba[index + 2] = bgra[index + 0];
                rgba[index + 3] = bgra[index + 3];
            }
        }
        return rgba;
    }

    Bytes PatternBgra(u32 width, u32 height, u8 seed)
    {
        Bytes bgra(static_cast<size_t>(width) * height * 4);
        for (size_t index = 0; index < bgra.size(); ++index)
            bgra[index] = static_cast<u8>(index * 7 + seed);
        return bgra;
    }

    BrowserFrameView MakeFrame(const Bytes& bgra, u32 width, u32 height, const std::vector<BrowserDirtyRect>& dirty, u64 sequence)
    {
        BrowserFrameView frame;
        frame.Bgra = bgra.data();
        frame.Width = width;
        frame.Height = height;
        frame.StrideBytes = width * 4;
        frame.Dirty = dirty;
        frame.Sequence = sequence;
        return frame;
    }

    // ---- fake UI-texture provider ----------------------------------------------------------

    class FakeUiTextures final : public IBrowserUiTextures
    {
    public:
        struct Texture
        {
            u32 Width = 0;
            u32 Height = 0;
            Bytes Pixels;
            bool Live = false;
        };

        std::vector<Texture> Items; // handle = index + 1
        u32 BackpressureCreates = 0;
        u32 BackpressureUpdates = 0;
        UiTextureError CreateFailure = UiTextureError::None;
        UiTextureError UpdateFailure = UiTextureError::None;
        size_t Creates = 0;
        size_t Updates = 0;
        size_t RejectedUpdates = 0;
        size_t Destroys = 0;
        std::vector<std::string>* EventLog = nullptr;

        UiTextureHandle Create(u32 width, u32 height, std::string_view) override
        {
            if (BackpressureCreates > 0)
            {
                --BackpressureCreates;
                m_Last = UiTextureError::UpdateBackpressure;
                return Engine::kInvalidUiTextureHandle;
            }
            if (CreateFailure != UiTextureError::None)
            {
                m_Last = CreateFailure;
                return Engine::kInvalidUiTextureHandle;
            }
            if (width == 0 || height == 0 || width > Engine::kMaximumUiTextureDimension || height > Engine::kMaximumUiTextureDimension)
            {
                m_Last = UiTextureError::InvalidSize;
                return Engine::kInvalidUiTextureHandle;
            }
            Items.push_back({ width, height, Bytes(static_cast<size_t>(width) * height * 4, 0), true });
            ++Creates;
            m_Last = UiTextureError::None;
            return Items.size();
        }

        bool Update(UiTextureHandle handle, const Engine::UiTextureUpdate& update) override
        {
            Texture* texture = Find(handle);
            if (texture == nullptr)
            {
                m_Last = UiTextureError::InvalidHandle;
                return false;
            }
            if (BackpressureUpdates > 0)
            {
                --BackpressureUpdates;
                ++RejectedUpdates;
                m_Last = UiTextureError::UpdateBackpressure;
                return false;
            }
            if (UpdateFailure != UiTextureError::None)
            {
                m_Last = UpdateFailure;
                return false;
            }
            const Engine::UiTextureRect& rect = update.Rect;
            if (update.Pixels == nullptr || rect.Width == 0 || rect.Height == 0 || rect.X + rect.Width > texture->Width
                || rect.Y + rect.Height > texture->Height)
            {
                m_Last = UiTextureError::InvalidUpdate;
                return false;
            }
            const u64 pitch = update.RowPitchBytes != 0 ? update.RowPitchBytes : static_cast<u64>(rect.Width) * 4;
            const u64 required = (rect.Height - 1) * pitch + static_cast<u64>(rect.Width) * 4;
            if (pitch < static_cast<u64>(rect.Width) * 4 || update.PixelBytes < required)
            {
                m_Last = UiTextureError::InvalidUpdate;
                return false;
            }
            const u8* source = static_cast<const u8*>(update.Pixels);
            for (u32 row = 0; row < rect.Height; ++row)
            {
                std::memcpy(&texture->Pixels[(static_cast<size_t>(rect.Y + row) * texture->Width + rect.X) * 4],
                    source + row * pitch, static_cast<size_t>(rect.Width) * 4);
            }
            ++Updates;
            m_Last = UiTextureError::None;
            return true;
        }

        bool Destroy(UiTextureHandle handle) override
        {
            Texture* texture = Find(handle);
            if (texture == nullptr)
            {
                m_Last = UiTextureError::InvalidHandle;
                return false;
            }
            texture->Live = false;
            texture->Pixels.clear();
            ++Destroys;
            if (EventLog != nullptr)
                EventLog->push_back("TextureDestroy");
            m_Last = UiTextureError::None;
            return true;
        }

        u64 GetImGuiId(UiTextureHandle handle) override { return Find(handle) != nullptr ? 0x1000 + handle : 0; }
        UiTextureError LastError() override { return m_Last; }

        size_t LiveCount() const
        {
            return static_cast<size_t>(std::count_if(Items.begin(), Items.end(), [](const Texture& item) { return item.Live; }));
        }

        const Texture* ByImGuiId(u64 id) const
        {
            if (id < 0x1001 || id - 0x1000 > Items.size())
                return nullptr;
            const Texture& texture = Items[id - 0x1001];
            return texture.Live ? &texture : nullptr;
        }

    private:
        Texture* Find(UiTextureHandle handle)
        {
            if (handle == 0 || handle > Items.size() || !Items[handle - 1].Live)
                return nullptr;
            return &Items[handle - 1];
        }

        UiTextureError m_Last = UiTextureError::None;
    };

    // ---- 1. view-size debounce -------------------------------------------------------------------

    bool CheckDebounce()
    {
        Checker check { "debounce" };
        BrowserViewDebouncer debouncer(150, 4096);
        BrowserViewSize out;

        check.Expect(debouncer.Update(1000, 640.0f, 360.0f, 1.0f, out) && out.Width == 640 && out.Height == 360 && out.DeviceScale == 1.0f,
            "the first size is applied at once");
        check.Expect(!debouncer.Update(1010, 640.0f, 360.0f, 1.0f, out), "an unchanged size applies nothing");

        // A splitter drag: a new size every 16 ms for 2 seconds never applies.
        u64 now = 2000;
        size_t applied = 0;
        for (int step = 0; step < 125; ++step)
        {
            now += 16;
            if (debouncer.Update(now, 640.0f + static_cast<float>(step + 1), 360.0f, 1.0f, out))
                ++applied;
        }
        check.Expect(applied == 0, "continuous resizing never reaches the browser");
        check.Expect(debouncer.HasPending() && debouncer.Applied().Width == 640, "the pending size is held, the applied size is unchanged");

        // It settles: exactly one application, after the settle time and not before.
        const float settled = 640.0f + 125.0f;
        check.Expect(!debouncer.Update(now + 149, settled, 360.0f, 1.0f, out), "149 ms after the last change is too early");
        check.Expect(debouncer.Update(now + 150, settled, 360.0f, 1.0f, out) && out.Width == 765, "150 ms of stability applies the size");
        check.Expect(!debouncer.Update(now + 400, settled, 360.0f, 1.0f, out) && !debouncer.HasPending(), "and only once");

        // Returning to the applied size cancels the pending change.
        check.Expect(!debouncer.Update(now + 500, 800.0f, 360.0f, 1.0f, out) && debouncer.HasPending(), "a new size becomes pending");
        check.Expect(!debouncer.Update(now + 520, settled, 360.0f, 1.0f, out) && !debouncer.HasPending(), "reverting cancels it");
        check.Expect(!debouncer.Update(now + 900, settled, 360.0f, 1.0f, out), "nothing is applied after a revert");

        // Scale: DIP = floor(pixels / scale); a scale change is a change and is debounced too.
        BrowserViewDebouncer scaled(150, 4096);
        check.Expect(scaled.Update(0, 1000.0f, 500.0f, 1.5f, out) && out.Width == 666 && out.Height == 333 && out.DeviceScale == 1.5f,
            "1000x500 pixels at 1.5 is 666x333 DIPs");
        check.Expect(!scaled.Update(10, 1000.0f, 500.0f, 2.0f, out), "a scale change waits for the settle time");
        check.Expect(scaled.Update(200, 1000.0f, 500.0f, 2.0f, out) && out.Width == 500 && out.Height == 250 && out.DeviceScale == 2.0f,
            "then applies the new DIP size and scale");

        // Limits: the physical size never exceeds the texture limit; hostile numbers stay finite and positive.
        BrowserViewDebouncer limited(0, 4096);
        check.Expect(limited.Update(0, 100000.0f, 100000.0f, 1.0f, out) && out.Width == 4096 && out.Height == 4096, "capped at 4096 pixels");
        BrowserViewDebouncer limitedScaled(0, 4096);
        check.Expect(limitedScaled.Update(0, 100000.0f, 100000.0f, 2.0f, out) && out.Width == 2048 && out.Height == 2048,
            "capped at 4096 physical pixels, i.e. 2048 DIPs at scale 2");
        BrowserViewDebouncer hostile(0, 4096);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        check.Expect(hostile.Update(0, nan, -5.0f, nan, out) && out.Width == 1 && out.Height == 1 && out.DeviceScale == 1.0f,
            "NaN and negative inputs clamp to 1x1 at scale 1");
        BrowserViewDebouncer extreme(0, 4096);
        check.Expect(extreme.Update(0, 640.0f, 360.0f, 1000.0f, out) && out.DeviceScale == 8.0f, "the scale is limited to 8");
        BrowserViewDebouncer tiny(0, 4096);
        check.Expect(tiny.Update(0, 640.0f, 360.0f, 0.0001f, out) && out.DeviceScale == 0.25f, "the scale is limited to 0.25");
        return check.Ok;
    }

    // ---- 2. uploader scenarios ----------------------------------------------------------------------

    bool CheckUploaderScenarios()
    {
        Checker check { "uploader" };
        FakeUiTextures textures;
        BrowserTextureUploader uploader(textures);

        // First frame: create, full upload, swap.
        Bytes bgra = PatternBgra(16, 16, 3);
        uploader.OnFrame(MakeFrame(bgra, 16, 16, { { 0, 0, 16, 16 } }, 1));
        check.Expect(!uploader.HasDisplayTexture() && uploader.Counters().FramesApplied == 1, "no texture before the first Service");
        uploader.Service();
        check.Expect(uploader.HasDisplayTexture() && uploader.DisplayWidth() == 16 && uploader.DisplayHeight() == 16
                && uploader.Counters().TexturesCreated == 1 && uploader.Counters().TextureSwaps == 1 && textures.LiveCount() == 1,
            "first Service creates, fills, and shows one 16x16 texture");
        const FakeUiTextures::Texture* shown = textures.ByImGuiId(uploader.DisplayId());
        check.Expect(shown != nullptr && shown->Pixels == ReferenceRgba(bgra, 16, 16), "the first upload equals the swizzled frame");
        check.Expect(!uploader.HasPendingWork(), "nothing is pending");

        // Backpressure keeps the dirty region and retries without a full upload.
        Bytes second = bgra;
        for (int y = 3; y < 8; ++y)
            for (int x = 2; x < 6; ++x)
                second[(static_cast<size_t>(y) * 16 + x) * 4 + 2] ^= 0x5a;
        uploader.OnFrame(MakeFrame(second, 16, 16, { { 2, 3, 4, 5 } }, 2));
        textures.BackpressureUpdates = 2;
        const size_t updatesBefore = textures.Updates;
        uploader.Service();
        check.Expect(textures.Updates == updatesBefore && uploader.PendingRectCount() == 1 && uploader.HasPendingWork(),
            "a rejected update leaves the region queued");
        uploader.Service();
        check.Expect(textures.Updates == updatesBefore && uploader.PendingRectCount() == 1, "still rejected, still queued");
        check.Expect(uploader.Counters().BackpressureRetries == 2 && !uploader.Failed(), "backpressure is transient, not a failure");

        // A later frame during the backlog merges into the queue; coverage never shrinks.
        Bytes third = second;
        for (int y = 12; y < 14; ++y)
            for (int x = 10; x < 12; ++x)
                third[(static_cast<size_t>(y) * 16 + x) * 4 + 0] ^= 0x33;
        uploader.OnFrame(MakeFrame(third, 16, 16, { { 10, 12, 2, 2 } }, 3));
        check.Expect(uploader.PendingRectCount() >= 1 && uploader.PendingRectCount() <= BrowserPanelLimits::kMaximumPendingRects,
            "the backlog stays bounded");
        const u64 texelsBefore = uploader.Counters().UpdatedTexels;
        uploader.Service();
        check.Expect(!uploader.HasPendingWork(), "the backlog drains once the Renderer accepts updates");
        check.Expect(uploader.Counters().UpdatedTexels - texelsBefore <= 16u * 16u, "the retry never uploads more than the frame");
        shown = textures.ByImGuiId(uploader.DisplayId());
        check.Expect(shown != nullptr && shown->Pixels == ReferenceRgba(third, 16, 16), "no dirty pixel was lost across the retries");
        check.Expect(uploader.Counters().TexturesCreated == 1, "retries never recreate the texture");

        // Resize with create backpressure: the old picture stays up until the new texture is full.
        const u64 oldId = uploader.DisplayId();
        Bytes resized = PatternBgra(24, 20, 9);
        uploader.OnFrame(MakeFrame(resized, 24, 20, { { 0, 0, 5, 5 } }, 4));
        check.Expect(uploader.FrameWidth() == 24 && uploader.DisplayWidth() == 16, "the frame size changed, the displayed texture did not");
        textures.BackpressureCreates = 1;
        uploader.Service();
        check.Expect(uploader.HasDisplayTexture() && uploader.DisplayId() == oldId && textures.LiveCount() == 1,
            "while the replacement is delayed the last good frame is still shown");
        textures.BackpressureUpdates = 1;
        uploader.Service();
        check.Expect(uploader.DisplayId() == oldId && textures.LiveCount() == 2 && uploader.HasPendingWork(),
            "a created replacement is not shown until its full upload is accepted");
        uploader.Service();
        check.Expect(uploader.DisplayWidth() == 24 && uploader.DisplayHeight() == 20 && uploader.DisplayId() != oldId && textures.LiveCount() == 1,
            "the replacement swaps in and the old texture is destroyed");
        shown = textures.ByImGuiId(uploader.DisplayId());
        check.Expect(shown != nullptr && shown->Pixels == ReferenceRgba(resized, 24, 20),
            "the whole new frame is in the replacement (a resize ignores the partial dirty list)");

        // A second resize while a replacement is pending discards the stale replacement.
        Bytes big = PatternBgra(30, 30, 1);
        Bytes bigger = PatternBgra(40, 12, 2);
        uploader.OnFrame(MakeFrame(big, 30, 30, { { 0, 0, 30, 30 } }, 5));
        textures.BackpressureUpdates = 1;
        uploader.Service(); // creates 30x30, upload rejected
        check.Expect(textures.LiveCount() == 2, "30x30 replacement pending");
        uploader.OnFrame(MakeFrame(bigger, 40, 12, { { 0, 0, 40, 12 } }, 6));
        uploader.Service();
        check.Expect(uploader.DisplayWidth() == 40 && uploader.DisplayHeight() == 12 && textures.LiveCount() == 1,
            "the stale 30x30 replacement was destroyed and the newest size shown");
        shown = textures.ByImGuiId(uploader.DisplayId());
        check.Expect(shown != nullptr && shown->Pixels == ReferenceRgba(bigger, 40, 12), "and its content is complete");

        // Shrinking back to the displayed size reuses the displayed texture instead of creating one.
        const size_t createsBefore = textures.Creates;
        uploader.OnFrame(MakeFrame(bigger, 40, 12, { { 0, 0, 40, 12 } }, 7));
        uploader.Service();
        check.Expect(textures.Creates == createsBefore, "a same-size frame needs no new texture");

        // A rejected frame changes nothing.
        BrowserFrameView invalid;
        const u64 appliedBefore = uploader.Counters().FramesApplied;
        uploader.OnFrame(invalid);
        check.Expect(uploader.Counters().FramesRejected == 1 && uploader.Counters().FramesApplied == appliedBefore
                && uploader.FrameWidth() == 40,
            "an invalid frame is counted and ignored");

        // Non-transient failure latches until a frame of a different size arrives.
        Bytes dirtyFrame = bigger;
        dirtyFrame[0] ^= 1;
        uploader.OnFrame(MakeFrame(dirtyFrame, 40, 12, { { 0, 0, 1, 1 } }, 8));
        textures.UpdateFailure = UiTextureError::UpdateRecordFailed;
        uploader.Service();
        check.Expect(uploader.Failed() && !uploader.HasDisplayTexture() && Contains(uploader.FailureText(), "UpdateRecordFailed")
                && Contains(uploader.FailureText(), "unavailable"),
            "a hard failure marks the texture unavailable and names the reason");
        const size_t updatesAtFailure = textures.Updates;
        uploader.OnFrame(MakeFrame(dirtyFrame, 40, 12, { { 0, 0, 1, 1 } }, 9));
        uploader.Service();
        uploader.Service();
        check.Expect(textures.Updates == updatesAtFailure && uploader.Counters().Failures == 1, "a latched failure stops further submissions and is counted once");
        textures.UpdateFailure = UiTextureError::None;
        Bytes recovered = PatternBgra(20, 20, 4);
        uploader.OnFrame(MakeFrame(recovered, 20, 20, { { 0, 0, 20, 20 } }, 10));
        check.Expect(!uploader.Failed(), "a frame of a different size clears the latch");
        uploader.Service();
        shown = textures.ByImGuiId(uploader.DisplayId());
        check.Expect(uploader.HasDisplayTexture() && shown != nullptr && shown->Pixels == ReferenceRgba(recovered, 20, 20) && textures.LiveCount() == 1,
            "and the panel recovers with a complete texture");

        // Create failure latches too, and never retries by itself.
        FakeUiTextures broken;
        broken.CreateFailure = UiTextureError::DeviceCreateFailed;
        BrowserTextureUploader failing(broken);
        failing.OnFrame(MakeFrame(recovered, 20, 20, { { 0, 0, 20, 20 } }, 1));
        failing.Service();
        failing.Service();
        check.Expect(failing.Failed() && !failing.HasDisplayTexture() && failing.Counters().Failures == 1 && broken.LiveCount() == 0,
            "a create failure is latched once");

        // Release destroys everything and is repeatable.
        uploader.Release();
        uploader.Release();
        check.Expect(textures.LiveCount() == 0 && !uploader.HasDisplayTexture(), "Release leaves no texture behind");
        return check.Ok;
    }

    // ---- 3. generated uploader property -----------------------------------------------------------------

    bool RunPanelProperty(std::string_view name, const Spiral::Tests::Property& property, size_t iterations)
    {
        Spiral::Tests::CampaignOptions options;
        options.Iterations = iterations;
        if (const char* seed = std::getenv("SPIRAL_FAB_PANEL_SEED"))
            options.Seed = std::strtoull(seed, nullptr, 10);
        Spiral::Tests::ChoiceTrace replay;
        if (const char* trace = std::getenv("SPIRAL_FAB_PANEL_REPLAY"); trace && Spiral::Tests::ParseTrace(trace, replay))
            options.Replay = replay;

        Spiral::Tests::Counterexample failure;
        if (Spiral::Tests::RunCampaign(options, property, failure))
            return true;

        const std::string minimized = Spiral::Tests::SerializeTrace(failure.MinimizedTrace);
        const std::string rerun = "SPIRAL_FAB_PANEL_SEED=" + std::to_string(failure.Seed) + " SPIRAL_FAB_PANEL_REPLAY=\"" + minimized
            + "\" EngineTests --test <registered name of " + std::string(name) + ">";
        const std::filesystem::path artifact = std::filesystem::temp_directory_path()
            / ("spiral-fab-panel-counterexample-" + std::string(name) + ".json");
        std::string artifactError;
        const bool written = Spiral::Tests::WriteCounterexample(artifact, name, failure, rerun, artifactError);
        std::cerr << "Fab browser panel property failed [" << name << "]: " << failure.Message << " seed=" << failure.Seed
                  << " iteration=" << failure.Iteration << " originalTrace=" << Spiral::Tests::SerializeTrace(failure.OriginalTrace)
                  << " minimizedTrace=" << minimized << "\n  rerun: " << rerun << '\n';
        if (written)
            std::cerr << "  counterexample: " << artifact.string() << '\n';
        else
            std::cerr << "  counterexample write failed: " << artifactError << '\n';
        return false;
    }

    // Random frames (a prefix of dirty changes, resizes), random Renderer
    // backpressure, random Service cadence. Invariants: at most two live textures;
    // a displayed texture is replaced only by one that already holds the whole
    // current frame; the display never disappears; and once the Renderer accepts
    // everything, the texture equals the latest frame exactly with one live texture.
    bool UploaderProperty(Spiral::Tests::ChoiceStream& stream, std::string& message)
    {
        FakeUiTextures textures;
        BrowserTextureUploader uploader(textures);

        u32 width = static_cast<u32>(stream.NextSize(4, 28));
        u32 height = static_cast<u32>(stream.NextSize(4, 28));
        Bytes bgra(static_cast<size_t>(width) * height * 4);
        for (u8& byte : bgra)
            byte = static_cast<u8>(stream.Next());
        u64 sequence = 0;
        uploader.OnFrame(MakeFrame(bgra, width, height, { { 0, 0, static_cast<int>(width), static_cast<int>(height) } }, ++sequence));

        bool everDisplayed = false;
        const size_t steps = stream.NextSize(6, 40);
        for (size_t step = 0; step < steps; ++step)
        {
            switch (stream.Next() % 5)
            {
            case 0: // an in-place change inside one to three rectangles
            {
                std::vector<BrowserDirtyRect> dirty;
                const size_t count = stream.NextSize(1, 3);
                for (size_t index = 0; index < count; ++index)
                {
                    BrowserDirtyRect rect;
                    rect.X = static_cast<int>(stream.NextSize(0, width - 1));
                    rect.Y = static_cast<int>(stream.NextSize(0, height - 1));
                    rect.Width = static_cast<int>(stream.NextSize(1, width - static_cast<size_t>(rect.X)));
                    rect.Height = static_cast<int>(stream.NextSize(1, height - static_cast<size_t>(rect.Y)));
                    for (int y = rect.Y; y < rect.Y + rect.Height; ++y)
                        for (int x = rect.X; x < rect.X + rect.Width; ++x)
                            for (int channel = 0; channel < 4; ++channel)
                                bgra[(static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4 + static_cast<size_t>(channel)] =
                                    static_cast<u8>(stream.Next());
                    dirty.push_back(rect);
                }
                uploader.OnFrame(MakeFrame(bgra, width, height, dirty, ++sequence));
                break;
            }
            case 1: // a resize
            {
                width = static_cast<u32>(stream.NextSize(4, 28));
                height = static_cast<u32>(stream.NextSize(4, 28));
                bgra.assign(static_cast<size_t>(width) * height * 4, 0);
                for (u8& byte : bgra)
                    byte = static_cast<u8>(stream.Next());
                uploader.OnFrame(MakeFrame(bgra, width, height, { { 0, 0, static_cast<int>(width), static_cast<int>(height) } }, ++sequence));
                break;
            }
            case 2:
                textures.BackpressureUpdates = static_cast<u32>(stream.NextSize(0, 3));
                textures.BackpressureCreates = static_cast<u32>(stream.NextSize(0, 2));
                break;
            default:
            {
                const u64 swapsBefore = uploader.Counters().TextureSwaps;
                uploader.Service();
                if (uploader.Counters().TextureSwaps != swapsBefore)
                {
                    const FakeUiTextures::Texture* shown = textures.ByImGuiId(uploader.DisplayId());
                    if (shown == nullptr || shown->Width != width || shown->Height != height
                        || shown->Pixels != ReferenceRgba(bgra, width, height))
                    {
                        message = "a texture was swapped in before it held the whole current frame";
                        return false;
                    }
                }
                break;
            }
            }
            if (textures.LiveCount() > 2)
            {
                message = "more than two live textures";
                return false;
            }
            if (everDisplayed && !uploader.HasDisplayTexture())
            {
                message = "the displayed picture disappeared";
                return false;
            }
            everDisplayed = everDisplayed || uploader.HasDisplayTexture();
        }

        textures.BackpressureUpdates = 0;
        textures.BackpressureCreates = 0;
        for (int round = 0; round < 64 && (uploader.HasPendingWork() || !uploader.HasDisplayTexture()); ++round)
            uploader.Service();

        if (uploader.Failed() || !uploader.HasDisplayTexture())
        {
            message = "the uploader did not reach a displayed texture after backpressure ended";
            return false;
        }
        if (textures.LiveCount() != 1)
        {
            message = "a texture leaked or was missing after quiescence";
            return false;
        }
        const FakeUiTextures::Texture* shown = textures.ByImGuiId(uploader.DisplayId());
        if (shown == nullptr || shown->Width != width || shown->Height != height || shown->Pixels != ReferenceRgba(bgra, width, height))
        {
            message = "the final texture does not equal the latest frame";
            return false;
        }
        return true;
    }

    // ---- session fixture for the core -----------------------------------------------------------------

    std::vector<std::string>* g_EventLog = nullptr;
    bool g_InSurfacePump = false;
    bool g_DestroyedInsidePump = false;

    void DestroySessionSurface(IBrowserSurface*)
    {
        if (g_EventLog != nullptr)
            g_EventLog->push_back("SurfaceDestroy");
        if (g_InSurfacePump)
            g_DestroyedInsidePump = true;
    }

    // Decorator over FakeBrowserSurface that logs lifecycle calls, and can fake an
    // initialisation error, a surface that never closes, and a Pump hook.
    class RecordingSurface final : public IBrowserSurface
    {
    public:
        RecordingSurface(SpiralTests::FakeBrowserSurface& inner, std::vector<std::string>& log)
            : m_Inner(inner)
            , m_Log(log)
        {
        }

        std::string InitializeError;
        bool NeverCloses = false;
        std::function<void()> PumpHook;

        std::string_view BackendName() const override { return "fake"; }

        bool Initialize(const BrowserSurfaceConfig& config, Listener& listener, std::string& error) override
        {
            m_Log.push_back("Initialize");
            if (!InitializeError.empty())
            {
                error = InitializeError;
                return false;
            }
            return m_Inner.Initialize(config, listener, error);
        }

        void Pump() override
        {
            g_InSurfacePump = true;
            if (PumpHook)
                PumpHook();
            m_Inner.Pump();
            g_InSurfacePump = false;
        }

        void SetViewSize(const BrowserViewSize& size) override
        {
            m_Log.push_back("SetViewSize " + std::to_string(size.Width) + "x" + std::to_string(size.Height) + "@"
                + std::to_string(static_cast<int>(size.DeviceScale * 100)));
            m_Inner.SetViewSize(size);
        }

        void SetVisible(bool visible) override
        {
            m_Log.push_back(visible ? "SetVisible 1" : "SetVisible 0");
            m_Inner.SetVisible(visible);
        }

        void SetFocus(bool focused) override
        {
            m_Log.push_back(focused ? "SetFocus 1" : "SetFocus 0");
            m_Inner.SetFocus(focused);
        }

        void SendMouseMove(const BrowserMouse& mouse, bool leave) override { m_Inner.SendMouseMove(mouse, leave); }

        void SendMouseButton(const BrowserMouse& mouse, BrowserMouseButton button, bool down, int clickCount) override
        {
            m_Inner.SendMouseButton(mouse, button, down, clickCount);
        }

        void SendMouseCaptureLost() override { m_Inner.SendMouseCaptureLost(); }
        void SendMouseWheel(const BrowserMouse& mouse, float dx, float dy) override { m_Inner.SendMouseWheel(mouse, dx, dy); }
        void SendKey(const BrowserKey& key) override
        {
            if (key.Kind == BrowserKey::Phase::Up)
                m_Log.push_back("KeyUp " + std::to_string(key.GlfwKey));
            m_Inner.SendKey(key);
        }

        void SetScreenInfo(const BrowserScreenInfo& info) override { m_Inner.SetScreenInfo(info); }

        void SetGrantedHosts(std::span<const std::string> hosts) override
        {
            m_Log.push_back("SetGrantedHosts " + std::to_string(hosts.size()));
            m_Inner.SetGrantedHosts(hosts);
        }

        bool RetryDeniedNavigation() override
        {
            m_Log.push_back("RetryDeniedNavigation");
            return m_Inner.RetryDeniedNavigation();
        }

        void Navigate(std::string_view url) override
        {
            m_Log.push_back("Navigate " + std::string(url));
            m_Inner.Navigate(url);
        }

        void GoBack() override { m_Inner.GoBack(); }
        void GoForward() override { m_Inner.GoForward(); }
        void Reload() override { m_Inner.Reload(); }
        void Stop() override { m_Inner.Stop(); }
        void CancelDownload(u64 id) override { m_Inner.CancelDownload(id); }

        void ClearBrowsingData() override
        {
            m_Log.push_back("ClearBrowsingData");
            m_Inner.ClearBrowsingData();
        }

        void RequestClose() override
        {
            m_Log.push_back("RequestClose");
            if (!NeverCloses)
                m_Inner.RequestClose();
        }

        bool IsClosed() const override { return !NeverCloses && m_Inner.IsClosed(); }

        void Shutdown(u32 timeoutMs) override
        {
            m_Log.push_back("Shutdown " + std::to_string(timeoutMs));
            m_Inner.Shutdown(timeoutMs);
        }

    private:
        SpiralTests::FakeBrowserSurface& m_Inner;
        std::vector<std::string>& m_Log;
    };

    struct Session
    {
        FakeUiTextures Textures;
        SpiralTests::FakeBrowserSurface Fake;
        std::vector<std::string> Log;
        std::vector<std::string> Warnings;
        std::unique_ptr<RecordingSurface> Wrapper;
        std::unique_ptr<BrowserPanelCore> Core;
        u64 Now = 100000;
        int LoadCalls = 0;
        int SleepCalls = 0;
        std::string LoadError;
        bool LoadThrows = false;
        BrowserPanelConfig Config;

        Session()
        {
            g_EventLog = &Log;
            g_InSurfacePump = false;
            g_DestroyedInsidePump = false;
            Textures.EventLog = &Log;
            Wrapper = std::make_unique<RecordingSurface>(Fake, Log);
            Config.EditorDirectory = "/opt/spiral-fab-panel-test/editor";
            Config.ProfileDirectory = "/opt/spiral-fab-panel-test/profile";
            Config.DownloadStagingDirectory = "/opt/spiral-fab-panel-test/staging";

            BrowserPanelEnvironment environment;
            environment.Textures = &Textures;
            environment.LoadSurface = [this](const BrowserPanelConfig&)
            {
                ++LoadCalls;
                if (LoadThrows)
                    throw std::runtime_error("loader exploded");
                BrowserSurfaceLoadResult result;
                if (!LoadError.empty())
                {
                    result.Error = LoadError;
                    return result;
                }
                result.Surface = MakeBrowserSurfacePtr(Wrapper.get(), &DestroySessionSurface);
                return result;
            };
            environment.NowMs = [this] { return Now; };
            environment.SleepMs = [this](u32 milliseconds)
            {
                Now += milliseconds;
                // A regression that drops the close deadline must fail the test, not hang it.
                if (++SleepCalls > 20000)
                    throw std::runtime_error("the close wait did not terminate");
            };
            environment.LogWarn = [this](std::string_view message) { Warnings.emplace_back(message); };
            Core = std::make_unique<BrowserPanelCore>(std::move(environment));
            Core->Configure(Config);
        }

        ~Session()
        {
            Core.reset();
            g_EventLog = nullptr;
        }

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        BrowserPanelLayout Layout(float x = 100.0f, float y = 50.0f, float width = 640.0f, float height = 360.0f, bool hovered = true,
            bool focused = false) const
        {
            BrowserPanelLayout layout;
            layout.Surface = { x, y, width, height };
            layout.SurfaceHovered = hovered;
            layout.PanelFocused = focused;
            return layout;
        }

        // One UI frame as the adapter performs it.
        void Frame(const BrowserPanelLayout& layout)
        {
            Core->Pump();
            Core->UpdateLayout(layout);
            Now += 16;
        }

        void DeliverFrame(u32 width, u32 height, u8 seed = 1)
        {
            Fake.ScriptFrame(width, height, PatternBgra(width, height, seed), { { 0, 0, static_cast<int>(width), static_cast<int>(height) } });
        }

        size_t Count(const std::string& entry) const { return static_cast<size_t>(std::count(Log.begin(), Log.end(), entry)); }
        bool Has(const std::string& entry) const { return Count(entry) > 0; }

        // Starts the browser, draws one frame, and delivers a first frame.
        void StartRunning(u32 width = 640, u32 height = 360)
        {
            Core->SetVisible(true);
            Core->Pump();
            Core->UpdateLayout(Layout(100.0f, 50.0f, static_cast<float>(width), static_cast<float>(height)));
            DeliverFrame(width, height);
            Frame(Layout(100.0f, 50.0f, static_cast<float>(width), static_cast<float>(height)));
        }
    };

    // ---- 4. lifecycle and startup failures --------------------------------------------------------------------

    bool CheckStartupTexts()
    {
        Checker check { "startup-text" };
        const std::string inUse = DescribeBrowserStartupFailure("the browser profile is in use by another running Spiral instance");
        check.Expect(Contains(inUse, "another Spiral instance") && Contains(inUse, "restart the Editor"), "profile in use: " + inUse);
        const std::string sandbox = DescribeBrowserStartupFailure(
            "the Chromium sandbox is unavailable: this system denies unprivileged user namespaces (error 1).");
        check.Expect(Contains(sandbox, "user namespaces") && Contains(sandbox, "restart the Editor") && Contains(sandbox, "(error 1)"),
            "sandbox denial: " + sandbox);
        const std::string missing = DescribeBrowserStartupFailure(
            "the Fab browser runtime is not installed (cef/libSpiralBrowserHost.so was not found next to the Editor)");
        check.Expect(Contains(missing, "not installed") && Contains(missing, "restart the Editor"), "missing library: " + missing);
        const std::string generic = DescribeBrowserStartupFailure("something odd.");
        check.Expect(Contains(generic, "could not start: something odd") && Contains(generic, "Restart the Editor")
                && !Contains(generic, ".."),
            "generic: " + generic);
        return check.Ok;
    }

    bool CheckLifecycle()
    {
        Checker check { "lifecycle" };
        {
            Session s;
            check.Expect(s.Core->GetState() == BrowserPanelCore::State::NotStarted && s.LoadCalls == 0, "starts idle without loading");
            s.Core->Pump();
            s.Core->SetVisible(false);
            s.Core->SetVisible(false);
            check.Expect(s.LoadCalls == 0 && s.Core->GetDiagnostics().State == "NotStarted" && !s.Core->GetDiagnostics().Initialized,
                "Pump and a hide request never load the host");
            check.Expect(!s.Core->WantsKeyboard() && !s.Core->StatusLine().empty(), "idle panel has a status line and no keyboard");

            s.Core->SetVisible(true);
            check.Expect(s.LoadCalls == 1 && s.Core->GetState() == BrowserPanelCore::State::Starting && s.Fake.IsInitialized(),
                "the first transition to visible loads and initializes once");
            check.Expect(s.Log.size() >= 2 && s.Log[0] == "Initialize" && s.Log[1] == "SetVisible 0", "initialize first, then the exact hidden bookkeeping");
            const BrowserSurfaceConfig& passed = s.Fake.Config();
            check.Expect(passed.ProfileDir == s.Config.ProfileDirectory && passed.DownloadStagingDir == s.Config.DownloadStagingDirectory
                    && passed.MaxFps == BrowserPanelLimits::kMaximumBrowserFps && passed.RenderMode == BrowserRenderMode::Software,
                "the surface config carries the panel directories, the 30 fps cap, and software rendering");
            check.Expect(s.Core->GetDiagnostics().Initialized && s.Core->GetDiagnostics().Visible, "diagnostics report the initialized visible panel");

            s.Core->SetVisible(false);
            s.Core->SetVisible(true);
            check.Expect(s.LoadCalls == 1 && s.Count("Initialize") == 1, "closing and reopening the panel never reloads the host");

            // First layout: size, visibility, then the home navigation, in that order, once.
            s.Core->UpdateLayout(s.Layout(10.0f, 20.0f, 640.0f, 360.0f));
            const auto sizeAt = std::find(s.Log.begin(), s.Log.end(), "SetViewSize 640x360@100");
            const auto visibleAt = std::find(sizeAt, s.Log.end(), "SetVisible 1");
            const auto navigateAt = std::find(visibleAt, s.Log.end(), "Navigate https://www.fab.com/");
            check.Expect(sizeAt != s.Log.end() && visibleAt != s.Log.end() && navigateAt != s.Log.end(),
                "first layout: view size, then visible, then the home page");
            s.Core->UpdateLayout(s.Layout(10.0f, 20.0f, 640.0f, 360.0f));
            check.Expect(s.Count("Navigate https://www.fab.com/") == 1 && s.Count("SetVisible 1") == 1, "later frames repeat neither the navigation nor the visibility");

            s.DeliverFrame(640, 360);
            s.Core->Pump();
            check.Expect(s.Core->GetState() == BrowserPanelCore::State::Running && s.Core->GetDiagnostics().FramesReceived == 1
                    && s.Core->GetDiagnostics().FrameWidth == 640 && s.Core->GetDiagnostics().TextureValid
                    && s.Core->GetDiagnostics().DisplayHost == "www.fab.com" && s.Textures.LiveCount() == 1,
                "the first frame makes the panel Running with a valid texture and the display host");
            check.Expect(s.Core->GetDiagnostics().DisplayHost.find('/') == std::string::npos, "only the host is shown");
            check.Expect(s.Fake.OutOfPumpCallbacks() == 0, "every callback arrived inside Pump");

            // Hidden/collapsed mapping.
            s.Core->NotifyContentHidden();
            s.Core->NotifyContentHidden();
            check.Expect(s.Count("SetVisible 0") == 2 && s.Log.back() == "SetVisible 0", "collapsing hides the surface once");
            s.Core->UpdateLayout(s.Layout(10.0f, 20.0f, 640.0f, 360.0f));
            check.Expect(s.Count("SetVisible 1") == 2, "drawing again shows it once");
            s.Core->Pump(); // consumed the draw
            s.Core->Pump(); // no draw since the previous Pump: hidden by omission
            check.Expect(s.Log.back() == "SetVisible 0", "a panel not drawn since the last Pump is hidden for the browser");
            s.Core->UpdateLayout(s.Layout(10.0f, 20.0f, 640.0f, 8.0f));
            check.Expect(s.Log.back() == "SetVisible 0", "a content region under 16 pixels counts as hidden");
            s.Core->UpdateLayout(s.Layout(10.0f, 20.0f, 640.0f, 360.0f));
            s.Core->SetVisible(false);
            check.Expect(s.Log.back() == "SetVisible 0" && !s.Core->IsVisible(), "closing the window hides the surface");

            // Orderly shutdown.
            s.Core->SetVisible(true);
            s.Core->UpdateLayout(s.Layout());
            s.Log.clear();
            s.Core->Shutdown();
            const std::vector<std::string> tail = { "RequestClose", "Shutdown 2000", "SurfaceDestroy", "TextureDestroy" };
            check.Expect(s.Log.size() >= tail.size() && std::equal(tail.begin(), tail.end(), s.Log.end() - static_cast<std::ptrdiff_t>(tail.size())),
                "shutdown order is RequestClose, Shutdown, DestroySurface, then the UI texture");
            check.Expect(!s.Fake.ShutdownBeforeClose && s.Core->GetState() == BrowserPanelCore::State::Closed && s.Textures.LiveCount() == 0,
                "the surface was closed before Shutdown and nothing is left");
            check.Expect(s.Fake.PumpCount() >= 2, "shutdown pumped until the surface reported closed");
            s.Core->Shutdown();
            s.Core->SetVisible(true);
            check.Expect(s.LoadCalls == 1 && s.Core->GetState() == BrowserPanelCore::State::Closed && !s.Core->IsVisible(),
                "no restart after shutdown; a repeated Shutdown is harmless");
        }
        {
            // A surface that never closes: bounded by the deadline, Shutdown still runs.
            Session s;
            s.Wrapper->NeverCloses = true;
            s.StartRunning();
            const u64 before = s.Now;
            s.Core->Shutdown();
            check.Expect(s.Now - before >= BrowserPanelLimits::kCloseDeadlineMilliseconds
                    && s.Now - before < BrowserPanelLimits::kCloseDeadlineMilliseconds + 100,
                "the wait for a close is bounded by the 3 second deadline");
            check.Expect(s.Has("Shutdown 2000") && s.Has("SurfaceDestroy") && s.Textures.LiveCount() == 0
                    && s.Core->GetState() == BrowserPanelCore::State::Closed,
                "after the deadline the surface is still shut down, destroyed, and the texture released");
            bool warned = false;
            for (const std::string& warning : s.Warnings)
                warned = warned || Contains(warning, "did not close");
            check.Expect(warned, "the forced shutdown is logged");
        }
        {
            // Shutdown of a panel that never started.
            Session s;
            s.Core->Shutdown();
            s.Core->SetVisible(true);
            check.Expect(s.LoadCalls == 0 && s.Core->GetState() == BrowserPanelCore::State::Closed, "unused panel shuts down without loading");
        }
        {
            // Sign out.
            Session s;
            s.StartRunning();
            check.Expect(s.Core->GetState() == BrowserPanelCore::State::Running, "running before sign-out");
            s.Core->ClearBrowsingData();
            check.Expect(s.Fake.ClearBrowsingDataCount == 1 && s.Core->GetState() == BrowserPanelCore::State::Closing
                    && Contains(s.Core->StatusLine(), "Signing out"),
                "sign-out asks the surface and starts closing");
            for (int frame = 0; frame < 4 && s.Core->GetState() == BrowserPanelCore::State::Closing; ++frame)
                s.Core->Pump();
            check.Expect(s.Core->GetState() == BrowserPanelCore::State::Closed && s.Core->SignedOut() && s.Has("Shutdown 2000")
                    && s.Has("SurfaceDestroy") && s.Textures.LiveCount() == 0,
                "the close completes, shuts down, and releases the texture");
            check.Expect(Contains(s.Core->StatusLine(), "Restart the Editor to sign in again"), "the panel tells the user to restart: " + s.Core->StatusLine());
            s.Core->SetVisible(true);
            s.Core->SetVisible(true);
            check.Expect(s.LoadCalls == 1 && s.Core->GetState() == BrowserPanelCore::State::Closed, "no second browser is started after sign-out");
            s.Core->ClearBrowsingData();
            check.Expect(s.Fake.ClearBrowsingDataCount == 1, "a second sign-out request is ignored");
        }
        return check.Ok;
    }

    bool CheckStartupFailures()
    {
        Checker check { "startup-failures" };
        {
            Session s;
            s.Config.ProfileDirectory = "relative/profile";
            s.Core->Configure(s.Config);
            s.Core->SetVisible(true);
            const BrowserPanelDiagnostics diagnostics = s.Core->GetDiagnostics();
            check.Expect(diagnostics.State == "Failed" && diagnostics.Failed && s.LoadCalls == 0 && Contains(diagnostics.Error, "configuration is invalid")
                    && Contains(diagnostics.Error, "Restart the Editor") && !Contains(diagnostics.Error, "relative/profile"),
                "an invalid configuration fails before loading and never echoes the path: " + diagnostics.Error);
            check.Expect(s.Core->StatusLine() == diagnostics.Error, "the status line shows the failure");
        }
        {
            Session s;
            s.Config.HomeUrl = "https://example.org/";
            s.Core->Configure(s.Config);
            s.Core->SetVisible(true);
            check.Expect(s.Core->GetState() == BrowserPanelCore::State::Failed && s.LoadCalls == 0, "a home URL outside the allowlist fails before loading");
        }
        {
            Session s;
            s.LoadError = "the Fab browser runtime is not installed (cef/libSpiralBrowserHost.so was not found next to the Editor)";
            s.Core->SetVisible(true);
            check.Expect(s.Core->GetState() == BrowserPanelCore::State::Failed && s.LoadCalls == 1 && !s.Fake.IsInitialized(),
                "a missing library is a final failure");
            check.Expect(Contains(s.Core->StatusLine(), "not installed") && Contains(s.Core->StatusLine(), "restart the Editor"), s.Core->StatusLine());
            s.Core->SetVisible(false);
            s.Core->SetVisible(true);
            s.Core->Pump();
            s.Core->UpdateLayout(s.Layout());
            Engine::MouseMovedEvent moved(120.0f, 80.0f);
            check.Expect(!s.Core->OnEvent(moved) && !moved.Handled && !s.Core->WantsKeyboard(), "a failed panel consumes nothing");
            check.Expect(s.LoadCalls == 1, "a failed start is never retried in this process");
            s.Core->Shutdown();
            check.Expect(s.Core->GetState() == BrowserPanelCore::State::Failed, "a failed panel stays failed after Shutdown so the reason remains visible");
        }
        {
            Session s;
            s.Wrapper->InitializeError = "the browser profile is in use by another running Spiral instance";
            s.Core->SetVisible(true);
            check.Expect(s.Core->GetState() == BrowserPanelCore::State::Failed && s.Has("SurfaceDestroy") && !s.Core->GetDiagnostics().Initialized
                    && Contains(s.Core->StatusLine(), "another Spiral instance"),
                "profile in use: the surface is destroyed and the user is told to close the other instance and restart");
        }
        {
            Session s;
            s.Wrapper->InitializeError = "the Chromium sandbox is unavailable: this system denies unprivileged user namespaces (error 1)";
            s.Core->SetVisible(true);
            check.Expect(s.Core->GetState() == BrowserPanelCore::State::Failed && Contains(s.Core->StatusLine(), "user namespaces")
                    && Contains(s.Core->StatusLine(), "restart the Editor"),
                "sandbox preflight denial: " + s.Core->StatusLine());
        }
        {
            Session s;
            s.LoadThrows = true;
            bool escaped = false;
            try
            {
                s.Core->SetVisible(true);
            }
            catch (...)
            {
                escaped = true;
            }
            check.Expect(!escaped && s.Core->GetState() == BrowserPanelCore::State::Failed && Contains(s.Core->StatusLine(), "loader exploded"),
                "an exception from the loader is contained and shown");
        }
        return check.Ok;
    }

    // ---- 5. input routing ------------------------------------------------------------------------------------------

    bool CheckInputRouting()
    {
        Checker check { "input" };
        Session s;
        s.StartRunning(640, 360);
        const auto layoutAt = [&](bool hovered, bool focused) { return s.Layout(100.0f, 50.0f, 640.0f, 360.0f, hovered, focused); };

        // Mouse over the surface goes to the page in view-local DIPs and is Handled.
        s.Core->UpdateLayout(layoutAt(true, false));
        Engine::MouseMovedEvent move(150.0f, 90.0f);
        check.Expect(s.Core->OnEvent(move) && move.Handled && s.Fake.MouseMoves.size() == 1 && !s.Fake.MouseMoves[0].Leave
                && s.Fake.MouseMoves[0].Mouse.X == 50.0f && s.Fake.MouseMoves[0].Mouse.Y == 40.0f,
            "a move over the surface reaches the page at (50,40) and is consumed");

        // Click: capture, click count, keyboard ownership needs the focused panel.
        Engine::MouseButtonPressedEvent press(0);
        check.Expect(s.Core->OnEvent(press) && press.Handled && s.Fake.MouseButtons.size() == 1 && s.Fake.MouseButtons[0].Down
                && s.Fake.MouseButtons[0].ClickCount == 1 && s.Fake.MouseButtons[0].Button == BrowserMouseButton::Left
                && s.Fake.MouseButtons[0].Mouse.X == 50.0f,
            "a press over the surface is a left click with count 1");
        check.Expect(!s.Core->WantsKeyboard(), "a click alone does not give the page the keyboard until the panel has focus");
        s.Core->UpdateLayout(layoutAt(true, true));
        check.Expect(s.Core->WantsKeyboard() && !s.Fake.FocusStates.empty() && s.Fake.FocusStates.back() && s.Core->GetDiagnostics().KeyboardOwnedByPage,
            "after the panel took focus the page owns the keyboard and the surface is focused");
        Engine::MouseButtonReleasedEvent release(0);
        check.Expect(s.Core->OnEvent(release) && !s.Fake.MouseButtons.back().Down && s.Fake.MouseButtons.back().ClickCount == 1, "release carries the same count");
        s.Now += 100;
        Engine::MouseButtonPressedEvent pressAgain(0);
        s.Core->OnEvent(pressAgain);
        check.Expect(s.Fake.MouseButtons.back().ClickCount == 2, "a second press within 500 ms and 4 pixels is a double click");
        Engine::MouseButtonReleasedEvent releaseAgain(0);
        s.Core->OnEvent(releaseAgain);
        check.Expect(!s.Fake.MouseButtons.back().Down && s.Fake.MouseButtons.back().ClickCount == 2, "the release of a double click repeats count 2");
        Engine::MouseButtonPressedEvent unknown(7);
        check.Expect(!s.Core->OnEvent(unknown), "an unsupported mouse button is not consumed");

        // Keyboard: down, char, up, with scancode and modifiers; consumed so Editor shortcuts do not run.
        Engine::KeyPressedEvent keyDown(65, false, 38, Engine::InputModifierControl);
        check.Expect(s.Core->OnEvent(keyDown) && keyDown.Handled, "a key goes to the page while it owns the keyboard");
        const BrowserKey down = s.Fake.Keys.back();
        check.Expect(down.Kind == BrowserKey::Phase::Down && down.GlfwKey == 65 && down.WindowsVirtualKey == 65 && down.Scancode == 38
                && down.Modifiers == BrowserModifier::Control && !down.Repeat,
            "key down carries the GLFW key, the Windows virtual key, the scancode, and Control");
        Engine::CharTypedEvent typed(0x00E9);
        check.Expect(s.Core->OnEvent(typed) && s.Fake.Keys.back().Kind == BrowserKey::Phase::Char && s.Fake.Keys.back().Codepoint == 0x00E9,
            "typed text arrives as a char event with its code point");
        Engine::KeyPressedEvent repeatKey(65, true, 38, Engine::InputModifierNone);
        s.Core->OnEvent(repeatKey);
        check.Expect(s.Fake.Keys.back().Repeat, "auto-repeat is flagged");
        Engine::KeyReleasedEvent keyUp(65, 38, Engine::InputModifierNone);
        check.Expect(s.Core->OnEvent(keyUp) && s.Fake.Keys.back().Kind == BrowserKey::Phase::Up, "the matching key up is delivered");
        Engine::CharTypedEvent control(0x07);
        check.Expect(!s.Core->OnEvent(control), "a control character is not forwarded as text");

        // Wheel: the router's pixel convention, GLFW sign unchanged.
        Engine::MouseScrolledEvent wheel(0.0f, 1.0f);
        check.Expect(s.Core->OnEvent(wheel) && s.Fake.Wheels.size() == 1 && s.Fake.Wheels[0].DeltaY == BrowserInputRouter::kPixelsPerWheelNotch
                && s.Fake.Wheels[0].DeltaX == 0.0f && s.Fake.Wheels[0].Mouse.X == 50.0f,
            "one wheel notch is 40 pixels with the GLFW sign, at the last pointer position");

        // Losing the OS window while a key is down: the page gets the key up, then loses focus.
        Engine::KeyPressedEvent heldKey(70, false, 41, Engine::InputModifierNone);
        s.Core->OnEvent(heldKey);
        const size_t keysBeforeFocusLoss = s.Fake.Keys.size();
        Engine::WindowFocusEvent lost(false);
        const auto logStart = static_cast<std::ptrdiff_t>(s.Log.size());
        check.Expect(!s.Core->OnEvent(lost) && !lost.Handled, "window focus changes are observed, never consumed");
        {
            const auto keyUpAt = std::find(s.Log.begin() + logStart, s.Log.end(), "KeyUp 70");
            const auto unfocusAt = std::find(s.Log.begin() + logStart, s.Log.end(), "SetFocus 0");
            check.Expect(keyUpAt != s.Log.end() && unfocusAt != s.Log.end() && keyUpAt < unfocusAt, "the key up is sent before the page is unfocused");
        }
        check.Expect(s.Fake.Keys.size() == keysBeforeFocusLoss + 1 && s.Fake.Keys.back().Kind == BrowserKey::Phase::Up && s.Fake.Keys.back().GlfwKey == 70
                && !s.Fake.FocusStates.back() && !s.Core->WantsKeyboard(),
            "focus loss releases the held key before unfocusing the page");
        Engine::WindowFocusEvent regained(true);
        s.Core->OnEvent(regained);

        // Keys without ownership belong to the Editor.
        Engine::KeyPressedEvent editorKey(90, false, 52, Engine::InputModifierControl);
        const size_t keysBefore = s.Fake.Keys.size();
        check.Expect(!s.Core->OnEvent(editorKey) && !editorKey.Handled && s.Fake.Keys.size() == keysBefore, "without keyboard ownership Ctrl+Z is left to the Editor");
        Engine::CharTypedEvent editorChar('z');
        check.Expect(!s.Core->OnEvent(editorChar), "and so is typed text");

        // Pointer leaves the surface: the page is told once; the Editor then owns the mouse.
        s.Core->UpdateLayout(layoutAt(true, false));
        s.Core->OnEvent(move);
        const size_t movesBefore = s.Fake.MouseMoves.size();
        Engine::MouseMovedEvent outside(20.0f, 20.0f);
        check.Expect(!s.Core->OnEvent(outside) && !outside.Handled, "a move outside the surface is not consumed");
        check.Expect(s.Fake.MouseMoves.size() == movesBefore + 1 && s.Fake.MouseMoves.back().Leave, "the page receives one mouse-leave");
        Engine::MouseScrolledEvent outsideWheel(0.0f, 1.0f);
        check.Expect(!s.Core->OnEvent(outsideWheel), "wheel outside the surface belongs to the Editor");

        // Cursor leaving the window while over the page.
        s.Core->OnEvent(move);
        const size_t movesBeforeLeave = s.Fake.MouseMoves.size();
        Engine::CursorEnterEvent leaveWindow(false);
        check.Expect(!s.Core->OnEvent(leaveWindow) && s.Fake.MouseMoves.size() == movesBeforeLeave + 1 && s.Fake.MouseMoves.back().Leave,
            "the cursor leaving the window sends a mouse-leave");

        // A popup or modal over the panel takes the mouse away from the page.
        s.Core->UpdateLayout(layoutAt(true, true));
        s.Core->OnEvent(move);
        const size_t movesBeforeModal = s.Fake.MouseMoves.size();
        BrowserPanelLayout modal = layoutAt(true, true);
        modal.ModalOrPopupOpen = true;
        s.Core->UpdateLayout(modal);
        check.Expect(s.Fake.MouseMoves.size() == movesBeforeModal + 1 && s.Fake.MouseMoves.back().Leave, "a modal opening over the pointer tells the page the pointer left");
        Engine::MouseMovedEvent underModal(150.0f, 90.0f);
        check.Expect(!s.Core->OnEvent(underModal) && s.Fake.MouseMoves.size() == movesBeforeModal + 1, "with a modal open the page gets no mouse events");

        // Capture is released when the panel is hidden mid-drag.
        s.Core->UpdateLayout(layoutAt(true, true));
        Engine::MouseMovedEvent inside(150.0f, 90.0f);
        s.Core->OnEvent(inside);
        Engine::MouseButtonPressedEvent dragStart(0);
        s.Core->OnEvent(dragStart);
        const size_t captureLostBefore = s.Fake.CaptureLostCount;
        s.Core->NotifyContentHidden();
        check.Expect(s.Fake.CaptureLostCount == captureLostBefore + 1 && !s.Core->WantsKeyboard(), "hiding the panel mid-drag releases mouse capture and the keyboard");
        return check.Ok;
    }

    bool CheckInputScaleAndResize()
    {
        Checker check { "input-scale" };
        Session s;
        s.Core->SetVisible(true);
        // The content scale arrives before the first layout.
        Engine::WindowContentScaleEvent scale(2.0f, 2.0f);
        s.Core->OnEvent(scale);
        s.Core->UpdateLayout(s.Layout(100.0f, 50.0f, 640.0f, 360.0f));
        check.Expect(s.Has("SetViewSize 320x180@200") && s.Core->AppliedDeviceScale() == 2.0f, "at scale 2 a 640x360 pixel region is a 320x180 DIP view");
        Engine::MouseMovedEvent move(160.0f, 110.0f);
        s.Core->UpdateLayout(s.Layout(100.0f, 50.0f, 640.0f, 360.0f));
        s.Core->OnEvent(move);
        check.Expect(!s.Fake.MouseMoves.empty() && s.Fake.MouseMoves.back().Mouse.X == 30.0f && s.Fake.MouseMoves.back().Mouse.Y == 30.0f,
            "pointer positions are converted from pixels to DIPs (60,60 pixels is 30,30 DIPs)");

        // A splitter drag: many sizes, one application after it settles.
        size_t viewSizesBefore = s.Fake.ViewSizes.size();
        for (int step = 1; step <= 40; ++step)
        {
            s.Now += 16;
            s.Core->UpdateLayout(s.Layout(100.0f, 50.0f, 640.0f + static_cast<float>(step * 3), 360.0f));
        }
        check.Expect(s.Fake.ViewSizes.size() == viewSizesBefore, "no SetViewSize while the region keeps changing");
        s.Now += 200;
        s.Core->UpdateLayout(s.Layout(100.0f, 50.0f, 640.0f + 120.0f, 360.0f));
        check.Expect(s.Fake.ViewSizes.size() == viewSizesBefore + 1 && s.Fake.ViewSizes.back().Width == 380, "one SetViewSize after the region settles (760 px / 2)");
        return check.Ok;
    }

    // ---- 6. downloads, status, containment ------------------------------------------------------------------------

    bool CheckDownloadsAndStatus()
    {
        Checker check { "downloads" };
        {
            Session s;
            s.StartRunning();
            s.Fake.ScriptDownload("Fab Asset.zip", 120, { 40, 80, 120 }, SpiralTests::FakeBrowserSurface::DownloadEnd::Complete);
            s.Fake.ScriptDownload("../evil.exe", 10, {}, SpiralTests::FakeBrowserSurface::DownloadEnd::Complete);
            s.Core->Pump();
            check.Expect(s.Core->Downloads().Size() == 1 && s.Core->GetDiagnostics().DownloadsCompleted == 1, "one completed download queued; the blocked one is not");
            check.Expect(Contains(s.Core->StatusLine(), "Download blocked") && Contains(s.Core->StatusLine(), "evil.exe"), "the last event (a blocked download) is shown: " + s.Core->StatusLine());
            BrowserPanelDownload taken;
            check.Expect(s.Core->TryTakeCompletedDownload(taken) && taken.DisplayName == "Fab_Asset.zip" && !taken.StagedPath.empty(), "the completed download can be taken");
            const std::filesystem::path relative = taken.StagedPath.lexically_relative(s.Config.DownloadStagingDirectory);
            check.Expect(!relative.empty() && *relative.begin() != ".." && std::distance(relative.begin(), relative.end()) == 2,
                "the staged path is <staging>/<directory>/<name>");
            check.Expect(!s.Core->TryTakeCompletedDownload(taken), "and only once");

            s.Fake.ScriptDownload("broken.glb", 50, { 25 }, SpiralTests::FakeBrowserSurface::DownloadEnd::Fail);
            s.Core->Pump();
            check.Expect(Contains(s.Core->StatusLine(), "Download failed") && s.Core->Downloads().Size() == 0, "a failed download is reported, not queued");

            // Navigation denial: host only; counters; no query or credential text anywhere.
            s.Fake.ScriptNavigationRequest("https://evil.example/phish?token=secret", BrowserNavigationKind::TopLevel);
            s.Core->Pump();
            const BrowserPanelDiagnostics diagnostics = s.Core->GetDiagnostics();
            check.Expect(diagnostics.NavigationDenials == 1 && Contains(s.Core->StatusLine(), "evil.example"), "a denied navigation is counted and named by host");
            for (const std::string& text : { s.Core->StatusLine(), diagnostics.Error, diagnostics.DisplayHost })
                check.Expect(!Contains(text, "secret") && !Contains(text, "phish") && !Contains(text, "http"), "no URL material in " + text);
            for (const std::string& warning : s.Warnings)
                check.Expect(!Contains(warning, "secret") && !Contains(warning, "http"), "no URL material in a log line: " + warning);

            // Page problems are not fatal and clear on the next load.
            s.Core->OnFailed("the page failed to load (net::ERR_NAME_NOT_RESOLVED, net error -105)");
            check.Expect(s.Core->GetState() == BrowserPanelCore::State::Running && Contains(s.Core->StatusLine(), "Page problem")
                    && Contains(s.Core->GetDiagnostics().Error, "net error -105"),
                "a page failure is shown but the panel keeps running");
            s.Core->OnLoadState(true, false, false);
            check.Expect(s.Core->GetDiagnostics().Error.empty() && s.Core->GetDiagnostics().Loading, "starting a load clears the page problem");
            s.Core->OnCursor(BrowserCursor::Hand);
            check.Expect(s.Core->Cursor() == BrowserCursor::Hand, "the page cursor is kept for the adapter");

            // An empty staged path on completion is not queued.
            BrowserDownloadEvent empty;
            empty.Kind = BrowserDownloadEvent::State::Completed;
            empty.DisplayName = "x.zip";
            const size_t queued = s.Core->Downloads().Size();
            s.Core->OnDownload(empty);
            check.Expect(s.Core->Downloads().Size() == queued, "a completion without a staged file is ignored");
        }
        {
            // Bounded queue: the oldest entries are dropped, order is kept.
            BrowserDownloadQueue queue(16);
            for (int index = 0; index < 20; ++index)
                queue.Push({ std::filesystem::path("/s/" + std::to_string(index)), "m" + std::to_string(index) + ".glb" });
            check.Expect(queue.Size() == 16 && queue.Dropped() == 4, "the queue holds 16 and counts 4 dropped");
            BrowserPanelDownload taken;
            bool ordered = true;
            for (int index = 4; index < 20; ++index)
                ordered = queue.TryTake(taken) && taken.DisplayName == "m" + std::to_string(index) + ".glb" && ordered;
            check.Expect(ordered && !queue.TryTake(taken), "the newest 16 come out oldest first");
            BrowserDownloadQueue tiny(0);
            tiny.Push({ "/a", "a" });
            tiny.Push({ "/b", "b" });
            check.Expect(tiny.Size() == 1 && tiny.TryTake(taken) && taken.DisplayName == "b", "a zero capacity is raised to one");
        }
        return check.Ok;
    }

    bool CheckContainment()
    {
        Checker check { "containment" };
        {
            // A throwing Pump fails the panel; nothing escapes.
            Session s;
            s.StartRunning();
            s.Wrapper->PumpHook = [] { throw std::runtime_error("surface exploded"); };
            bool escaped = false;
            try
            {
                s.Core->Pump();
                s.Core->Pump();
            }
            catch (...)
            {
                escaped = true;
            }
            check.Expect(!escaped && s.Core->GetState() == BrowserPanelCore::State::Failed && Contains(s.Core->StatusLine(), "surface exploded")
                    && s.Has("SurfaceDestroy") && s.Textures.LiveCount() == 0,
                "an exception inside the surface fails the panel and releases everything");
            Engine::MouseMovedEvent moved(150.0f, 90.0f);
            check.Expect(!s.Core->OnEvent(moved), "a failed panel stays inert");
        }
        {
            // An unexpected close reported from inside Pump must not destroy the surface during that Pump.
            Session s;
            s.StartRunning();
            s.Wrapper->PumpHook = [&s] { s.Core->OnClosed(); };
            s.Core->Pump();
            check.Expect(s.Core->GetState() == BrowserPanelCore::State::Failed && Contains(s.Core->StatusLine(), "closed unexpectedly"),
                "an unexpected close fails the panel");
            check.Expect(!g_DestroyedInsidePump && s.Has("SurfaceDestroy"), "the surface was destroyed after its Pump returned, never inside it");
        }
        {
            // A texture failure is local: the browser keeps running and recovers on a resize.
            Session s;
            s.Textures.CreateFailure = UiTextureError::DeviceCreateFailed;
            s.StartRunning();
            const BrowserPanelDiagnostics diagnostics = s.Core->GetDiagnostics();
            check.Expect(s.Core->GetState() == BrowserPanelCore::State::Running && !diagnostics.TextureValid && Contains(diagnostics.Error, "DeviceCreateFailed")
                    && Contains(s.Core->StatusLine(), "texture unavailable"),
                "texture creation failure marks the texture unavailable but not the browser: " + s.Core->StatusLine());
            bool logged = false;
            for (const std::string& warning : s.Warnings)
                logged = logged || Contains(warning, "texture unavailable");
            check.Expect(logged, "and it is logged");
            s.Textures.CreateFailure = UiTextureError::None;
            s.DeliverFrame(700, 400);
            s.Frame(s.Layout(100.0f, 50.0f, 700.0f, 400.0f));
            check.Expect(s.Core->GetDiagnostics().TextureValid && s.Core->GetDiagnostics().FrameWidth == 700, "a frame of a new size recovers the texture");
        }
        {
            // While hidden no GPU work is submitted, and the dirty region survives to the next show.
            Session s;
            s.StartRunning();
            const size_t updatesBefore = s.Textures.Updates;
            s.Core->NotifyContentHidden();
            s.Fake.ScriptFrame(640, 360, PatternBgra(640, 360, 77), { { 5, 5, 20, 20 } });
            s.Core->Pump();
            check.Expect(s.Textures.Updates == updatesBefore && s.Core->Uploader().HasPendingWork(), "a hidden panel submits nothing and keeps the dirty region");
            s.Frame(s.Layout());
            s.Core->Pump();
            check.Expect(s.Textures.Updates > updatesBefore && !s.Core->Uploader().HasPendingWork(), "the region is uploaded once the panel is shown again");
        }
        return check.Ok;
    }
}

namespace SpiralTests
{
    bool TestBrowserPanelViewDebounce()
    {
        return CheckDebounce();
    }

    bool TestBrowserPanelTextureUploaderScenarios()
    {
        return CheckUploaderScenarios();
    }

    bool TestBrowserPanelTextureUploaderProperty()
    {
        return RunPanelProperty("uploader-retry-and-resize", UploaderProperty, 400);
    }

    bool TestBrowserPanelLifecycleAndStartupFailures()
    {
        bool ok = CheckStartupTexts();
        ok = CheckLifecycle() && ok;
        ok = CheckStartupFailures() && ok;
        return ok;
    }

    bool TestBrowserPanelInputRouting()
    {
        bool ok = CheckInputRouting();
        ok = CheckInputScaleAndResize() && ok;
        return ok;
    }

    bool TestBrowserPanelDownloadsStatusAndContainment()
    {
        bool ok = CheckDownloadsAndStatus();
        ok = CheckContainment() && ok;
        return ok;
    }
}
