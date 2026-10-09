#include "FabBrowserCoreTests.h"

#include "BrowserDownloadPolicy.h"
#include "BrowserFrameMirror.h"
#include "BrowserInputRouter.h"
#include "BrowserNavigationPolicy.h"
#include "BrowserSurface.h"
#include "FabIntake.h"
#include "NullBrowserSurface.h"
#include "TestSupport/FakeBrowserSurface.h"
#include "TestSupport/GeneratedTest.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__linux__)
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace
{
    using namespace Fab;
    using Bytes = std::vector<Engine::u8>;
    using Engine::u32;
    using Engine::u64;
    using Engine::u8;

    // Failure hypotheses, oracles, and non-claims for the whole file:
    // - Every oracle here is a hand-written table or a deliberately naive
    //   per-pixel / per-byte / set-based model that shares no code with the
    //   production logic under test.
    // - Properties are deterministic (fixed seed) and replayable through
    //   SPIRAL_FAB_BROWSER_SEED / SPIRAL_FAB_BROWSER_REPLAY; a failure prints
    //   the seed, the original and minimised choice traces, and writes a
    //   counterexample JSON under the system temp directory.
    // - Tier: fast, in-process, no GPU, no browser engine, no network.
    // - Not claimed: any real browser engine behaviour, ImGui/GLFW event
    //   delivery, Windows execution, or filesystem race resistance beyond the
    //   POSIX no-follow open.

    struct Checker
    {
        const char* Suite;
        bool Ok = true;

        void Expect(bool condition, const std::string& message)
        {
            if (!condition)
            {
                std::cerr << "Fab browser core test failed [" << Suite << "]: " << message << '\n';
                Ok = false;
            }
        }
    };

    std::atomic<u64> g_FixtureCounter { 0 };

    class TempDir
    {
    public:
        explicit TempDir(std::string_view name)
        {
            const u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
            m_Path = std::filesystem::temp_directory_path()
                / ("spiral-fab-browser-core-" + std::string(name) + "-" + std::to_string(stamp) + "-"
                    + std::to_string(g_FixtureCounter.fetch_add(1)));
            std::error_code error;
            std::filesystem::create_directories(m_Path, error);
        }

        ~TempDir()
        {
            std::error_code error;
            std::filesystem::remove_all(m_Path, error);
        }

        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;

        const std::filesystem::path& Path() const { return m_Path; }

    private:
        std::filesystem::path m_Path;
    };

    void WriteFile(const std::filesystem::path& path, const Bytes& bytes)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    Bytes ReadFile(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    }

    Bytes ToBytes(std::string_view text)
    {
        return Bytes(text.begin(), text.end());
    }

    bool RunProperty(std::string_view name, const Spiral::Tests::Property& property, size_t iterations)
    {
        Spiral::Tests::CampaignOptions options;
        options.Iterations = iterations;
        if (const char* seed = std::getenv("SPIRAL_FAB_BROWSER_SEED"))
            options.Seed = std::strtoull(seed, nullptr, 10);
        Spiral::Tests::ChoiceTrace replay;
        if (const char* trace = std::getenv("SPIRAL_FAB_BROWSER_REPLAY"); trace && Spiral::Tests::ParseTrace(trace, replay))
            options.Replay = replay;

        Spiral::Tests::Counterexample failure;
        if (Spiral::Tests::RunCampaign(options, property, failure))
            return true;

        const std::string minimized = Spiral::Tests::SerializeTrace(failure.MinimizedTrace);
        const std::string rerun = "SPIRAL_FAB_BROWSER_SEED=" + std::to_string(failure.Seed)
            + " SPIRAL_FAB_BROWSER_REPLAY=\"" + minimized + "\" EngineTests --test <registered name of " + std::string(name) + ">";
        const std::filesystem::path artifact = std::filesystem::temp_directory_path()
            / ("spiral-fab-browser-core-counterexample-" + std::string(name) + ".json");
        std::string artifactError;
        const bool written = Spiral::Tests::WriteCounterexample(artifact, name, failure, rerun, artifactError);
        std::cerr << "Fab browser core property failed [" << name << "]: " << failure.Message
            << " seed=" << failure.Seed << " iteration=" << failure.Iteration
            << " originalTrace=" << Spiral::Tests::SerializeTrace(failure.OriginalTrace)
            << " minimizedTrace=" << minimized << "\n  rerun: " << rerun << '\n';
        if (written)
            std::cerr << "  counterexample: " << artifact.string() << '\n';
        else
            std::cerr << "  counterexample write failed: " << artifactError << '\n';
        return false;
    }

    // ---- independent per-pixel oracles --------------------------------------------

    struct Image
    {
        u32 Width = 0;
        u32 Height = 0;
        Bytes Rgba;
    };

    // Per-byte swizzle of a (possibly padded) BGRA buffer into a tight RGBA image.
    Image ReferenceRgba(const Bytes& bgra, u32 width, u32 height, u32 stride)
    {
        Image image { width, height, Bytes(static_cast<size_t>(width) * height * 4) };
        for (u32 y = 0; y < height; ++y)
        {
            for (u32 x = 0; x < width; ++x)
            {
                const size_t in = static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
                const size_t out = (static_cast<size_t>(y) * width + x) * 4;
                image.Rgba[out + 0] = bgra[in + 2];
                image.Rgba[out + 1] = bgra[in + 1];
                image.Rgba[out + 2] = bgra[in + 0];
                image.Rgba[out + 3] = bgra[in + 3];
            }
        }
        return image;
    }

    bool PixelInRects(int x, int y, const std::vector<BrowserDirtyRect>& rects)
    {
        for (const BrowserDirtyRect& rect : rects)
        {
            if (x >= rect.X && x < rect.X + rect.Width && y >= rect.Y && y < rect.Y + rect.Height)
                return true;
        }
        return false;
    }

    u32 LcgNext(u32& state)
    {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }

    // ---- swizzle and physical size ---------------------------------------------

    bool CheckSwizzleAndPhysicalSize()
    {
        Checker check { "swizzle-and-size" };

        // Literal pixel: memory B,G,R,A = 1,2,3,4 must become R,G,B,A = 3,2,1,4.
        const std::array<u8, 4> bgraPixel { 1, 2, 3, 4 };
        std::array<u8, 4> rgbaPixel {};
        SwizzleBgraToRgba(bgraPixel.data(), rgbaPixel.data(), 1);
        check.Expect(rgbaPixel == std::array<u8, 4> { 3, 2, 1, 4 }, "literal pixel swaps only the first and third byte");

        // Differential against the per-byte reference over every pixel count
        // around the word and alignment boundaries, at misaligned offsets.
        u32 state = 12345;
        for (size_t pixels = 0; pixels <= 67; ++pixels)
        {
            for (size_t misalignment = 0; misalignment < 4; ++misalignment)
            {
                Bytes source(pixels * 4 + misalignment);
                for (u8& byte : source)
                    byte = static_cast<u8>(LcgNext(state));
                Bytes destination(pixels * 4 + misalignment + 1, 0xAB);
                SwizzleBgraToRgba(source.data() + misalignment, destination.data() + misalignment, pixels);
                bool equal = true;
                for (size_t index = 0; index < pixels; ++index)
                {
                    const u8* in = source.data() + misalignment + index * 4;
                    const u8* out = destination.data() + misalignment + index * 4;
                    equal = equal && out[0] == in[2] && out[1] == in[1] && out[2] == in[0] && out[3] == in[3];
                }
                check.Expect(equal, "swizzle matches per-byte reference for " + std::to_string(pixels) + " pixels");
                check.Expect(std::all_of(destination.begin(), destination.begin() + static_cast<std::ptrdiff_t>(misalignment),
                                 [](u8 byte) { return byte == 0xAB; })
                        && destination.back() == 0xAB,
                    "swizzle writes only inside its destination range");
            }
        }

        struct SizeCase
        {
            u32 Width, Height;
            float Scale;
            u32 Maximum;
            u32 ExpectedWidth, ExpectedHeight;
        };
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        const SizeCase cases[] = {
            { 960, 540, 2.0f, 4096, 1920, 1080 },
            { 100, 100, 1.5f, 4096, 150, 150 },
            { 101, 101, 1.5f, 4096, 152, 152 },
            { 0, 0, 1.0f, 4096, 1, 1 },
            { 800, 0, 1.0f, 4096, 800, 1 },
            { 800, 600, nan, 4096, 800, 600 },
            { 800, 600, -1.0f, 4096, 800, 600 },
            { 800, 600, 0.0f, 4096, 800, 600 },
            { 800, 600, inf, 4096, 800, 600 },
            { 800, 600, 100.0f, 4096, 4096, 4096 },
            { 800, 600, 8.0f, 4096, 4096, 4096 },
            { 800, 600, 0.01f, 4096, 200, 150 },
            { 0xFFFFFFFFu, 0xFFFFFFFFu, 1.0f, 4096, 4096, 4096 },
            { 10, 10, 1.0f, 0, 1, 1 },
            { 1, 1, 0.25f, 4096, 1, 1 },
        };
        for (const SizeCase& item : cases)
        {
            const BrowserPixelSize size = ComputePhysicalSize({ item.Width, item.Height, item.Scale }, item.Maximum);
            check.Expect(size.Width == item.ExpectedWidth && size.Height == item.ExpectedHeight,
                "physical size of " + std::to_string(item.Width) + "x" + std::to_string(item.Height) + " at scale "
                    + std::to_string(item.Scale) + " is " + std::to_string(size.Width) + "x" + std::to_string(size.Height));
        }
        return check.Ok;
    }

    // ---- mirror property ------------------------------------------------------------

    bool MirrorProperty(Spiral::Tests::ChoiceStream& stream, std::string& message)
    {
        constexpr u32 kMaximumDimension = 40;
        BrowserFrameMirror mirror(kMaximumDimension);
        u32 width = static_cast<u32>(stream.NextI64(1, kMaximumDimension, { 1, 2, 3, 4, 5, 8, 17, 40 }));
        u32 height = static_cast<u32>(stream.NextI64(1, kMaximumDimension, { 1, 2, 3, 4, 5, 8, 17, 40 }));
        u32 stride = width * 4;
        Bytes truth;
        Image previousMirror;
        std::vector<std::vector<bool>> changedSincePull;
        bool haveFrame = false;
        const size_t steps = stream.NextSize(1, 14);

        for (size_t step = 0; step < steps; ++step)
        {
            bool resize = haveFrame && stream.Next() % 100 < 15;
            if (resize)
            {
                const u32 oldWidth = width;
                const u32 oldHeight = height;
                width = static_cast<u32>(stream.NextI64(1, kMaximumDimension, { 1, 2, 5, 40 }));
                height = static_cast<u32>(stream.NextI64(1, kMaximumDimension, { 1, 2, 5, 40 }));
                resize = width != oldWidth || height != oldHeight;
            }
            const bool fresh = !haveFrame || resize;
            if (fresh)
            {
                stride = width * 4 + static_cast<u32>(stream.NextSize(0, 3)) * 4;
                truth.assign(static_cast<size_t>(stride) * height, 0);
                for (u8& byte : truth)
                    byte = static_cast<u8>(stream.Next());
                changedSincePull.assign(height, std::vector<bool>(width, false));
            }

            std::vector<BrowserDirtyRect> declared;
            const size_t rectCount = fresh ? stream.NextSize(0, 3) : stream.NextSize(0, 5);
            for (size_t index = 0; index < rectCount; ++index)
            {
                BrowserDirtyRect rect;
                rect.X = static_cast<int>(stream.NextI64(-4, static_cast<std::int64_t>(width) + 3, { 0, static_cast<std::int64_t>(width) - 1 }));
                rect.Y = static_cast<int>(stream.NextI64(-4, static_cast<std::int64_t>(height) + 3, { 0, static_cast<std::int64_t>(height) - 1 }));
                rect.Width = static_cast<int>(stream.NextI64(-1, static_cast<std::int64_t>(width) + 3, { 0, 1 }));
                rect.Height = static_cast<int>(stream.NextI64(-1, static_cast<std::int64_t>(height) + 3, { 0, 1 }));
                declared.push_back(rect);
                if (fresh)
                    continue;
                // Change pixels strictly inside the clipped rectangle; the declared
                // rectangle may be larger than what changed, never smaller.
                for (int y = std::max(rect.Y, 0); y < std::min<int>(rect.Y + rect.Height, static_cast<int>(height)); ++y)
                {
                    for (int x = std::max(rect.X, 0); x < std::min<int>(rect.X + rect.Width, static_cast<int>(width)); ++x)
                    {
                        if (stream.Next() % 4 == 0)
                            continue;
                        const size_t offset = static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
                        for (size_t channel = 0; channel < 4; ++channel)
                            truth[offset + channel] = static_cast<u8>(stream.Next());
                        changedSincePull[static_cast<size_t>(y)][static_cast<size_t>(x)] = true;
                    }
                }
            }
            // An empty declared list means "no usable change set": the contract is a full copy.
            if (!fresh && declared.empty())
            {
                for (u8& byte : truth)
                    byte = static_cast<u8>(stream.Next());
                for (auto& row : changedSincePull)
                    std::fill(row.begin(), row.end(), true);
            }

            BrowserFrameView frame;
            frame.Bgra = truth.data();
            frame.Width = width;
            frame.Height = height;
            frame.StrideBytes = stride;
            frame.Dirty = declared;

            if (haveFrame && stream.Next() % 5 == 0)
            {
                // Rejected frames must be failure-atomic. A copy taken beforehand is
                // the oracle for the untouched state, including its pending dirty set.
                BrowserFrameMirror before = mirror;
                BrowserFrameView bad = frame;
                switch (stream.Next() % 5)
                {
                case 0: bad.Bgra = nullptr; break;
                case 1: bad.Width = 0; break;
                case 2: bad.Height = 0; break;
                case 3: bad.StrideBytes = width * 4 - 1; break;
                default: bad.Width = kMaximumDimension + 1; bad.StrideBytes = bad.Width * 4; break;
                }
                if (mirror.ApplyFrame(bad) != BrowserFrameMirror::ApplyResult::Rejected)
                {
                    message = "invalid frame was not rejected";
                    return false;
                }
                BrowserFrameMirror after = mirror;
                std::vector<BrowserDirtyRect> afterDirty, beforeDirty;
                after.TakeDirty(afterDirty);
                before.TakeDirty(beforeDirty);
                const auto afterPixels = after.Pixels();
                const auto beforePixels = before.Pixels();
                if (after.Width() != before.Width() || after.Height() != before.Height()
                    || after.Generation() != before.Generation() || after.ResizeSerial() != before.ResizeSerial()
                    || afterDirty != beforeDirty
                    || !std::equal(afterPixels.begin(), afterPixels.end(), beforePixels.begin(), beforePixels.end()))
                {
                    message = "rejected frame mutated the mirror";
                    return false;
                }
            }

            const u64 generationBefore = mirror.Generation();
            const u64 serialBefore = mirror.ResizeSerial();
            const auto result = mirror.ApplyFrame(frame);
            const bool expectResized = fresh;
            if (result != (expectResized ? BrowserFrameMirror::ApplyResult::Resized : BrowserFrameMirror::ApplyResult::Applied))
            {
                message = "unexpected apply result at step " + std::to_string(step);
                return false;
            }
            if (mirror.Generation() != generationBefore + 1 || (mirror.ResizeSerial() != serialBefore + (expectResized ? 1 : 0)))
            {
                message = "generation or resize serial counters wrong";
                return false;
            }
            haveFrame = true;

            const Image expected = ReferenceRgba(truth, width, height, stride);
            const auto pixels = mirror.Pixels();
            if (mirror.Width() != width || mirror.Height() != height || mirror.StrideBytes() != width * 4
                || !std::equal(pixels.begin(), pixels.end(), expected.Rgba.begin(), expected.Rgba.end()))
            {
                message = "mirror pixels differ from the per-pixel swizzle of the latest frame at step " + std::to_string(step);
                return false;
            }
            if (fresh)
            {
                std::vector<BrowserDirtyRect> dirty;
                mirror.TakeDirty(dirty);
                for (u32 y = 0; y < height; ++y)
                    for (u32 x = 0; x < width; ++x)
                        if (!PixelInRects(static_cast<int>(x), static_cast<int>(y), dirty))
                        {
                            message = "a resized mirror did not report the whole frame dirty";
                            return false;
                        }
                changedSincePull.assign(height, std::vector<bool>(width, false));
            }
            else if (stream.Next() % 2 == 0)
            {
                std::vector<BrowserDirtyRect> dirty;
                mirror.TakeDirty(dirty);
                if (dirty.size() > 8)
                {
                    message = "more than eight pending dirty rectangles";
                    return false;
                }
                for (size_t first = 0; first < dirty.size(); ++first)
                {
                    const BrowserDirtyRect& a = dirty[first];
                    if (a.Width <= 0 || a.Height <= 0 || a.X < 0 || a.Y < 0 || a.X + a.Width > static_cast<int>(width)
                        || a.Y + a.Height > static_cast<int>(height))
                    {
                        message = "dirty rectangle empty or outside the frame";
                        return false;
                    }
                    for (size_t second = first + 1; second < dirty.size(); ++second)
                    {
                        const BrowserDirtyRect& b = dirty[second];
                        if (a.X < b.X + b.Width && b.X < a.X + a.Width && a.Y < b.Y + b.Height && b.Y < a.Y + a.Height)
                        {
                            message = "pending dirty rectangles overlap";
                            return false;
                        }
                    }
                }
                for (u32 y = 0; y < height; ++y)
                    for (u32 x = 0; x < width; ++x)
                        if (changedSincePull[y][x] && !PixelInRects(static_cast<int>(x), static_cast<int>(y), dirty))
                        {
                            message = "a changed pixel was lost from the dirty set at step " + std::to_string(step);
                            return false;
                        }
                changedSincePull.assign(height, std::vector<bool>(width, false));
            }
        }
        return true;
    }

    bool CheckDirtyRectsAreTheCopyBoundary()
    {
        Checker check { "dirty-boundary" };
        // The mirror trusts the declared dirty set: a pixel changed outside it
        // keeps its previous value. This is the property that makes dirty-rect
        // copying cheaper than a full copy.
        constexpr u32 size = 4;
        Bytes first(size * size * 4, 10);
        Bytes second = first;
        second[0] = 99;                     // pixel (0,0) changes but is not declared
        second[(2 * size + 2) * 4] = 77;    // pixel (2,2) changes and is declared
        BrowserFrameMirror mirror;
        BrowserFrameView frame;
        frame.Bgra = first.data();
        frame.Width = size;
        frame.Height = size;
        frame.StrideBytes = size * 4;
        check.Expect(mirror.ApplyFrame(frame) == BrowserFrameMirror::ApplyResult::Resized, "first frame resizes");
        std::vector<BrowserDirtyRect> sink;
        mirror.TakeDirty(sink);
        const BrowserDirtyRect declared { 2, 2, 1, 1 };
        frame.Bgra = second.data();
        frame.Dirty = std::span<const BrowserDirtyRect>(&declared, 1);
        check.Expect(mirror.ApplyFrame(frame) == BrowserFrameMirror::ApplyResult::Applied, "second frame applies");
        check.Expect(mirror.Pixels()[2] == 10, "undeclared change at (0,0) is not copied (blue became red channel offset 2)");
        check.Expect(mirror.Pixels()[(2 * size + 2) * 4 + 2] == 77, "declared change at (2,2) is copied");
        std::vector<BrowserDirtyRect> dirty;
        mirror.TakeDirty(dirty);
        check.Expect(dirty.size() == 1 && dirty[0] == declared, "pending dirty set is exactly the declared rectangle");
        check.Expect(!mirror.HasDirty(), "TakeDirty clears the pending set");
        return check.Ok;
    }

    bool CoalesceProperty(Spiral::Tests::ChoiceStream& stream, std::string& message)
    {
        const u32 width = static_cast<u32>(stream.NextI64(1, 64, { 1, 2, 64 }));
        const u32 height = static_cast<u32>(stream.NextI64(1, 64, { 1, 2, 64 }));
        const size_t count = stream.NextSize(0, 90);
        const size_t maximumRects = stream.NextSize(1, 8);
        std::vector<BrowserDirtyRect> input;
        for (size_t index = 0; index < count; ++index)
        {
            input.push_back({ static_cast<int>(stream.NextI64(-10, static_cast<std::int64_t>(width) + 10)),
                static_cast<int>(stream.NextI64(-10, static_cast<std::int64_t>(height) + 10)),
                static_cast<int>(stream.NextI64(-2, static_cast<std::int64_t>(width) + 10, { 0, 1 })),
                static_cast<int>(stream.NextI64(-2, static_cast<std::int64_t>(height) + 10, { 0, 1 })) });
        }
        const std::vector<BrowserDirtyRect> output = CoalesceDirtyRects(input, width, height, maximumRects);
        if (output.size() > maximumRects)
        {
            message = "output exceeds the rectangle cap";
            return false;
        }
        for (size_t first = 0; first < output.size(); ++first)
        {
            const BrowserDirtyRect& a = output[first];
            if (a.Width <= 0 || a.Height <= 0 || a.X < 0 || a.Y < 0 || a.X + a.Width > static_cast<int>(width)
                || a.Y + a.Height > static_cast<int>(height))
            {
                message = "output rectangle empty or outside the frame";
                return false;
            }
            for (size_t second = first + 1; second < output.size(); ++second)
            {
                const BrowserDirtyRect& b = output[second];
                if (a.X < b.X + b.Width && b.X < a.X + a.Width && a.Y < b.Y + b.Height && b.Y < a.Y + a.Height)
                {
                    message = "output rectangles overlap";
                    return false;
                }
            }
        }
        // Coverage: every pixel of every input rectangle that lies inside the frame is covered.
        for (int y = 0; y < static_cast<int>(height); ++y)
        {
            for (int x = 0; x < static_cast<int>(width); ++x)
            {
                bool required = false;
                for (const BrowserDirtyRect& rect : input)
                    required = required || (rect.Width > 0 && rect.Height > 0 && x >= rect.X && x < rect.X + rect.Width
                        && y >= rect.Y && y < rect.Y + rect.Height);
                if (required && !PixelInRects(x, y, output))
                {
                    message = "pixel (" + std::to_string(x) + "," + std::to_string(y) + ") of an input rectangle is not covered";
                    return false;
                }
            }
        }
        return true;
    }

    bool CheckCoalesceExamples()
    {
        Checker check { "coalesce-examples" };
        const auto same = [](const std::vector<BrowserDirtyRect>& actual, std::vector<BrowserDirtyRect> expected)
        {
            return actual == expected;
        };
        check.Expect(CoalesceDirtyRects({}, 10, 10).empty(), "no rectangles stay empty");
        const BrowserDirtyRect single { 1, 2, 3, 4 };
        check.Expect(same(CoalesceDirtyRects(std::span<const BrowserDirtyRect>(&single, 1), 10, 10), { single }),
            "a single rectangle is unchanged");
        const BrowserDirtyRect pair[] = { { 0, 0, 2, 2 }, { 8, 8, 2, 2 } };
        check.Expect(same(CoalesceDirtyRects(pair, 10, 10), { pair[0], pair[1] }), "far-apart rectangles stay separate");
        const BrowserDirtyRect overlap[] = { { 0, 0, 4, 4 }, { 2, 2, 4, 4 } };
        check.Expect(same(CoalesceDirtyRects(overlap, 10, 10), { { 0, 0, 6, 6 } }), "overlapping rectangles merge to their bounding box");
        const BrowserDirtyRect outside[] = { { 8, 8, 10, 10 }, { -5, -5, 6, 6 }, { 20, 20, 3, 3 }, { 0, 0, 0, 5 } };
        check.Expect(same(CoalesceDirtyRects(outside, 10, 10), { { 8, 8, 2, 2 }, { 0, 0, 1, 1 } }),
            "rectangles are clipped to the frame and empty or fully outside ones are dropped");
        std::vector<BrowserDirtyRect> nine;
        for (int index = 0; index < 9; ++index)
            nine.push_back({ index * 3, 0, 1, 1 });
        const std::vector<BrowserDirtyRect> capped = CoalesceDirtyRects(nine, 40, 4, 8);
        check.Expect(capped.size() == 8, "nine disjoint rectangles reduce to the cap of eight");
        std::vector<BrowserDirtyRect> many;
        for (int index = 0; index < 100; ++index)
            many.push_back({ index % 10, index / 10, 1, 1 });
        check.Expect(same(CoalesceDirtyRects(many, 10, 10), { { 0, 0, 10, 10 } }), "more than 64 rectangles collapse to their bounding box");
        return check.Ok;
    }
}

namespace
{
    // ---- navigation policy -----------------------------------------------------

    const char* VerdictName(BrowserNavigationVerdict verdict)
    {
        switch (verdict)
        {
        case BrowserNavigationVerdict::Allow: return "Allow";
        case BrowserNavigationVerdict::AllowSubFrame: return "AllowSubFrame";
        case BrowserNavigationVerdict::DenyPopup: return "DenyPopup";
        case BrowserNavigationVerdict::DenyScheme: return "DenyScheme";
        case BrowserNavigationVerdict::DenyMalformedUrl: return "DenyMalformedUrl";
        case BrowserNavigationVerdict::DenyHost: return "DenyHost";
        }
        return "?";
    }

    bool CheckNavigationPolicy()
    {
        Checker check { "navigation" };
        BrowserNavigationPolicy policy;

        const auto expectVerdict = [&](const BrowserNavigationPolicy& subject, std::string_view url, BrowserNavigationVerdict expected)
        {
            const BrowserNavigationVerdict actual = subject.Evaluate(url, BrowserNavigationKind::TopLevel);
            check.Expect(actual == expected,
                "top-level '" + std::string(url.substr(0, 80)) + "' expected " + VerdictName(expected) + " got " + VerdictName(actual));
            check.Expect(subject.IsTopLevelAllowed(url) == (expected == BrowserNavigationVerdict::Allow),
                "IsTopLevelAllowed agrees with Evaluate for '" + std::string(url.substr(0, 80)) + "'");
        };

        // Hand-built tables. Every row is an independent claim about a real
        // browser's parsing of the authority, not derived from the code.
        for (const char* url : {
                 "https://www.fab.com/",
                 "https://www.fab.com",
                 "https://WWW.FAB.COM/",
                 "https://www.fab.com/listings/01234567-89ab-cdef-0123-456789abcdef?format=glb#files",
                 "https://www.epicgames.com/id/login",
                 "https://www.epicgames.com/id/authorize?client_id=x&redirect_uri=https%3A%2F%2Fwww.fab.com%2Fsocial%2Fcomplete%2Fepic%2F",
                 // The authority ends at '#' or '?', so these are www.fab.com pages.
                 "https://www.fab.com#@evil.example/",
                 "https://www.fab.com?@evil.example/" })
        {
            expectVerdict(policy, url, BrowserNavigationVerdict::Allow);
        }

        for (const char* url : { "http://www.fab.com/", "file:///etc/passwd", "javascript:alert(1)", "data:text/html,x",
                 "about:blank", "blob:https://www.fab.com/0a1b", "ftp://www.fab.com/", "view-source:https://www.fab.com/",
                 "chrome://version", "devtools://devtools/bundled/inspector.html", "ws://www.fab.com/", "wss://www.fab.com/",
                 "https:/www.fab.com/", "https:www.fab.com", "www.fab.com", "//www.fab.com/", "" })
        {
            expectVerdict(policy, url, BrowserNavigationVerdict::DenyScheme);
        }

        for (const char* url : { "https://fab.com/", "https://www.fab.com.evil.example/", "https://evil.example/www.fab.com",
                 "https://evilwww.fab.com/", "https://www.fab.com./", "https://sub.www.fab.com/", "https://wwww.fab.com/",
                 "https://www-fab.com/", "https://www.fab.co/", "https://www.fab.com.cn/", "https://www.epicgames.com.evil/",
                 "https://epicgames.com/", "https://127.0.0.1/", "https://2130706433/", "https://0x7f.0.0.1/", "https://0177.0.0.1/",
                 "https://xn--fab-9ra.com/", "https://www.xn--fab-9ra.com/", "https://localhost/" })
        {
            expectVerdict(policy, url, BrowserNavigationVerdict::DenyHost);
        }

        const std::string oversized = "https://www.fab.com/" + std::string(2048, 'a');
        const std::string atLimit = "https://www.fab.com/" + std::string(2048 - 20, 'a');
        expectVerdict(policy, atLimit, BrowserNavigationVerdict::Allow);
        expectVerdict(policy, oversized, BrowserNavigationVerdict::DenyMalformedUrl);
        for (const char* url : { "https://user@www.fab.com/", "https://user:pw@www.fab.com/", "https://www.fab.com@evil.example/",
                 "https://www.fab.com:443/", "https://www.fab.com:8443/", "https://www.fab.com:80@evil.example/",
                 "https://[::1]/", "https://[::ffff:127.0.0.1]/", "https://www.fab.com\\@evil.example/",
                 "https://www.fab.com\\.evil.example/", "https://www.fab.com/\n", "https://www.fab.com/a b",
                 "https://www.fab.com/\t", "https://www.fab.com\r\nHost: evil", "https://www.fab.com/\x01",
                 "https://\xEF\xBD\x97\xEF\xBD\x97\xEF\xBD\x97.fab.com/", "https://www.f\xD0\xB0" "b.com/",
                 "https://www.fab.com%2eevil.example/", "https://www.fab.com%00.evil.example/", "https://www.fab.com%40evil.example/",
                 "HTTPS://www.fab.com/", "Https://www.fab.com/", "https:///www.fab.com/", "https://", "https://:443/", "https://@/" })
        {
            expectVerdict(policy, url, BrowserNavigationVerdict::DenyMalformedUrl);
        }

        for (const char* url : { "https://www.fab.com/", "https://evil.example/", "javascript:1", "" })
        {
            check.Expect(policy.Evaluate(url, BrowserNavigationKind::Popup) == BrowserNavigationVerdict::DenyPopup,
                std::string("popup is always denied: '") + url + "'");
            check.Expect(policy.Evaluate(url, BrowserNavigationKind::SubFrame) == BrowserNavigationVerdict::AllowSubFrame,
                std::string("sub-frames are outside the top-level allowlist: '") + url + "'");
        }
        check.Expect(IsNavigationAllowed(BrowserNavigationVerdict::AllowSubFrame) && IsNavigationAllowed(BrowserNavigationVerdict::Allow)
                && !IsNavigationAllowed(BrowserNavigationVerdict::DenyPopup) && !IsNavigationAllowed(BrowserNavigationVerdict::DenyHost),
            "IsNavigationAllowed classifies every verdict");

        // The provider list starts empty and is data-driven.
        check.Expect(policy.ProviderHosts().empty(), "provider host list starts empty");
        check.Expect(policy.AllowedHosts().size() == 2, "exactly the two built-in hosts at construction");
        expectVerdict(policy, "https://accounts.example.com/signin", BrowserNavigationVerdict::DenyHost);

        const size_t hostCountBefore = policy.AllowedHosts().size();
        const std::string longLabel(64, 'a');
        const std::string longHost = std::string(250, 'a') + ".com";
        for (const std::string& invalid : std::vector<std::string> {
                 "", ".example.com", "example.com.", "*.example.com", "example", "EXAMPLE.com", "Example.com",
                 "xn--e1afmkfd.xn--p1ai", "example.xn--p1ai", "127.0.0.1", "1.2.3.4", "example.123", "exa mple.com",
                 "example..com", "-bad.example.com", "bad-.example.com", "a.b:443", "user@example.com", "[::1]",
                 "example.com/path", "example.com?x", longLabel + ".com", longHost, "www.fab.com", "www.epicgames.com", "exa_mple.com" })
        {
            std::string error;
            const bool added = policy.AddProviderHost(invalid, error);
            check.Expect(!added && !error.empty(), "provider host '" + invalid.substr(0, 40) + "' is rejected with an error");
            check.Expect(policy.AllowedHosts().size() == hostCountBefore, "a rejected provider host leaves the policy unchanged");
        }
        std::string error;
        check.Expect(policy.AddProviderHost("accounts.example.com", error) && error.empty(), "a lower-case two-label host is accepted");
        check.Expect(!policy.AddProviderHost("accounts.example.com", error), "a duplicate provider host is rejected");
        check.Expect(policy.ProviderHosts().size() == 1 && policy.ProviderHosts()[0] == "accounts.example.com",
            "ProviderHosts reports the added host only");
        expectVerdict(policy, "https://accounts.example.com/signin", BrowserNavigationVerdict::Allow);
        expectVerdict(policy, "https://evil.accounts.example.com/", BrowserNavigationVerdict::DenyHost);
        expectVerdict(policy, "https://example.com/", BrowserNavigationVerdict::DenyHost);
        expectVerdict(policy, "https://accounts.example.com:8443/", BrowserNavigationVerdict::DenyMalformedUrl);
        expectVerdict(policy, "https://accounts.example.com.evil/", BrowserNavigationVerdict::DenyHost);
        expectVerdict(policy, "http://accounts.example.com/", BrowserNavigationVerdict::DenyScheme);

        // Logging and display text never carry credentials, query, or fragment.
        const std::pair<const char*, const char*> logCases[] = {
            { "https://www.fab.com/path?token=secret", "www.fab.com" },
            { "https://WWW.Fab.COM/", "www.fab.com" },
            { "https://user:pw@evil.example/", "<invalid-host>" },
            { "https://EVIL.example:8080/", "<invalid-host>" },
            { "javascript:alert(1)", "<invalid-host>" },
            { "https://evil.example\n.x/", "<invalid-host>" },
            { "https:///", "<invalid-host>" },
            { "", "<invalid-host>" },
        };
        for (const auto& [url, expected] : logCases)
            check.Expect(BrowserNavigationPolicy::HostForLog(url) == expected, std::string("HostForLog('") + url + "')");
        check.Expect(BrowserNavigationPolicy::HostForLog("https://" + std::string(254, 'a') + "/") == "<invalid-host>",
            "an over-long host is not echoed");

        const std::pair<const char*, const char*> displayCases[] = {
            { "https://www.fab.com/listings/abc?token=secret#frag", "www.fab.com/listings/abc" },
            { "https://user:pw@www.fab.com/x", "www.fab.com/x" },
            { "https://www.fab.com", "www.fab.com" },
            { "https://www.fab.com/a\x01" "b", "www.fab.com/a?b" },
            { "not a url", "" },
        };
        for (const auto& [url, expected] : displayCases)
            check.Expect(BrowserNavigationPolicy::DisplayAddress(url) == expected, std::string("DisplayAddress('") + url + "')");
        check.Expect(BrowserNavigationPolicy::DisplayAddress("https://www.fab.com/" + std::string(500, 'p')).size() == 200,
            "DisplayAddress is capped at 200 bytes");
        return check.Ok;
    }

    // ---- download policy -------------------------------------------------------

    bool IsWindowsReservedStemReference(std::string_view name)
    {
        std::string stem(name.substr(0, name.find('.')));
        for (char& c : stem)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        static const std::set<std::string> reserved { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4",
            "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
        return reserved.contains(stem);
    }

    bool SanitizerProperty(Spiral::Tests::ChoiceStream& stream, std::string& message)
    {
        static constexpr std::string_view interesting = "./\\:.. aZ09-_%\n\x01\x7f\xC3\xA9<>|?*\"~";
        std::string input;
        const size_t length = stream.NextSize(0, 200);
        for (size_t index = 0; index < length; ++index)
        {
            const u64 choice = stream.Next();
            input.push_back(choice % 3 == 0 ? static_cast<char>(choice >> 8) : interesting[(choice >> 8) % interesting.size()]);
        }
        const size_t maximum = stream.NextSize(8, 160);
        const std::string name = SanitizeDownloadFileName(input, maximum);
        const auto fail = [&](const char* what)
        {
            message = std::string(what) + " for input of " + std::to_string(input.size()) + " bytes, output '" + name + "'";
            return false;
        };
        if (name.empty() || name.size() > maximum)
            return fail("empty or over-long name");
        if (!std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '-' || c == '_'; })
            || name.front() == '.')
            return fail("illegal character or leading dot");
        if (name.back() == '.' || std::count(name.begin(), name.end(), '.') > 1 || name == "." || name == "..")
            return fail("trailing dot, second dot, or dot-only name");
        if (IsWindowsReservedStemReference(name))
            return fail("Windows reserved device name");
        if (SanitizeDownloadFileName(name, maximum) != name)
            return fail("not idempotent");
        const size_t dot = name.find('.');
        if (dot != std::string::npos && std::any_of(name.begin() + static_cast<std::ptrdiff_t>(dot), name.end(),
                [](char c) { return c >= 'A' && c <= 'Z'; }))
            return fail("extension not lower-case");
        return true;
    }

    bool CheckDownloadPolicy()
    {
        Checker check { "download" };

        const std::string longStem(300, 'A');
        const std::pair<std::string, std::string> sanitizerCases[] = {
            { "model.zip", "model.zip" },
            { "Model.ZIP", "Model.zip" },
            { "../../etc/passwd.zip", "passwd.zip" },
            { "..\\..\\win.zip", "win.zip" },
            { "/abs/path/a.glb", "a.glb" },
            { "C:\\Users\\x\\a.gltf", "a.gltf" },
            { "~/a.zip", "a.zip" },
            { "a.zip.", "a.zip" },
            { "a.zip...", "a.zip" },
            { "a.zip ", "a.zip_" },
            { "a.b.c.zip", "a_b_c.zip" },
            { "model.exe.zip", "model_exe.zip" },
            { "model.zip.exe", "model_zip.exe" },
            { "a.zip:evil.exe", "a_zip_evil.exe" },
            { ".hidden.zip", "hidden.zip" },
            { "...zip", "zip" },
            { "a/.zip", "zip" },
            { ".", "download" },
            { "..", "download" },
            { "", "download" },
            { "////", "download" },
            { "dir/", "download" },
            { "con.zip", "_con.zip" },
            { "CON", "_CON" },
            { "nul.glb", "_nul.glb" },
            { "COM1.zip", "_COM1.zip" },
            { "com10.zip", "com10.zip" },
            { "lpt9.gltf", "_lpt9.gltf" },
            { "my model (1).zip", "my_model__1_.zip" },
            { "\xC3\xBCn\xC3\xAF" "code.zip", "__n__code.zip" },
            { std::string("a\0b.zip", 7), "a_b.zip" },
            { "a\nb.zip", "a_b.zip" },
            { "%2e%2e%2fa.zip", "_2e_2e_2fa.zip" },
            { "a%00.zip", "a_00.zip" },
            { "-rf.zip", "-rf.zip" },
            { longStem + ".zip", std::string(124, 'A') + ".zip" },
            { "a." + std::string(100, 'x'), "a." + std::string(16, 'x') },
        };
        for (const auto& [input, expected] : sanitizerCases)
        {
            const std::string actual = SanitizeDownloadFileName(input);
            check.Expect(actual == expected, "sanitize '" + input.substr(0, 40) + "' expected '" + expected.substr(0, 40)
                    + "' got '" + actual.substr(0, 40) + "'");
        }
        const std::string shortened = SanitizeDownloadFileName(longStem + ".glb", 16);
        check.Expect(shortened == std::string(12, 'A') + ".glb", "a small cap keeps the extension and truncates the stem");
        check.Expect(ExtensionOfSanitizedName("a.zip") == "zip" && ExtensionOfSanitizedName("a").empty()
                && ExtensionOfSanitizedName("a.") .empty(),
            "ExtensionOfSanitizedName returns the text after the last dot");

        check.Expect(RunProperty("download-sanitizer", SanitizerProperty, 600), "sanitizer property: portable, traversal-free, idempotent");

        // Decisions.
        BrowserDownloadPolicy policy;
        const BrowserDownloadLimits defaults;
        const auto accepted = policy.Evaluate({ "Fab-Asset.zip", 1000 }, 0xABCDEFull);
        check.Expect(accepted.Verdict == BrowserDownloadVerdict::Accept && accepted.SanitizedName == "Fab-Asset.zip",
            "a zip is accepted with its sanitized name");
        check.Expect(accepted.StagingDirectoryName == "d00000000-0000000000abcdef", "first staging directory is d<seq>-<nonce>");
        check.Expect(accepted.RelativeStagedPath == std::filesystem::path("d00000000-0000000000abcdef") / "Fab-Asset.zip",
            "relative staged path is directory/name");

        for (const auto& [name, expected] : std::vector<std::pair<std::string, BrowserDownloadVerdict>> {
                 { "x.exe", BrowserDownloadVerdict::RejectExtension }, { "x", BrowserDownloadVerdict::RejectExtension },
                 { "x.zip.exe", BrowserDownloadVerdict::RejectExtension }, { "x.7z", BrowserDownloadVerdict::RejectExtension },
                 { "x.zip_", BrowserDownloadVerdict::RejectExtension }, { "x.zip:evil.exe", BrowserDownloadVerdict::RejectExtension },
                 { "", BrowserDownloadVerdict::RejectExtension }, { "..", BrowserDownloadVerdict::RejectExtension },
                 { "x.ZIP", BrowserDownloadVerdict::Accept }, { "x.Glb", BrowserDownloadVerdict::Accept },
                 { "x.gltf", BrowserDownloadVerdict::Accept }, { "../../x.zip", BrowserDownloadVerdict::Accept } })
        {
            const auto decision = policy.Evaluate({ name, 10 }, 1);
            check.Expect(decision.Verdict == expected, "verdict for suggested name '" + name + "'");
            const bool accept = decision.Verdict == BrowserDownloadVerdict::Accept;
            check.Expect(accept == !decision.StagingDirectoryName.empty() && accept == !decision.RelativeStagedPath.empty(),
                "only an accepted download carries staging names");
            check.Expect(decision.SanitizedName.find('/') == std::string::npos && decision.SanitizedName.find('\\') == std::string::npos,
                "a decision never carries a path separator");
        }

        BrowserDownloadPolicy sizePolicy;
        check.Expect(sizePolicy.Evaluate({ "a.zip", defaults.MaximumBytes }, 0).Verdict == BrowserDownloadVerdict::Accept,
            "exactly the size limit is accepted");
        check.Expect(sizePolicy.Evaluate({ "a.zip", defaults.MaximumBytes + 1 }, 0).Verdict == BrowserDownloadVerdict::RejectSize,
            "one byte over the size limit is rejected");
        check.Expect(sizePolicy.Evaluate({ "a.zip", 0 }, 0).Verdict == BrowserDownloadVerdict::Accept, "an unknown length is accepted");
        check.Expect(sizePolicy.Evaluate({ "a.exe", defaults.MaximumBytes + 1 }, 0).Verdict == BrowserDownloadVerdict::RejectExtension,
            "type is checked before size");
        check.Expect(sizePolicy.WithinSizeLimit(defaults.MaximumBytes) && !sizePolicy.WithinSizeLimit(defaults.MaximumBytes + 1)
                && sizePolicy.WithinSizeLimit(0),
            "WithinSizeLimit boundary");

        BrowserDownloadLimits glbOnly;
        glbOnly.AllowedExtensions = { "glb" };
        glbOnly.MaximumBytes = 100;
        BrowserDownloadPolicy custom(glbOnly);
        check.Expect(custom.Evaluate({ "a.zip", 1 }, 0).Verdict == BrowserDownloadVerdict::RejectExtension
                && custom.Evaluate({ "a.glb", 100 }, 0).Verdict == BrowserDownloadVerdict::Accept
                && custom.Evaluate({ "a.glb", 101 }, 0).Verdict == BrowserDownloadVerdict::RejectSize,
            "custom limits are honoured");

        // Rejections do not consume sequence numbers; accepts never collide, even
        // with a constant nonce and under ASCII case folding.
        BrowserDownloadPolicy sequence;
        check.Expect(sequence.Evaluate({ "a.zip", 1 }, 7).StagingDirectoryName == "d00000000-0000000000000007", "sequence starts at zero");
        sequence.Evaluate({ "a.exe", 1 }, 7);
        sequence.Evaluate({ "a.zip", defaults.MaximumBytes + 1 }, 7);
        check.Expect(sequence.Evaluate({ "a.zip", 1 }, 7).StagingDirectoryName == "d00000001-0000000000000007",
            "rejections do not consume a sequence number");
        std::set<std::string> folded;
        for (int index = 0; index < 2000; ++index)
        {
            std::string directory = sequence.Evaluate({ "Same.ZIP", 1 }, 0xFFFFFFFFFFFFFFFFull).StagingDirectoryName;
            std::transform(directory.begin(), directory.end(), directory.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            check.Expect(folded.insert(directory).second, "staging directory names are unique after case folding");
        }

        // Path resolution proves containment even for hand-made hostile decisions.
        const std::filesystem::path root = std::filesystem::path("/spiral-fab-test") / "staging";
        std::string error;
        const std::filesystem::path resolved = BrowserDownloadPolicy::ResolveStagedPath(root, accepted, error);
        check.Expect(resolved == root / "d00000000-0000000000abcdef" / "Fab-Asset.zip" && error.empty(), "a policy decision resolves under the root");
        const auto hostile = [&](std::string directory, std::string name, BrowserDownloadVerdict verdict)
        {
            BrowserDownloadDecision decision;
            decision.Verdict = verdict;
            decision.StagingDirectoryName = std::move(directory);
            decision.SanitizedName = std::move(name);
            std::string localError;
            const std::filesystem::path path = BrowserDownloadPolicy::ResolveStagedPath(root, decision, localError);
            return path.empty() && !localError.empty();
        };
        check.Expect(hostile("..", "a.zip", BrowserDownloadVerdict::Accept), "directory .. is rejected");
        check.Expect(hostile(".", "a.zip", BrowserDownloadVerdict::Accept), "directory . is rejected");
        check.Expect(hostile("", "a.zip", BrowserDownloadVerdict::Accept), "empty directory is rejected");
        check.Expect(hostile("d1", "../a.zip", BrowserDownloadVerdict::Accept), "name with traversal is rejected");
        check.Expect(hostile("d1", "..", BrowserDownloadVerdict::Accept), "name .. is rejected");
        check.Expect(hostile("d1", "", BrowserDownloadVerdict::Accept), "empty name is rejected");
        check.Expect(hostile("d1", "a/b.zip", BrowserDownloadVerdict::Accept), "name with separator is rejected");
        check.Expect(hostile("/etc", "a.zip", BrowserDownloadVerdict::Accept), "absolute directory is rejected");
        check.Expect(hostile("a/b", "a.zip", BrowserDownloadVerdict::Accept), "multi-segment directory is rejected");
        check.Expect(hostile("d1", "a:b.zip", BrowserDownloadVerdict::Accept), "name with colon is rejected");
        check.Expect(hostile("d1", "a.zip", BrowserDownloadVerdict::RejectExtension), "a non-accepted decision does not resolve");
        {
            BrowserDownloadDecision decision = accepted;
            std::string localError;
            check.Expect(BrowserDownloadPolicy::ResolveStagedPath({}, decision, localError).empty() && !localError.empty(),
                "an empty staging root is rejected");
        }

        // Log lines are built from sanitized text only.
        const auto blocked = policy.Evaluate({ "../../secret-token=abc/evil.zip?sig=XYZ&X-Amz-Credential=KEY", 5 }, 0);
        const std::string line = BrowserDownloadPolicy::FormatLogLine(blocked, 5);
        check.Expect(blocked.Verdict == BrowserDownloadVerdict::RejectExtension, "a query-string tail makes the extension unsupported");
        check.Expect(line.find("secret") == std::string::npos && line.find("/") == std::string::npos
                && line.find("sig=") == std::string::npos && line.find("http") == std::string::npos,
            "the log line contains no path, query, or token text");
        check.Expect(BrowserDownloadPolicy::FormatLogLine(accepted, 1000) == "download accepted name=Fab-Asset.zip declaredBytes=1000",
            "accepted log line format");
        return check.Ok;
    }

    // ---- key translation oracle -------------------------------------------------

    bool CheckKeyTranslation()
    {
        Checker check { "key-translation" };
        std::map<int, int> expected;
        // Letters and digits: GLFW uses ASCII, Windows VK_A..VK_Z / VK_0..VK_9 use ASCII too.
        for (int letter = 0; letter < 26; ++letter)
            expected[65 + letter] = 0x41 + letter;
        for (int digit = 0; digit < 10; ++digit)
            expected[48 + digit] = 0x30 + digit;
        for (int function = 0; function < 24; ++function)
            expected[290 + function] = 0x70 + function;
        for (int keypad = 0; keypad < 10; ++keypad)
            expected[320 + keypad] = 0x60 + keypad;
        const std::pair<int, int> table[] = {
            { 32, 0x20 }, { 39, 0xDE }, { 44, 0xBC }, { 45, 0xBD }, { 46, 0xBE }, { 47, 0xBF }, { 59, 0xBA }, { 61, 0xBB },
            { 91, 0xDB }, { 92, 0xDC }, { 93, 0xDD }, { 96, 0xC0 }, { 161, 0xE2 },
            { 256, 0x1B }, { 257, 0x0D }, { 258, 0x09 }, { 259, 0x08 }, { 260, 0x2D }, { 261, 0x2E },
            { 262, 0x27 }, { 263, 0x25 }, { 264, 0x28 }, { 265, 0x26 },
            { 266, 0x21 }, { 267, 0x22 }, { 268, 0x24 }, { 269, 0x23 },
            { 280, 0x14 }, { 281, 0x91 }, { 282, 0x90 }, { 283, 0x2C }, { 284, 0x13 },
            { 330, 0x6E }, { 331, 0x6F }, { 332, 0x6A }, { 333, 0x6D }, { 334, 0x6B }, { 335, 0x0D },
            { 340, 0x10 }, { 341, 0x11 }, { 342, 0x12 }, { 343, 0x5B },
            { 344, 0x10 }, { 345, 0x11 }, { 346, 0x12 }, { 347, 0x5C }, { 348, 0x5D },
        };
        for (const auto& [glfw, vk] : table)
            expected[glfw] = vk;

        for (int key = -10; key <= 400; ++key)
        {
            const auto found = expected.find(key);
            const int actual = TranslateGlfwKeyToWindowsVirtualKey(key);
            check.Expect(actual == (found == expected.end() ? 0 : found->second),
                "GLFW key " + std::to_string(key) + " translates to " + std::to_string(actual));
        }
        check.Expect(TranslateGlfwKeyToWindowsVirtualKey(314) == 0 && TranslateGlfwKeyToWindowsVirtualKey(336) == 0
                && TranslateGlfwKeyToWindowsVirtualKey(162) == 0 && TranslateGlfwKeyToWindowsVirtualKey(-1) == 0
                && TranslateGlfwKeyToWindowsVirtualKey(std::numeric_limits<int>::max()) == 0
                && TranslateGlfwKeyToWindowsVirtualKey(std::numeric_limits<int>::min()) == 0,
            "keys with no Windows equivalent (F25, KP equal, world 2, unknown, extremes) translate to zero");

        BrowserMouseButton button = BrowserMouseButton::Middle;
        check.Expect(TranslateGlfwMouseButton(0, button) && button == BrowserMouseButton::Left, "GLFW button 0 is left");
        check.Expect(TranslateGlfwMouseButton(1, button) && button == BrowserMouseButton::Right, "GLFW button 1 is right");
        check.Expect(TranslateGlfwMouseButton(2, button) && button == BrowserMouseButton::Middle, "GLFW button 2 is middle");
        button = BrowserMouseButton::Right;
        check.Expect(!TranslateGlfwMouseButton(3, button) && !TranslateGlfwMouseButton(-1, button) && !TranslateGlfwMouseButton(7, button)
                && button == BrowserMouseButton::Right,
            "other GLFW buttons are refused and leave the output untouched");

        check.Expect(BrowserModifier::Shift == 1 && BrowserModifier::Control == 2 && BrowserModifier::Alt == 4 && BrowserModifier::Super == 8
                && BrowserModifier::CapsLock == 16 && BrowserModifier::NumLock == 32 && BrowserModifier::KeyMask == 63,
            "BrowserModifier key bits equal GLFW_MOD_* so conversion is a mask");
        return check.Ok;
    }
}

namespace
{
    // ---- input router scenarios ---------------------------------------------------

    BrowserPanelState Panel(bool focused, bool hovered)
    {
        BrowserPanelState state;
        state.Surface = { 100.0f, 50.0f, 400.0f, 300.0f };
        state.PanelVisible = true;
        state.PanelFocused = focused;
        state.SurfaceHovered = hovered;
        return state;
    }

    constexpr u32 kCtrl = BrowserModifier::Control;
    constexpr int kPress = 1;
    constexpr int kRelease = 0;
    constexpr int kRepeat = 2;

    // Click inside the surface, let the UI phase focus the panel, release.
    void GainKeyboard(BrowserInputRouter& router, u64 time = 1000)
    {
        router.UpdatePanel(Panel(false, true));
        router.OnMouseButton(BrowserMouseButton::Left, true, 200, 100, 0, time);
        router.UpdatePanel(Panel(true, true));
        router.OnMouseButton(BrowserMouseButton::Left, false, 200, 100, 0, time + 10);
    }

    bool CheckRouterKeyboard()
    {
        Checker check { "router-keyboard" };
        BrowserInputRouter router;
        router.UpdatePanel(Panel(false, true));

        // Before any click the editor owns every key: Ctrl+Z / Y and the viewport F must work.
        for (const int key : { 90, 89, 70, 256 })
        {
            const auto route = router.OnKey(key, 0, kPress, kCtrl);
            check.Expect(!route.Browser && route.ImGui && route.EditorShortcuts, "before a click, key " + std::to_string(key) + " is the editor's");
        }
        const auto editorChar = router.OnChar(U'a', 0);
        check.Expect(!editorChar.Browser && editorChar.ImGui && editorChar.EditorShortcuts, "before a click, typed text is the editor's");

        const auto down = router.OnMouseButton(BrowserMouseButton::Left, true, 200, 100, 0, 1000);
        check.Expect(down.Browser && down.ImGui && !down.EditorShortcuts && down.ButtonDown && down.ClickCount == 1
                && down.Mouse.X == 100.0f && down.Mouse.Y == 50.0f && (down.Mouse.Modifiers & BrowserModifier::LeftButton) != 0,
            "a click inside the surface goes to the browser with view-local coordinates");
        check.Expect(router.HasMouseCapture() && !router.OwnsKeyboard() && !down.KeyboardOwnerChanged,
            "the keyboard is not owned until ImGui has focused the panel");
        const auto focused = router.UpdatePanel(Panel(true, true));
        check.Expect(focused.KeyboardOwnerChanged && router.OwnsKeyboard() && !focused.Browser && focused.SyntheticKeyUps.empty(),
            "the UI phase that focuses the panel grants the keyboard");
        const auto release = router.OnMouseButton(BrowserMouseButton::Left, false, 200, 100, 0, 1050);
        check.Expect(release.Browser && !release.ButtonDown && !router.HasMouseCapture(), "the matching release reaches the browser");

        // The guard: no editor shortcut may fire while a web page has the keyboard.
        for (const int key : { 90, 89, 70 })
        {
            const auto press = router.OnKey(key, 44, kPress, kCtrl);
            check.Expect(press.Browser && !press.ImGui && !press.EditorShortcuts && press.Key.Kind == BrowserKey::Phase::Down
                    && press.Key.GlfwKey == key && press.Key.WindowsVirtualKey == key && press.Key.Scancode == 44
                    && press.Key.Modifiers == kCtrl && !press.Key.Repeat,
                "owned key " + std::to_string(key) + " reaches only the browser");
            const auto up = router.OnKey(key, 44, kRelease, kCtrl);
            check.Expect(up.Browser && !up.EditorShortcuts && up.Key.Kind == BrowserKey::Phase::Up, "owned key release reaches the browser");
        }
        const auto repeat = router.OnKey(70, 33, kRepeat, 0);
        check.Expect(repeat.Browser && repeat.Key.Repeat && repeat.Key.Kind == BrowserKey::Phase::Down, "a repeat is a repeated down");
        router.OnKey(70, 33, kRelease, 0);
        const auto escape = router.OnKey(256, 9, kPress, 0);
        check.Expect(escape.Browser && escape.Key.WindowsVirtualKey == 0x1B, "Escape is forwarded to the page, not consumed");
        router.OnKey(256, 9, kRelease, 0);
        const auto typed = router.OnChar(U'x', 0);
        check.Expect(typed.Browser && !typed.EditorShortcuts && !typed.ImGui && typed.Key.Kind == BrowserKey::Phase::Char
                && typed.Key.Codepoint == U'x',
            "typed text goes to the browser");
        const auto emoji = router.OnChar(U'\U0001F600', BrowserModifier::Shift);
        check.Expect(emoji.Browser && emoji.Key.Codepoint == U'\U0001F600' && emoji.Key.Modifiers == BrowserModifier::Shift,
            "a supplementary-plane code point is preserved");
        for (const char32_t invalid : { U'\0', U'\x1F', U'\x7F', U'\x9F', char32_t(0xD800), char32_t(0xDFFF), char32_t(0x110000) })
        {
            const auto route = router.OnChar(invalid, 0);
            check.Expect(!route.Browser && !route.ImGui && !route.EditorShortcuts,
                "invalid typed code point U+" + std::to_string(static_cast<u32>(invalid)) + " is swallowed");
        }
        check.Expect(router.OnChar(U' ', 0).Browser, "a space is typed text");
        const auto masked = router.OnKey(65, 0, kPress, 0xFFFF);
        check.Expect(masked.Key.Modifiers == BrowserModifier::KeyMask, "GLFW modifier bits above 5 are masked off");
        router.OnKey(65, 0, kRelease, 0);
        for (const int action : { -1, 3, 100 })
        {
            const auto route = router.OnKey(65, 0, action, 0);
            check.Expect(!route.Browser && !route.ImGui && !route.EditorShortcuts, "an invalid key action is ignored");
        }

        // A click elsewhere revokes the keyboard and releases what the browser still holds.
        router.OnKey(65, 30, kPress, 0);
        router.OnKey(66, 48, kPress, 0);
        check.Expect(router.BrowserHoldsKeys(), "the router tracks keys delivered down to the browser");
        const auto outside = router.OnMouseButton(BrowserMouseButton::Left, true, 10, 10, 0, 2000);
        check.Expect(!outside.Browser && outside.EditorShortcuts && outside.KeyboardOwnerChanged && !router.OwnsKeyboard(),
            "a press outside the surface revokes the keyboard");
        check.Expect(outside.SyntheticKeyUps.size() == 2 && std::count(outside.SyntheticKeyUps.begin(), outside.SyntheticKeyUps.end(), 65) == 1
                && std::count(outside.SyntheticKeyUps.begin(), outside.SyntheticKeyUps.end(), 66) == 1 && !router.BrowserHoldsKeys(),
            "both held keys receive a synthetic release exactly once");
        const auto lateUp = router.OnKey(65, 30, kRelease, 0);
        check.Expect(!lateUp.Browser && lateUp.EditorShortcuts, "a release after the synthetic one is not delivered twice");
        const auto afterRevoke = router.OnKey(70, 0, kPress, 0);
        check.Expect(!afterRevoke.Browser && afterRevoke.EditorShortcuts, "after revocation the viewport F shortcut works again");
        router.OnMouseButton(BrowserMouseButton::Left, false, 10, 10, 0, 2010);

        // Every other revocation path, and none of them regrants on its own.
        const auto expectRegrantNeedsClick = [&](const char* name, auto revoke, auto restore)
        {
            BrowserInputRouter subject;
            GainKeyboard(subject);
            check.Expect(subject.OwnsKeyboard(), std::string(name) + ": precondition owns keyboard");
            subject.OnKey(65, 0, kPress, 0);
            const BrowserInputRoute route = revoke(subject);
            check.Expect(route.KeyboardOwnerChanged && !subject.OwnsKeyboard() && route.SyntheticKeyUps == std::vector<int> { 65 },
                std::string(name) + ": keyboard revoked with a synthetic key up");
            restore(subject);
            check.Expect(!subject.OwnsKeyboard(), std::string(name) + ": restoring the condition does not regrant without a click");
            subject.UpdatePanel(Panel(true, true));
            GainKeyboard(subject, 5000);
            check.Expect(subject.OwnsKeyboard(), std::string(name) + ": a fresh click regrants the keyboard");
        };
        expectRegrantNeedsClick("text input elsewhere",
            [](BrowserInputRouter& r) { auto s = Panel(true, true); s.OtherTextInputActive = true; return r.UpdatePanel(s); },
            [](BrowserInputRouter& r) { r.UpdatePanel(Panel(true, true)); });
        expectRegrantNeedsClick("panel hidden",
            [](BrowserInputRouter& r) { auto s = Panel(true, true); s.PanelVisible = false; return r.UpdatePanel(s); },
            [](BrowserInputRouter& r) { r.UpdatePanel(Panel(true, true)); });
        expectRegrantNeedsClick("panel loses focus",
            [](BrowserInputRouter& r) { return r.UpdatePanel(Panel(false, true)); },
            [](BrowserInputRouter& r) { r.UpdatePanel(Panel(true, true)); });
        expectRegrantNeedsClick("window loses focus",
            [](BrowserInputRouter& r) { return r.OnWindowFocus(false); },
            [](BrowserInputRouter& r) { r.OnWindowFocus(true); r.UpdatePanel(Panel(true, true)); });
        expectRegrantNeedsClick("explicit release",
            [](BrowserInputRouter& r) { return r.ReleaseKeyboard(); },
            [](BrowserInputRouter& r) { r.UpdatePanel(Panel(true, true)); });
        return check.Ok;
    }

    bool CheckRouterMouse()
    {
        Checker check { "router-mouse" };
        constexpr auto kLeft = BrowserMouseButton::Left;

        {
            // Capture: a drag that starts inside keeps going outside, then the pointer leaves.
            BrowserInputRouter router;
            router.UpdatePanel(Panel(true, true));
            check.Expect(router.OnMouseButton(kLeft, true, 200, 100, 0, 1000).Browser, "press inside begins capture");
            const auto drag = router.OnMouseMove(700, 500, 0);
            check.Expect(drag.Browser && !drag.EditorShortcuts && drag.Mouse.X == 600.0f && drag.Mouse.Y == 450.0f
                    && (drag.Mouse.Modifiers & BrowserModifier::LeftButton) != 0,
                "captured moves outside the surface still reach the browser, view-local and with the button flag");
            const auto scroll = router.OnScroll(0, -1, 700, 500, 0);
            check.Expect(scroll.Browser, "captured wheel events reach the browser too");
            const auto release = router.OnMouseButton(kLeft, false, 700, 500, 0, 1100);
            check.Expect(release.Browser && release.SendMouseLeave && !router.HasMouseCapture(),
                "releasing outside ends capture and tells the page the pointer left");
            const auto moveOut = router.OnMouseMove(700, 500, 0);
            check.Expect(!moveOut.Browser && moveOut.EditorShortcuts && !moveOut.SendMouseLeave, "afterwards moves outside belong to the editor");
            check.Expect(router.OnMouseMove(200, 100, 0).Browser, "moving back over the surface returns to the browser");
            const auto leave = router.OnMouseMove(50, 50, 0);
            check.Expect(!leave.Browser && leave.SendMouseLeave && leave.EditorShortcuts, "leaving the surface sends exactly one mouse-leave");
            check.Expect(!router.OnMouseMove(40, 40, 0).SendMouseLeave, "no second mouse-leave");
        }
        {
            // A gesture that starts elsewhere stays with the editor until every button is up.
            BrowserInputRouter router;
            router.UpdatePanel(Panel(true, true));
            const auto outsidePress = router.OnMouseButton(kLeft, true, 10, 10, 0, 1000);
            check.Expect(!outsidePress.Browser && outsidePress.EditorShortcuts, "press outside is the editor's");
            check.Expect(!router.OnMouseMove(200, 100, 0).Browser, "dragging into the surface from outside does not enter the browser");
            check.Expect(!router.OnScroll(0, -2, 200, 100, 0).Browser, "wheel during an editor gesture stays with the editor");
            check.Expect(!router.OnMouseButton(BrowserMouseButton::Right, true, 200, 100, 0, 1010).Browser,
                "a second press over the surface during an editor gesture does not start browser capture");
            router.OnMouseButton(kLeft, false, 200, 100, 0, 1020);
            check.Expect(!router.OnMouseMove(200, 100, 0).Browser, "one button still down keeps the gesture with the editor");
            router.OnMouseButton(BrowserMouseButton::Right, false, 200, 100, 0, 1030);
            const auto wheel = router.OnScroll(1.5f, -2.0f, 200, 100, 0);
            check.Expect(wheel.Browser && wheel.WheelDeltaX == 60.0f && wheel.WheelDeltaY == -80.0f && wheel.Mouse.X == 100.0f,
                "after all buttons are up the wheel reaches the browser, 40 pixels per notch");
            const auto stray = router.OnMouseButton(kLeft, false, 200, 100, 0, 1100);
            check.Expect(!stray.Browser && stray.EditorShortcuts && !router.HasMouseCapture(), "a release with no tracked press is the editor's");
        }
        {
            // Hover is the occlusion-aware ImGui latch combined with the geometry.
            BrowserInputRouter router;
            router.UpdatePanel(Panel(true, false));
            check.Expect(!router.OnMouseMove(200, 100, 0).Browser, "inside the rectangle but not hovered (occluded) is the editor's");
            check.Expect(!router.OnMouseButton(kLeft, true, 200, 100, 0, 1).Browser, "an occluded press is the editor's");
            router.OnMouseButton(kLeft, false, 200, 100, 0, 2);
            router.UpdatePanel(Panel(true, true));
            check.Expect(!router.OnMouseMove(50, 50, 0).Browser, "hovered but outside the rectangle is the editor's");
            check.Expect(!router.OnMouseMove(500, 100, 0).Browser && !router.OnMouseMove(100, 350, 0).Browser, "the right and bottom edges are exclusive");
            check.Expect(router.OnMouseMove(100, 50, 0).Browser && router.OnMouseMove(499, 349, 0).Browser, "the left/top and last interior pixel are inside");
        }
        {
            // A drag-drop payload or a modal blocks the browser and clears hover.
            BrowserInputRouter router;
            router.UpdatePanel(Panel(true, true));
            check.Expect(router.OnMouseMove(200, 100, 0).Browser, "precondition: pointer in browser");
            auto payload = Panel(true, true);
            payload.DragDropPayloadActive = true;
            const auto blocked = router.UpdatePanel(payload);
            check.Expect(blocked.SendMouseLeave, "a payload starting over the surface sends a mouse-leave");
            const auto move = router.OnMouseMove(200, 100, 0);
            check.Expect(!move.Browser && !move.SendMouseLeave, "moves during a payload are the editor's with no repeated leave");
            check.Expect(!router.OnMouseButton(kLeft, true, 200, 100, 0, 1).Browser, "a press during a payload is the editor's");
            router.OnMouseButton(kLeft, false, 200, 100, 0, 2);
            auto modal = Panel(true, true);
            modal.ModalOrPopupOpen = true;
            router.UpdatePanel(modal);
            check.Expect(!router.OnMouseMove(200, 100, 0).Browser, "a modal blocks the browser");
        }
        {
            // A modal opening during capture cancels it.
            BrowserInputRouter router;
            router.UpdatePanel(Panel(true, true));
            router.OnMouseButton(kLeft, true, 200, 100, 0, 1000);
            auto modal = Panel(true, true);
            modal.ModalOrPopupOpen = true;
            const auto cancelled = router.UpdatePanel(modal);
            check.Expect(cancelled.SendCaptureLost && cancelled.SendMouseLeave && !router.HasMouseCapture(), "capture is cancelled with capture-lost and mouse-leave");
            const auto release = router.OnMouseButton(kLeft, false, 200, 100, 0, 1010);
            check.Expect(!release.Browser && release.EditorShortcuts, "the release of a cancelled capture is not delivered to the browser");
        }
        {
            // Window focus loss during capture.
            BrowserInputRouter router;
            router.UpdatePanel(Panel(true, true));
            router.OnMouseButton(kLeft, true, 200, 100, 0, 1000);
            const auto lost = router.OnWindowFocus(false);
            check.Expect(lost.SendCaptureLost && lost.SendMouseLeave && !router.HasMouseCapture(), "focus loss cancels capture");
            check.Expect(!router.OnMouseButton(kLeft, false, 200, 100, 0, 1010).Browser, "the late release goes to the editor");
            router.OnWindowFocus(true);
            check.Expect(router.OnMouseButton(kLeft, true, 200, 100, 0, 1500).Browser, "capture works again after focus returns");
        }
        {
            // Click counting.
            BrowserInputRouter router;
            router.UpdatePanel(Panel(true, true));
            const auto click = [&](BrowserMouseButton button, float x, u64 time)
            {
                const auto down = router.OnMouseButton(button, true, x, 100, 0, time);
                router.OnMouseButton(button, false, x, 100, 0, time + 20);
                return down.ClickCount;
            };
            check.Expect(click(kLeft, 200, 1000) == 1 && click(kLeft, 200, 1300) == 2 && click(kLeft, 201, 1500) == 3
                    && click(kLeft, 200, 1700) == 3,
                "rapid clicks count up to three and stay there");
            check.Expect(click(kLeft, 200, 2400) == 1, "a click more than 500 ms after the previous one restarts the count");
            check.Expect(click(BrowserMouseButton::Right, 200, 2500) == 1, "a different button restarts the count");
            check.Expect(click(BrowserMouseButton::Right, 200, 2600) == 2, "the same button continues");
            check.Expect(click(BrowserMouseButton::Right, 210, 2700) == 1, "a click more than 4 pixels away restarts the count");
            check.Expect(click(BrowserMouseButton::Right, 214, 2800) == 2, "a click within 4 pixels continues");
        }
        {
            // Chords: extra buttons during capture are the browser's, even outside the rectangle.
            BrowserInputRouter router;
            router.UpdatePanel(Panel(true, true));
            router.OnMouseButton(kLeft, true, 200, 100, 0, 1);
            const auto right = router.OnMouseButton(BrowserMouseButton::Right, true, 900, 900, 0, 2);
            check.Expect(right.Browser && (right.Mouse.Modifiers & (BrowserModifier::LeftButton | BrowserModifier::RightButton))
                    == (BrowserModifier::LeftButton | BrowserModifier::RightButton),
                "a second button during capture reaches the browser with both button flags");
            check.Expect(router.OnMouseButton(kLeft, false, 900, 900, 0, 3).Browser && router.HasMouseCapture(),
                "capture lasts until the last button is released");
            check.Expect(router.OnMouseButton(BrowserMouseButton::Right, false, 900, 900, 0, 4).Browser && !router.HasMouseCapture(),
                "the last release ends capture");
        }
        {
            // Pointer leaving the OS window.
            BrowserInputRouter router;
            router.UpdatePanel(Panel(true, true));
            router.OnMouseMove(200, 100, 0);
            check.Expect(router.OnCursorLeftWindow().SendMouseLeave && !router.OnCursorLeftWindow().SendMouseLeave,
                "leaving the window sends one mouse-leave");
            router.OnMouseButton(kLeft, true, 200, 100, 0, 1);
            check.Expect(!router.OnCursorLeftWindow().SendMouseLeave, "leaving the window during capture keeps capture");
        }
        {
            // A hidden panel never receives mouse input.
            BrowserInputRouter router;
            auto hidden = Panel(true, true);
            hidden.PanelVisible = false;
            router.UpdatePanel(hidden);
            check.Expect(!router.OnMouseButton(kLeft, true, 200, 100, 0, 1).Browser && !router.OnScroll(0, 1, 200, 100, 0).Browser,
                "a hidden panel receives no mouse input");
        }
        return check.Ok;
    }
}

namespace
{
    // ---- router model -------------------------------------------------------------
    //
    // A deliberately different formulation of the router rules: per-button
    // origin tags and explicit sets instead of the router's capture/gesture
    // flags, so a bookkeeping slip in either shows up as a divergence.

    struct ModelRoute
    {
        bool Browser = false;
        bool ImGui = false;
        bool Editor = false;
        bool Leave = false;
        bool CaptureLost = false;
        bool OwnerChanged = false;
        std::vector<int> KeyUps;
    };

    class RouterModel
    {
    public:
        ModelRoute UpdatePanel(const BrowserPanelState& state)
        {
            ModelRoute out;
            const bool lostFocus = m_Panel.PanelFocused && !state.PanelFocused;
            m_Panel = state;
            if (lostFocus || !state.PanelVisible || state.OtherTextInputActive)
                m_Latch = false;
            if (HasOrigin('B') && !CanRoute())
                CancelCapture(out);
            if (m_PointerIn && !HasOrigin('B') && (!CanRoute() || !state.SurfaceHovered))
            {
                out.Leave = true;
                m_PointerIn = false;
            }
            Settle(out);
            return out;
        }

        ModelRoute WindowFocus(bool focused)
        {
            ModelRoute out;
            m_WindowFocused = focused;
            if (!focused)
            {
                if (HasOrigin('B'))
                    CancelCapture(out);
                if (m_PointerIn)
                {
                    out.Leave = true;
                    m_PointerIn = false;
                }
                m_Pressed.clear();
                m_Latch = false;
            }
            Settle(out);
            return out;
        }

        ModelRoute CursorLeft()
        {
            ModelRoute out;
            if (m_PointerIn && !HasOrigin('B'))
            {
                out.Leave = true;
                m_PointerIn = false;
            }
            return out;
        }

        ModelRoute ReleaseKeyboard()
        {
            ModelRoute out;
            m_Latch = false;
            Settle(out);
            return out;
        }

        ModelRoute Move(float x, float y)
        {
            ModelRoute out;
            out.ImGui = true;
            if (HasOrigin('B') || (!HasOrigin('E') && Over(x, y)))
            {
                out.Browser = true;
                m_PointerIn = true;
                return out;
            }
            out.Editor = true;
            if (m_PointerIn)
            {
                out.Leave = true;
                m_PointerIn = false;
            }
            return out;
        }

        ModelRoute Scroll(float x, float y)
        {
            ModelRoute out;
            out.ImGui = true;
            if (HasOrigin('B') || (!HasOrigin('E') && Over(x, y)))
                out.Browser = true;
            else
                out.Editor = true;
            return out;
        }

        ModelRoute Button(int button, bool down, float x, float y)
        {
            ModelRoute out;
            out.ImGui = true;
            if (down)
            {
                if (HasOrigin('B'))
                {
                    m_Pressed[button] = 'B';
                    m_PointerIn = true;
                    out.Browser = true;
                }
                else if (m_Pressed.empty() && Over(x, y))
                {
                    m_Pressed[button] = 'B';
                    m_Latch = true;
                    m_PointerIn = true;
                    out.Browser = true;
                }
                else
                {
                    m_Pressed[button] = 'E';
                    m_Latch = false;
                    out.Editor = true;
                }
                Settle(out);
                return out;
            }
            const auto found = m_Pressed.find(button);
            if (found == m_Pressed.end())
            {
                out.Editor = true;
                return out;
            }
            const char origin = found->second;
            m_Pressed.erase(found);
            if (origin == 'B')
            {
                out.Browser = true;
                if (m_Pressed.empty() && m_PointerIn && !Over(x, y))
                {
                    out.Leave = true;
                    m_PointerIn = false;
                }
            }
            else
            {
                out.Editor = true;
            }
            return out;
        }

        ModelRoute Key(int key, int action)
        {
            ModelRoute out;
            if (action < 0 || action > 2)
                return out;
            if (action == 0)
            {
                if (m_Held.erase(key) != 0)
                {
                    out.Browser = true;
                    return out;
                }
                out.ImGui = out.Editor = true;
                return out;
            }
            if (m_Owns)
            {
                m_Held.insert(key);
                out.Browser = true;
                return out;
            }
            out.ImGui = out.Editor = true;
            return out;
        }

        ModelRoute Char(char32_t codepoint)
        {
            ModelRoute out;
            const bool valid = codepoint >= 0x20 && codepoint <= 0x10FFFF && !(codepoint >= 0x7F && codepoint <= 0x9F)
                && !(codepoint >= 0xD800 && codepoint <= 0xDFFF);
            if (!valid)
                return out;
            if (m_Owns)
                out.Browser = true;
            else
                out.ImGui = out.Editor = true;
            return out;
        }

        bool Owns() const { return m_Owns; }
        bool Capture() const { return HasOrigin('B'); }
        bool HoldsKeys() const { return !m_Held.empty(); }

        u32 ButtonMask() const
        {
            u32 mask = 0;
            for (const auto& [button, origin] : m_Pressed)
            {
                (void)origin;
                mask |= button == 0 ? BrowserModifier::LeftButton : (button == 1 ? BrowserModifier::MiddleButton : BrowserModifier::RightButton);
            }
            return mask;
        }

    private:
        bool CanRoute() const { return m_Panel.PanelVisible && !m_Panel.DragDropPayloadActive && !m_Panel.ModalOrPopupOpen; }

        bool Over(float x, float y) const
        {
            return CanRoute() && m_Panel.SurfaceHovered && x >= m_Panel.Surface.X && x < m_Panel.Surface.X + m_Panel.Surface.Width
                && y >= m_Panel.Surface.Y && y < m_Panel.Surface.Y + m_Panel.Surface.Height;
        }

        bool HasOrigin(char origin) const
        {
            return std::any_of(m_Pressed.begin(), m_Pressed.end(), [&](const auto& item) { return item.second == origin; });
        }

        void CancelCapture(ModelRoute& out)
        {
            out.CaptureLost = true;
            for (auto& item : m_Pressed)
                item.second = 'E';
            if (m_PointerIn)
            {
                out.Leave = true;
                m_PointerIn = false;
            }
        }

        void Settle(ModelRoute& out)
        {
            const bool owns = m_Latch && m_WindowFocused && m_Panel.PanelFocused && m_Panel.PanelVisible && !m_Panel.OtherTextInputActive;
            if (owns == m_Owns)
                return;
            m_Owns = owns;
            out.OwnerChanged = true;
            if (!owns)
            {
                out.KeyUps.assign(m_Held.begin(), m_Held.end());
                m_Held.clear();
            }
        }

        BrowserPanelState m_Panel;
        bool m_WindowFocused = true;
        bool m_Latch = false;
        bool m_Owns = false;
        bool m_PointerIn = false;
        std::map<int, char> m_Pressed;
        std::set<int> m_Held;
    };

    bool RouterModelProperty(Spiral::Tests::ChoiceStream& stream, std::string& message)
    {
        BrowserInputRouter router;
        RouterModel model;
        std::set<int> deliveredButtons;
        std::set<int> deliveredKeys;
        u64 time = 1000;
        const size_t operations = stream.NextSize(1, 120);
        std::string last;

        const auto fail = [&](const std::string& what)
        {
            message = what + " after operation '" + last + "'";
            return false;
        };
        const auto compare = [&](const BrowserInputRoute& actual, ModelRoute expected, const char* kind) -> bool
        {
            std::vector<int> ups = actual.SyntheticKeyUps;
            std::sort(ups.begin(), ups.end());
            std::sort(expected.KeyUps.begin(), expected.KeyUps.end());
            if (actual.Browser != expected.Browser || actual.ImGui != expected.ImGui || actual.EditorShortcuts != expected.Editor
                || actual.SendMouseLeave != expected.Leave || actual.SendCaptureLost != expected.CaptureLost
                || actual.KeyboardOwnerChanged != expected.OwnerChanged || ups != expected.KeyUps)
            {
                message = std::string(kind) + " route diverged from the model after operation '" + last + "': got browser=" + std::to_string(actual.Browser)
                    + " imgui=" + std::to_string(actual.ImGui) + " editor=" + std::to_string(actual.EditorShortcuts)
                    + " leave=" + std::to_string(actual.SendMouseLeave) + " lost=" + std::to_string(actual.SendCaptureLost)
                    + " owner=" + std::to_string(actual.KeyboardOwnerChanged) + " ups=" + std::to_string(ups.size())
                    + ", expected browser=" + std::to_string(expected.Browser) + " imgui=" + std::to_string(expected.ImGui)
                    + " editor=" + std::to_string(expected.Editor) + " leave=" + std::to_string(expected.Leave)
                    + " lost=" + std::to_string(expected.CaptureLost) + " owner=" + std::to_string(expected.OwnerChanged)
                    + " ups=" + std::to_string(expected.KeyUps.size());
                return false;
            }
            if (router.OwnsKeyboard() != model.Owns() || router.HasMouseCapture() != model.Capture()
                || router.BrowserHoldsKeys() != model.HoldsKeys())
            {
                message = std::string(kind) + " state diverged from the model after operation '" + last + "'";
                return false;
            }
            // Host-visible ledger: what the browser was told must always be balanced.
            if (actual.SendCaptureLost)
                deliveredButtons.clear();
            for (const int key : actual.SyntheticKeyUps)
            {
                if (deliveredKeys.erase(key) != 1)
                {
                    message = "synthetic key up for a key the browser never saw down after '" + last + "'";
                    return false;
                }
            }
            if (actual.Browser && std::string_view(kind) == "key")
            {
                if (actual.Key.Kind == BrowserKey::Phase::Down)
                    deliveredKeys.insert(actual.Key.GlfwKey);
                else if (actual.Key.Kind == BrowserKey::Phase::Up && deliveredKeys.erase(actual.Key.GlfwKey) != 1)
                {
                    message = "key up delivered to the browser without a prior down after '" + last + "'";
                    return false;
                }
                if (actual.ImGui || actual.EditorShortcuts)
                {
                    message = "a browser key was also given to ImGui or the editor after '" + last + "'";
                    return false;
                }
            }
            if (actual.Browser && std::string_view(kind) == "char" && (actual.ImGui || actual.EditorShortcuts))
            {
                message = "browser text was also given to ImGui or the editor after '" + last + "'";
                return false;
            }
            if (actual.Browser && std::string_view(kind).starts_with("mouse") && actual.EditorShortcuts)
            {
                message = "a browser mouse event was also given to editor shortcuts after '" + last + "'";
                return false;
            }
            return true;
        };

        for (size_t index = 0; index < operations; ++index)
        {
            time += static_cast<u64>(stream.NextI64(0, 700, { 0, 100, 500, 501 }));
            const float x = static_cast<float>(stream.NextI64(40, 560, { 100, 99, 499, 500, 300 }));
            const float y = static_cast<float>(stream.NextI64(20, 400, { 50, 49, 349, 350, 200 }));
            const u32 mods = static_cast<u32>(stream.Next() & 0xFF);
            switch (stream.Next() % 26)
            {
            case 0: case 1: case 2:
            {
                BrowserPanelState state;
                state.Surface = { 100, 50, 400, 300 };
                state.PanelVisible = stream.Next() % 100 < 88;
                state.PanelFocused = stream.Next() % 100 < 70;
                state.SurfaceHovered = stream.Next() % 100 < 70;
                state.OtherTextInputActive = stream.Next() % 100 < 8;
                state.DragDropPayloadActive = stream.Next() % 100 < 8;
                state.ModalOrPopupOpen = stream.Next() % 100 < 5;
                last = "UpdatePanel";
                const auto route = router.UpdatePanel(state);
                if (!compare(route, model.UpdatePanel(state), "panel") || route.Browser || route.ImGui || route.EditorShortcuts)
                    return message.empty() ? fail("panel update carried an event route") : false;
                break;
            }
            case 3:
            {
                const bool focused = stream.Next() % 100 < 80;
                last = focused ? "WindowFocus(true)" : "WindowFocus(false)";
                if (!compare(router.OnWindowFocus(focused), model.WindowFocus(focused), "focus"))
                    return false;
                break;
            }
            case 4: case 5: case 6: case 7: case 8: case 9:
            {
                last = "Move";
                const auto route = router.OnMouseMove(x, y, mods);
                const ModelRoute expected = model.Move(x, y);
                if (!compare(route, expected, "mouse-move"))
                    return false;
                if (route.Browser && (route.Mouse.X != x - 100.0f || route.Mouse.Y != y - 50.0f
                        || route.Mouse.Modifiers != ((mods & BrowserModifier::KeyMask) | model.ButtonMask())))
                    return fail("browser move carried wrong coordinates or modifiers");
                break;
            }
            case 10: case 11: case 12: case 13: case 14:
            {
                const int button = static_cast<int>(stream.Next() % 3);
                const BrowserMouseButton mapped = button == 0 ? BrowserMouseButton::Left
                    : (button == 1 ? BrowserMouseButton::Middle : BrowserMouseButton::Right);
                const bool pressedNow = (model.ButtonMask() & (button == 0 ? BrowserModifier::LeftButton
                                                              : (button == 1 ? BrowserModifier::MiddleButton : BrowserModifier::RightButton))) != 0;
                const bool stray = stream.Next() % 100 < 4;
                const bool down = !pressedNow && !stray;
                last = down ? "ButtonDown" : "ButtonUp";
                const auto route = router.OnMouseButton(mapped, down, x, y, mods, time);
                const ModelRoute expected = model.Button(button, down, x, y);
                if (!compare(route, expected, "mouse-button"))
                    return false;
                if (route.Browser)
                {
                    if (down)
                    {
                        if (!deliveredButtons.insert(button).second)
                            return fail("browser button down delivered twice");
                        if (route.ClickCount < 1 || route.ClickCount > 3)
                            return fail("click count outside 1..3");
                    }
                    else if (deliveredButtons.erase(button) != 1)
                        return fail("browser button up without a prior down");
                    if (route.Mouse.Modifiers != ((mods & BrowserModifier::KeyMask) | model.ButtonMask()))
                        return fail("browser button event carried wrong modifiers");
                }
                break;
            }
            case 15: case 16:
            {
                last = "Scroll";
                const float dx = static_cast<float>(stream.NextI64(-3, 3));
                const float dy = static_cast<float>(stream.NextI64(-3, 3));
                const auto route = router.OnScroll(dx, dy, x, y, mods);
                if (!compare(route, model.Scroll(x, y), "mouse-scroll"))
                    return false;
                if (route.Browser && (route.WheelDeltaX != dx * 40.0f || route.WheelDeltaY != dy * 40.0f))
                    return fail("wheel delta is not 40 pixels per notch");
                break;
            }
            case 17: case 18: case 19: case 20: case 21:
            {
                static constexpr int keys[] = { 65, 70, 90, 256, 340 };
                const int key = keys[stream.Next() % 5];
                const int action = static_cast<int>(stream.NextI64(-1, 3, { 0, 1, 2 }));
                last = "Key " + std::to_string(key) + " action " + std::to_string(action);
                if (!compare(router.OnKey(key, 7, action, mods), model.Key(key, action), "key"))
                    return false;
                break;
            }
            case 22: case 23:
            {
                static constexpr char32_t chars[] = { U'a', U' ', U'\x1F', U'\x7F', char32_t(0xD800), U'\U0001F600', char32_t(0x110000), U'\0' };
                const char32_t codepoint = chars[stream.Next() % 8];
                last = "Char " + std::to_string(static_cast<u32>(codepoint));
                if (!compare(router.OnChar(codepoint, mods), model.Char(codepoint), "char"))
                    return false;
                break;
            }
            case 24:
                last = "ReleaseKeyboard";
                if (!compare(router.ReleaseKeyboard(), model.ReleaseKeyboard(), "release"))
                    return false;
                break;
            default:
                last = "CursorLeft";
                if (!compare(router.OnCursorLeftWindow(), model.CursorLeft(), "cursor"))
                    return false;
                break;
            }
        }

        // Settle: losing window focus must leave the browser with no held button or key.
        last = "final WindowFocus(false)";
        if (!compare(router.OnWindowFocus(false), model.WindowFocus(false), "focus"))
            return false;
        if (!deliveredButtons.empty() || !deliveredKeys.empty())
            return fail("the browser was left with a held button or key");
        return true;
    }
}

namespace
{
    // ---- intake classification ------------------------------------------------------

    // Independent bitwise CRC-32 (shares no code with the importer's ZIP authority).
    u32 Crc32(const Bytes& bytes)
    {
        u32 crc = 0xFFFFFFFFu;
        for (const u8 byte : bytes)
        {
            crc ^= byte;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }

    void Put16(Bytes& out, u32 value)
    {
        out.push_back(static_cast<u8>(value));
        out.push_back(static_cast<u8>(value >> 8));
    }

    void Put32(Bytes& out, u32 value)
    {
        Put16(out, value & 0xFFFFu);
        Put16(out, value >> 16);
    }

    // One stored member; no dependency on miniz.
    Bytes MakeZip(std::string_view memberName, const Bytes& data, size_t commentBytes = 0)
    {
        Bytes zip;
        const u32 crc = Crc32(data);
        Put32(zip, 0x04034b50u);
        Put16(zip, 20); Put16(zip, 0); Put16(zip, 0); Put16(zip, 0); Put16(zip, 0x21);
        Put32(zip, crc); Put32(zip, static_cast<u32>(data.size())); Put32(zip, static_cast<u32>(data.size()));
        Put16(zip, static_cast<u32>(memberName.size())); Put16(zip, 0);
        zip.insert(zip.end(), memberName.begin(), memberName.end());
        zip.insert(zip.end(), data.begin(), data.end());
        const size_t centralOffset = zip.size();
        Put32(zip, 0x02014b50u);
        Put16(zip, 20); Put16(zip, 20); Put16(zip, 0); Put16(zip, 0); Put16(zip, 0); Put16(zip, 0x21);
        Put32(zip, crc); Put32(zip, static_cast<u32>(data.size())); Put32(zip, static_cast<u32>(data.size()));
        Put16(zip, static_cast<u32>(memberName.size())); Put16(zip, 0); Put16(zip, 0); Put16(zip, 0); Put16(zip, 0);
        Put32(zip, 0); Put32(zip, 0);
        zip.insert(zip.end(), memberName.begin(), memberName.end());
        const size_t centralSize = zip.size() - centralOffset;
        Put32(zip, 0x06054b50u);
        Put16(zip, 0); Put16(zip, 0); Put16(zip, 1); Put16(zip, 1);
        Put32(zip, static_cast<u32>(centralSize)); Put32(zip, static_cast<u32>(centralOffset));
        Put16(zip, static_cast<u32>(commentBytes));
        zip.insert(zip.end(), commentBytes, 'c');
        return zip;
    }

    Bytes MakeGlb(std::string json, u32 version = 2, bool lieAboutLength = false, std::string_view chunkType = "JSON")
    {
        while (json.size() % 4 != 0)
            json.push_back(' ');
        Bytes glb;
        glb.insert(glb.end(), { 'g', 'l', 'T', 'F' });
        Put32(glb, version);
        Put32(glb, static_cast<u32>(20 + json.size()) + (lieAboutLength ? 4u : 0u));
        Put32(glb, static_cast<u32>(json.size()));
        glb.insert(glb.end(), chunkType.begin(), chunkType.end());
        glb.insert(glb.end(), json.begin(), json.end());
        return glb;
    }

    const std::string kMinimalJson = R"({"asset":{"version":"2.0"},"scenes":[]})";

    bool CheckIntakeClassification()
    {
        Checker check { "intake" };
        TempDir fixture("intake");
        const std::filesystem::path root = fixture.Path();
        const auto classify = [&](const std::filesystem::path& path, const FabIntakeLimits& limits = {}) { return ClassifyFabIntakePath(path, limits); };
        const auto expectKind = [&](const std::filesystem::path& path, FabIntakeKind kind, FabIntakeReason reason, const char* what,
                                    const FabIntakeLimits& limits = {})
        {
            const FabIntakeClassification result = classify(path, limits);
            check.Expect(result.Kind == kind && result.Reason == reason,
                std::string(what) + ": kind " + std::to_string(static_cast<int>(result.Kind)) + " reason " + std::to_string(static_cast<int>(result.Reason)));
            return result;
        };

        const Bytes zip = MakeZip("a.txt", ToBytes("hi"));
        WriteFile(root / "pkg.zip", zip);
        WriteFile(root / "UPPER.ZIP", zip);
        WriteFile(root / "pkg.bin", zip);
        WriteFile(root / "noextension", zip);
        WriteFile(root / "pkg.glb", zip);
        WriteFile(root / "pkg.gltf", zip);
        const FabIntakeClassification zipResult = expectKind(root / "pkg.zip", FabIntakeKind::Zip, FabIntakeReason::None, "zip by content");
        check.Expect(zipResult.SizeBytes == zip.size(), "classification reports the file size");
        expectKind(root / "UPPER.ZIP", FabIntakeKind::Zip, FabIntakeReason::None, "upper-case extension");
        expectKind(root / "pkg.bin", FabIntakeKind::Zip, FabIntakeReason::None, "unknown extension, zip content");
        expectKind(root / "noextension", FabIntakeKind::Zip, FabIntakeReason::None, "no extension, zip content");
        expectKind(root / "pkg.glb", FabIntakeKind::Unsupported, FabIntakeReason::ExtensionContentMismatch, "zip renamed .glb");
        expectKind(root / "pkg.gltf", FabIntakeKind::Unsupported, FabIntakeReason::ExtensionContentMismatch, "zip renamed .gltf");

        WriteFile(root / "commented.zip", MakeZip("a.txt", ToBytes("hi"), 17));
        expectKind(root / "commented.zip", FabIntakeKind::Zip, FabIntakeReason::None, "zip with an end-record comment");
        Bytes truncated(zip.begin(), zip.begin() + 40);
        WriteFile(root / "truncated.zip", truncated);
        expectKind(root / "truncated.zip", FabIntakeKind::Unsupported, FabIntakeReason::IncompleteZip, "truncated zip");
        Bytes trailing = zip;
        trailing.insert(trailing.end(), 100, 'x');
        WriteFile(root / "trailing.zip", trailing);
        expectKind(root / "trailing.zip", FabIntakeKind::Unsupported, FabIntakeReason::IncompleteZip, "zip with trailing bytes after the end record");
        Bytes emptyArchive;
        Put32(emptyArchive, 0x06054b50u);
        emptyArchive.insert(emptyArchive.end(), 18, 0);
        WriteFile(root / "empty-archive.zip", emptyArchive);
        expectKind(root / "empty-archive.zip", FabIntakeKind::Zip, FabIntakeReason::None, "an empty archive is still structurally a zip");
        WriteFile(root / "empty.zip", {});
        expectKind(root / "empty.zip", FabIntakeKind::Unsupported, FabIntakeReason::Empty, "empty file");
        WriteFile(root / "one-byte.zip", { 'P' });
        expectKind(root / "one-byte.zip", FabIntakeKind::Unsupported, FabIntakeReason::UnrecognizedContent, "one byte");
        WriteFile(root / "text.zip", ToBytes("this is not a zip file at all, only text........"));
        expectKind(root / "text.zip", FabIntakeKind::Unsupported, FabIntakeReason::UnrecognizedContent, "text named .zip");
        FabIntakeLimits tiny;
        tiny.MaximumFileBytes = zip.size() - 1;
        const auto tooLarge = expectKind(root / "pkg.zip", FabIntakeKind::Unsupported, FabIntakeReason::TooLarge, "over the file limit", tiny);
        check.Expect(tooLarge.SizeBytes == zip.size(), "TooLarge reports the real size");
        tiny.MaximumFileBytes = zip.size();
        expectKind(root / "pkg.zip", FabIntakeKind::Zip, FabIntakeReason::None, "exactly the file limit", tiny);

        // GLB structure.
        WriteFile(root / "model.glb", MakeGlb(kMinimalJson));
        WriteFile(root / "model.bin", MakeGlb(kMinimalJson));
        WriteFile(root / "model-as.gltf", MakeGlb(kMinimalJson));
        WriteFile(root / "v1.glb", MakeGlb(kMinimalJson, 1));
        WriteFile(root / "lies.glb", MakeGlb(kMinimalJson, 2, true));
        WriteFile(root / "bin-chunk.glb", MakeGlb(kMinimalJson, 2, false, std::string_view("BIN\0", 4)));
        WriteFile(root / "not-object.glb", MakeGlb("[1,2,3]"));
        WriteFile(root / "spaces.glb", MakeGlb("   \n  " + kMinimalJson));
        Bytes shortGlb = MakeGlb(kMinimalJson);
        shortGlb.resize(15);
        WriteFile(root / "short.glb", shortGlb);
        expectKind(root / "model.glb", FabIntakeKind::Glb, FabIntakeReason::None, "glb");
        expectKind(root / "model.bin", FabIntakeKind::Glb, FabIntakeReason::None, "glb with unknown extension");
        expectKind(root / "model-as.gltf", FabIntakeKind::Unsupported, FabIntakeReason::ExtensionContentMismatch, "glb renamed .gltf");
        expectKind(root / "v1.glb", FabIntakeKind::Unsupported, FabIntakeReason::UnrecognizedContent, "glb version 1");
        expectKind(root / "lies.glb", FabIntakeKind::Unsupported, FabIntakeReason::UnrecognizedContent, "glb length field disagrees with the file size");
        expectKind(root / "bin-chunk.glb", FabIntakeKind::Unsupported, FabIntakeReason::UnrecognizedContent, "glb whose first chunk is not JSON");
        expectKind(root / "not-object.glb", FabIntakeKind::Unsupported, FabIntakeReason::UnrecognizedContent, "glb JSON that is not an object");
        expectKind(root / "spaces.glb", FabIntakeKind::Glb, FabIntakeReason::None, "glb JSON with leading whitespace");
        expectKind(root / "short.glb", FabIntakeKind::Unsupported, FabIntakeReason::UnrecognizedContent, "glb shorter than its headers");

        // glTF JSON.
        WriteFile(root / "scene.gltf", ToBytes(kMinimalJson));
        WriteFile(root / "padded.gltf", ToBytes("\n  " + kMinimalJson + "\r\n\t "));
        WriteFile(root / "noasset.gltf", ToBytes(R"({"scenes":[]})"));
        WriteFile(root / "trailing.gltf", ToBytes(kMinimalJson + "x"));
        WriteFile(root / "array.gltf", ToBytes(R"(["asset"])"));
        WriteFile(root / "bom.gltf", ToBytes("\xEF\xBB\xBF" + kMinimalJson));
        WriteFile(root / "scene-as.zip", ToBytes(kMinimalJson));
        expectKind(root / "scene.gltf", FabIntakeKind::Gltf, FabIntakeReason::None, "gltf");
        expectKind(root / "padded.gltf", FabIntakeKind::Gltf, FabIntakeReason::None, "gltf with surrounding whitespace");
        expectKind(root / "noasset.gltf", FabIntakeKind::Unsupported, FabIntakeReason::UnrecognizedContent, "JSON without an asset block");
        expectKind(root / "trailing.gltf", FabIntakeKind::Unsupported, FabIntakeReason::UnrecognizedContent, "JSON with trailing garbage");
        expectKind(root / "array.gltf", FabIntakeKind::Unsupported, FabIntakeReason::UnrecognizedContent, "JSON array");
        expectKind(root / "bom.gltf", FabIntakeKind::Unsupported, FabIntakeReason::UnrecognizedContent, "UTF-8 byte-order mark is not glTF");
        expectKind(root / "scene-as.zip", FabIntakeKind::Unsupported, FabIntakeReason::ExtensionContentMismatch, "gltf renamed .zip");
        FabIntakeLimits smallJson;
        smallJson.MaximumGltfJsonBytes = kMinimalJson.size() - 1;
        expectKind(root / "scene.gltf", FabIntakeKind::Unsupported, FabIntakeReason::UnrecognizedContent, "gltf JSON over the JSON limit", smallJson);

        // The mandatory asset key may straddle the streaming chunk boundary at any offset.
        for (int shift = -9; shift <= 2; ++shift)
        {
            const size_t keyStart = 64 * 1024 + static_cast<size_t>(static_cast<long long>(shift));
            std::string json = "{" + std::string(keyStart - 1, ' ') + R"("asset":{"version":"2.0"}})";
            WriteFile(root / "boundary.gltf", ToBytes(json));
            expectKind(root / "boundary.gltf", FabIntakeKind::Gltf, FabIntakeReason::None,
                ("asset key at offset " + std::to_string(keyStart)).c_str());
        }

        // Non-following and non-regular objects.
        expectKind(root / "missing.zip", FabIntakeKind::Unsupported, FabIntakeReason::Missing, "missing path");
        std::error_code linkError;
        std::filesystem::create_symlink(root / "pkg.zip", root / "link.zip", linkError);
        if (!linkError)
        {
            expectKind(root / "link.zip", FabIntakeKind::Unsupported, FabIntakeReason::NotRegularObject, "symlink to a valid zip");
            std::filesystem::create_symlink("missing-target", root / "dangling.zip", linkError);
            expectKind(root / "dangling.zip", FabIntakeKind::Unsupported, FabIntakeReason::NotRegularObject, "dangling symlink");
        }
        else
        {
            std::cerr << "Fab browser core test note [intake]: symlink creation unavailable, no-follow cases skipped\n";
        }
#if defined(__linux__)
        if (::mkfifo((root / "pipe.zip").c_str(), 0600) == 0)
            expectKind(root / "pipe.zip", FabIntakeKind::Unsupported, FabIntakeReason::NotRegularObject, "FIFO is refused without blocking");
#endif

        // Folders.
        std::filesystem::create_directories(root / "pkg-folder" / "scene");
        WriteFile(root / "pkg-folder" / "scene" / "model.glb", MakeGlb(kMinimalJson));
        WriteFile(root / "pkg-folder" / "readme.txt", ToBytes("hello"));
        expectKind(root / "pkg-folder", FabIntakeKind::Folder, FabIntakeReason::None, "folder with a glb root");
        std::filesystem::create_directories(root / "text-folder");
        WriteFile(root / "text-folder" / "readme.txt", ToBytes("hello"));
        WriteFile(root / "text-folder" / "fake.glb", ToBytes("not a glb"));
        WriteFile(root / "text-folder" / "fake.gltf", ToBytes("{}"));
        expectKind(root / "text-folder", FabIntakeKind::Unsupported, FabIntakeReason::FolderWithoutGltf, "folder whose .glb/.gltf files are not glTF");
        std::filesystem::create_directories(root / "empty-folder");
        expectKind(root / "empty-folder", FabIntakeKind::Unsupported, FabIntakeReason::FolderWithoutGltf, "empty folder");
        if (!linkError)
        {
            std::filesystem::create_directories(root / "link-folder");
            std::filesystem::create_symlink(root / "model.glb", root / "link-folder" / "linked.glb", linkError);
            std::filesystem::create_directory_symlink(root / "pkg-folder", root / "link-folder" / "linkdir", linkError);
            expectKind(root / "link-folder", FabIntakeKind::Unsupported, FabIntakeReason::FolderWithoutGltf,
                "folder holding only a symlinked glb and a symlinked directory");
            std::filesystem::create_directory_symlink(root / "pkg-folder", root / "folder-link", linkError);
            expectKind(root / "folder-link", FabIntakeKind::Unsupported, FabIntakeReason::NotRegularObject, "symlink to a folder");
        }
        std::filesystem::create_directories(root / "busy-folder");
        for (int index = 0; index < 10; ++index)
            WriteFile(root / "busy-folder" / ("junk" + std::to_string(index) + ".txt"), ToBytes("x"));
        FabIntakeLimits fewEntries;
        fewEntries.MaximumFolderEntries = 3;
        expectKind(root / "busy-folder", FabIntakeKind::Unsupported, FabIntakeReason::FolderTooLarge, "folder over the entry limit", fewEntries);
        expectKind(root / "busy-folder", FabIntakeKind::Unsupported, FabIntakeReason::FolderWithoutGltf, "same folder under the default limit");
        std::filesystem::create_directories(root / "shallow" / "a" / "b");
        WriteFile(root / "shallow" / "a" / "b" / "deep.glb", MakeGlb(kMinimalJson));
        std::filesystem::create_directories(root / "near" / "a");
        WriteFile(root / "near" / "a" / "near.glb", MakeGlb(kMinimalJson));
        FabIntakeLimits shallowLimit;
        shallowLimit.MaximumFolderDepth = 2;
        expectKind(root / "shallow", FabIntakeKind::Unsupported, FabIntakeReason::FolderWithoutGltf, "glb beyond the depth limit", shallowLimit);
        expectKind(root / "near", FabIntakeKind::Folder, FabIntakeReason::None, "glb within the depth limit", shallowLimit);
        expectKind(root / "shallow", FabIntakeKind::Folder, FabIntakeReason::None, "the same deep glb under the default depth limit");

        // Classification never mutates its input.
        check.Expect(ReadFile(root / "pkg.zip") == zip && ReadFile(root / "model.glb") == MakeGlb(kMinimalJson),
            "classification left the inspected files unchanged");

        // Planning.
        const std::vector<std::string> submitted = {
            (root / "pkg.zip").string(), (root / "." / "pkg.zip").string(), (root / "model.glb").string(),
            (root / "readme-missing.txt").string(), (root / "pkg-folder").string(), (root / "scene.gltf").string() };
        const FabIntakePlan plan = PlanFabIntake(FabIntakeOrigin::Typed, submitted);
        check.Expect(plan.Accepted.size() == 4, "duplicates collapse and four distinct supported paths remain");
        if (plan.Accepted.size() == 4)
        {
            check.Expect(plan.Accepted[0].Kind == FabIntakeKind::Zip && plan.Accepted[1].Kind == FabIntakeKind::Glb
                    && plan.Accepted[2].Kind == FabIntakeKind::Folder && plan.Accepted[3].Kind == FabIntakeKind::Gltf,
                "accepted requests keep submission order and kinds");
            check.Expect(std::all_of(plan.Accepted.begin(), plan.Accepted.end(), [](const FabIntakeRequest& r) { return r.Origin == FabIntakeOrigin::Typed; }),
                "the origin is propagated to every request");
            check.Expect(plan.Accepted[0].SizeBytes == zip.size(), "requests carry the classified size");
        }
        check.Expect(plan.Rejected.size() == 1 && plan.Rejected[0].Reason == FabIntakeReason::Missing
                && plan.Rejected[0].DisplayName == "readme-missing.txt",
            "a rejection carries a reason and the bare file name");
#if !defined(_WIN32)
        WriteFile(root / "bad\x1b[31mname\n.zip", ToBytes("junk junk junk junk junk junk"));
        const FabIntakePlan hostileName = PlanFabIntake(FabIntakeOrigin::Drop, std::vector<std::string> { (root / "bad\x1b[31mname\n.zip").string() });
        check.Expect(hostileName.Rejected.size() == 1 && hostileName.Rejected[0].DisplayName == "bad?[31mname?.zip",
            "control bytes in a rejected file name are replaced before display");
#endif

        FabIntakeLimits twoPaths;
        twoPaths.MaximumPathsPerSubmission = 2;
        const FabIntakePlan capped = PlanFabIntake(FabIntakeOrigin::Drop, submitted, twoPaths);
        check.Expect(capped.Accepted.size() == 2 && capped.Rejected.size() == 3
                && std::count_if(capped.Rejected.begin(), capped.Rejected.end(),
                       [](const FabIntakeRejection& r) { return r.Reason == FabIntakeReason::TooManyPaths; }) == 3,
            "paths beyond the submission cap are rejected as TooManyPaths without being classified");
        const std::vector<std::string> flood(1000, (root / "pkg.zip").string() + "x");
        const FabIntakePlan floodPlan = PlanFabIntake(FabIntakeOrigin::Drop, flood);
        check.Expect(floodPlan.Accepted.empty() && floodPlan.Rejected.size() == 1,
            "a flood of one repeated path costs one classification and one rejection");
        std::vector<std::string> distinct;
        for (int index = 0; index < 1000; ++index)
            distinct.push_back((root / ("missing" + std::to_string(index))).string());
        const FabIntakePlan distinctPlan = PlanFabIntake(FabIntakeOrigin::Drop, distinct);
        check.Expect(distinctPlan.Accepted.empty() && distinctPlan.Rejected.size() == 1000
                && std::count_if(distinctPlan.Rejected.begin(), distinctPlan.Rejected.end(),
                       [](const FabIntakeRejection& r) { return r.Reason == FabIntakeReason::TooManyPaths; }) == 968,
            "of 1000 distinct paths only the first 32 are classified");
        return check.Ok;
    }
}

namespace
{
    // ---- fake surface session -----------------------------------------------------

    class RecordingListener final : public IBrowserSurface::Listener
    {
    public:
        explicit RecordingListener(std::filesystem::path stagingRoot)
            : m_StagingRoot(std::move(stagingRoot))
        {
        }

        void OnFrame(const BrowserFrameView& frame) override
        {
            Frames.push_back(m_Mirror.ApplyFrame(frame));
        }

        void OnCursor(BrowserCursor cursor) override { Cursors.push_back(cursor); }
        void OnAddress(std::string_view display) override { Addresses.emplace_back(display); }
        void OnLoadState(bool loading, bool back, bool forward) override { LoadStates.push_back({ loading, back, forward }); }

        void OnDownload(const BrowserDownloadEvent& event) override
        {
            Downloads.push_back(event);
            if (event.Kind == BrowserDownloadEvent::State::Started)
            {
                // The adapter's job: the engine writes the file at the staged path.
                std::error_code error;
                std::filesystem::create_directories(event.StagedPath.parent_path(), error);
                WriteFile(event.StagedPath, StagedBytes);
            }
        }

        void OnNavigationDenied(std::string_view host) override { DeniedHosts.emplace_back(host); }
        void OnNavigationConsentOffered(std::string_view host) override { ConsentHosts.emplace_back(host); }
        void OnPopupRedirected(std::string_view host) override { PopupHosts.emplace_back(host); }
        void OnFailed(std::string_view reason) override { Failures.emplace_back(reason); }
        void OnClosed() override { ++ClosedCount; }

        BrowserFrameMirror& Mirror() { return m_Mirror; }

        struct LoadState
        {
            bool Loading, CanBack, CanForward;
        };

        std::vector<BrowserFrameMirror::ApplyResult> Frames;
        std::vector<BrowserCursor> Cursors;
        std::vector<std::string> Addresses;
        std::vector<LoadState> LoadStates;
        std::vector<BrowserDownloadEvent> Downloads;
        std::vector<std::string> DeniedHosts;
        std::vector<std::string> ConsentHosts;
        std::vector<std::string> PopupHosts;
        std::vector<std::string> Failures;
        size_t ClosedCount = 0;
        Bytes StagedBytes;

    private:
        std::filesystem::path m_StagingRoot;
        BrowserFrameMirror m_Mirror { 64 };
    };

    bool CheckSurfaceConfigValidation()
    {
        Checker check { "surface-config" };
#if defined(_WIN32)
        const std::filesystem::path profile = "C:\\Users\\u\\AppData\\Local\\Spiral\\fab-profile";
        const std::filesystem::path staging = "C:\\Users\\u\\AppData\\Local\\Spiral\\fab-staging";
        const std::filesystem::path relative = "profile";
#else
        const std::filesystem::path profile = "/home/u/.local/share/Spiral/fab-profile";
        const std::filesystem::path staging = "/home/u/.local/share/Spiral/fab-staging";
        const std::filesystem::path relative = "profile";
#endif
        BrowserSurfaceConfig good;
        good.ProfileDir = profile;
        good.DownloadStagingDir = staging;
        std::string error;
        check.Expect(ValidateBrowserSurfaceConfig(good, error) && error.empty(), "a canonical absolute configuration is accepted");

        const auto rejects = [&](const char* what, auto mutate)
        {
            BrowserSurfaceConfig config = good;
            mutate(config);
            std::string localError = "stale";
            check.Expect(!ValidateBrowserSurfaceConfig(config, localError) && !localError.empty() && localError != "stale",
                std::string(what) + " is rejected with a reason");
            check.Expect(localError.find("home") == std::string::npos && localError.find("Users") == std::string::npos,
                std::string(what) + ": the reason never echoes a path");
        };
        rejects("empty profile", [](BrowserSurfaceConfig& c) { c.ProfileDir.clear(); });
        rejects("relative profile", [&](BrowserSurfaceConfig& c) { c.ProfileDir = relative; });
        rejects("profile with a parent component", [&](BrowserSurfaceConfig& c) { c.ProfileDir = profile / ".." / "other"; });
        rejects("profile with a dot component", [&](BrowserSurfaceConfig& c) { c.ProfileDir = profile / "." / "p"; });
        rejects("profile with a trailing separator", [&](BrowserSurfaceConfig& c) { c.ProfileDir = profile / ""; });
        rejects("empty staging", [](BrowserSurfaceConfig& c) { c.DownloadStagingDir.clear(); });
        rejects("relative staging", [&](BrowserSurfaceConfig& c) { c.DownloadStagingDir = relative; });
        rejects("identical profile and staging", [&](BrowserSurfaceConfig& c) { c.DownloadStagingDir = c.ProfileDir; });
        rejects("staging inside profile", [&](BrowserSurfaceConfig& c) { c.DownloadStagingDir = c.ProfileDir / "Downloads"; });
        rejects("profile inside staging", [&](BrowserSurfaceConfig& c) { c.ProfileDir = c.DownloadStagingDir / "profile"; });
        rejects("relative helper", [&](BrowserSurfaceConfig& c) { c.HelperPath = relative; });
        rejects("relative resource directory", [&](BrowserSurfaceConfig& c) { c.ResourceDir = relative; });
        rejects("zero frame rate", [](BrowserSurfaceConfig& c) { c.MaxFps = 0; });
        rejects("frame rate above 60", [](BrowserSurfaceConfig& c) { c.MaxFps = 61; });

        BrowserSurfaceConfig sibling = good;
        sibling.DownloadStagingDir = profile.parent_path() / "fab-profile-staging";
        check.Expect(ValidateBrowserSurfaceConfig(sibling, error), "a sibling directory that merely shares a name prefix is not nested");
        BrowserSurfaceConfig optional = good;
        optional.HelperPath = profile.parent_path() / "SpiralBrowserHelper";
        optional.ResourceDir = profile.parent_path() / "cef";
        optional.MaxFps = 1;
        check.Expect(ValidateBrowserSurfaceConfig(optional, error), "optional absolute paths and the minimum frame rate are accepted");
        optional.MaxFps = 60;
        check.Expect(ValidateBrowserSurfaceConfig(optional, error), "the maximum frame rate is accepted");
        return check.Ok;
    }

    bool CheckNullSurface()
    {
        Checker check { "null-surface" };
        NullBrowserSurface surface("no engine in this build");
        RecordingListener listener("unused");
        std::string error;
        check.Expect(!surface.Initialize({}, listener, error) && error == "browser unavailable: no engine in this build",
            "Initialize fails and states the reason");
        check.Expect(surface.BackendName() == "null" && surface.IsClosed(), "the null surface names itself and is closed");
        surface.Pump();
        surface.SetViewSize({ 100, 100, 1.0f });
        surface.SetVisible(true);
        surface.SetFocus(true);
        surface.SendMouseMove({}, false);
        surface.SendMouseButton({}, BrowserMouseButton::Left, true, 1);
        surface.SendMouseCaptureLost();
        surface.SendMouseWheel({}, 1, 1);
        surface.SendKey({});
        surface.SetScreenInfo({});
        surface.SetGrantedHosts({});
        check.Expect(!surface.RetryDeniedNavigation(), "the null surface has nothing to repeat");
        surface.Navigate("https://www.fab.com/");
        surface.GoBack();
        surface.GoForward();
        surface.Reload();
        surface.Stop();
        surface.CancelDownload(1);
        surface.ClearBrowsingData();
        surface.RequestClose();
        surface.Shutdown(100);
        check.Expect(listener.Frames.empty() && listener.Addresses.empty() && listener.Downloads.empty() && listener.ClosedCount == 0
                && listener.Failures.empty(),
            "no operation on the null surface calls the listener");
        return check.Ok;
    }

    bool CheckFakeSession()
    {
        Checker check { "fake-session" };
        TempDir fixture("session");
        const std::filesystem::path staging = fixture.Path() / "staging";

        BrowserSurfaceConfig config;
        config.ProfileDir = fixture.Path() / "profile";
        config.DownloadStagingDir = staging;
        std::string error;
        check.Expect(config.Navigation.AddProviderHost("accounts.example.com", error), "provider host added to the session policy");

        RecordingListener listener(staging);
        listener.StagedBytes = MakeZip("model.glb", MakeGlb(kMinimalJson));
        SpiralTests::FakeBrowserSurface surface;
        surface.FailInitialize = true;
        check.Expect(!surface.Initialize(config, listener, error) && !error.empty() && !surface.IsInitialized(), "scripted initialisation failure");
        surface.FailInitialize = false;
        check.Expect(surface.Initialize(config, listener, error) && surface.IsInitialized() && surface.BackendName() == "fake", "initialisation");

        // Scripting alone delivers nothing: callbacks happen inside Pump only.
        surface.Navigate("https://www.fab.com/listings/abc?token=secret");
        surface.ScriptNavigationRequest("https://evil.example/phish?x=1", BrowserNavigationKind::TopLevel);
        surface.ScriptNavigationRequest("https://www.fab.com/new-window", BrowserNavigationKind::Popup);
        surface.ScriptNavigationRequest("https://popup.example.net/open?x=1", BrowserNavigationKind::Popup);
        surface.ScriptNavigationRequest("http://www.fab.com/", BrowserNavigationKind::TopLevel);
        surface.ScriptNavigationRequest("https://user:pw@www.fab.com/", BrowserNavigationKind::TopLevel);
        surface.ScriptNavigationRequest("https://accounts.example.com/signin", BrowserNavigationKind::TopLevel);
        surface.ScriptNavigationRequest("https://captcha.example.net/frame", BrowserNavigationKind::SubFrame);
        surface.ScriptCursor(BrowserCursor::Hand);
        check.Expect(listener.Addresses.empty() && listener.DeniedHosts.empty() && listener.Cursors.empty(), "nothing is delivered before Pump");
        surface.Pump();
        check.Expect(listener.Addresses
                == std::vector<std::string> { "www.fab.com/listings/abc", "www.fab.com/new-window", "accounts.example.com/signin" },
            "allowed top-level navigations, including the allowed popup, show a query-free display address");
        check.Expect(listener.PopupHosts == std::vector<std::string> { "www.fab.com" }, "only the allowed popup is reported as redirected");
        check.Expect(listener.DeniedHosts
                == std::vector<std::string> { "evil.example", "popup.example.net", "www.fab.com", "<invalid-host>" },
            "denied navigations, including the popup to an unlisted host, report a log-safe host only");
        check.Expect(listener.ConsentHosts == std::vector<std::string> { "evil.example", "popup.example.net" },
            "only a well-formed https denial on a valid unlisted host offers consent");
        check.Expect(listener.LoadStates.size() == 6 && listener.LoadStates[0].Loading && !listener.LoadStates[0].CanBack
                && !listener.LoadStates[1].Loading && listener.LoadStates[1].CanBack && listener.LoadStates[2].CanBack,
            "load state transitions for the three allowed navigations");
        check.Expect(listener.Cursors == std::vector<BrowserCursor> { BrowserCursor::Hand }, "scripted cursor delivered");
        for (const std::string& host : listener.DeniedHosts)
            check.Expect(host.find("secret") == std::string::npos && host.find("pw") == std::string::npos && host.find('?') == std::string::npos,
                "denied-host text carries no query or credentials");

        // Frames flow into the mirror.
        Bytes bgra(8 * 4 * 4);
        for (size_t index = 0; index < bgra.size(); ++index)
            bgra[index] = static_cast<u8>(index * 7 + 3);
        surface.ScriptFrame(8, 4, bgra, { { 0, 0, 8, 4 } });
        Bytes changed = bgra;
        changed[(1 * 8 + 5) * 4] = 200;
        surface.ScriptFrame(8, 4, changed, { { 5, 1, 1, 1 } });
        surface.Pump();
        check.Expect(listener.Frames.size() == 2 && listener.Frames[0] == BrowserFrameMirror::ApplyResult::Resized
                && listener.Frames[1] == BrowserFrameMirror::ApplyResult::Applied,
            "frames reach the listener's mirror");
        const Image expected = ReferenceRgba(changed, 8, 4, 32);
        const auto pixels = listener.Mirror().Pixels();
        check.Expect(std::equal(pixels.begin(), pixels.end(), expected.Rgba.begin(), expected.Rgba.end()), "mirror equals the swizzled latest frame");

        // Downloads: accepted, blocked by type, blocked by size, cancelled over the limit, failed.
        const u64 limit = config.Downloads.Limits().MaximumBytes;
        surface.ScriptDownload("Fab Asset.zip", 120, { 40, 80, 120 }, SpiralTests::FakeBrowserSurface::DownloadEnd::Complete);
        surface.ScriptDownload("../evil.exe", 10, {}, SpiralTests::FakeBrowserSurface::DownloadEnd::Complete);
        surface.ScriptDownload("huge.zip", limit + 1, {}, SpiralTests::FakeBrowserSurface::DownloadEnd::Complete);
        surface.ScriptDownload("runaway.zip", 0, { 10, limit + 1 }, SpiralTests::FakeBrowserSurface::DownloadEnd::Complete);
        surface.ScriptDownload("broken.glb", 50, { 25 }, SpiralTests::FakeBrowserSurface::DownloadEnd::Fail);
        surface.Pump();
        using State = BrowserDownloadEvent::State;
        std::vector<std::pair<std::string, State>> seen;
        for (const BrowserDownloadEvent& event : listener.Downloads)
            seen.emplace_back(event.DisplayName, event.Kind);
        const std::vector<std::pair<std::string, State>> wanted = {
            { "Fab_Asset.zip", State::Started }, { "Fab_Asset.zip", State::Progress }, { "Fab_Asset.zip", State::Progress },
            { "Fab_Asset.zip", State::Progress }, { "Fab_Asset.zip", State::Completed },
            { "evil.exe", State::Blocked }, { "huge.zip", State::Blocked },
            { "runaway.zip", State::Started }, { "runaway.zip", State::Progress }, { "runaway.zip", State::Cancelled },
            { "broken.glb", State::Started }, { "broken.glb", State::Progress }, { "broken.glb", State::Failed } };
        check.Expect(seen == wanted, "download event sequence matches the policy decisions");

        std::set<std::filesystem::path> stagedDirectories;
        for (const BrowserDownloadEvent& event : listener.Downloads)
        {
            if (event.Kind == State::Blocked)
            {
                check.Expect(event.StagedPath.empty(), "a blocked download has no staged path");
                continue;
            }
            const std::filesystem::path relative = event.StagedPath.lexically_normal().lexically_relative(staging.lexically_normal());
            check.Expect(!relative.empty() && *relative.begin() != ".." && std::distance(relative.begin(), relative.end()) == 2,
                "a staged path is exactly <staging>/<unique-directory>/<name>");
            stagedDirectories.insert(event.StagedPath.parent_path());
        }
        check.Expect(stagedDirectories.size() == 3, "each accepted download got its own staging directory");

        const BrowserDownloadEvent& completed = listener.Downloads[4];
        check.Expect(completed.Kind == State::Completed && completed.Bytes == 120 && completed.TotalBytes == 120, "completion event carries sizes");
        const FabIntakeClassification handoff = ClassifyFabIntakePath(completed.StagedPath);
        check.Expect(handoff.Kind == FabIntakeKind::Zip && handoff.Reason == FabIntakeReason::None,
            "the completed staged file classifies as a zip for the import controller");
        const FabIntakePlan plan = PlanFabIntake(FabIntakeOrigin::Download, std::vector<std::string> { completed.StagedPath.string() });
        check.Expect(plan.Accepted.size() == 1 && plan.Accepted[0].Origin == FabIntakeOrigin::Download && plan.Rejected.empty(),
            "the download hands off through PlanFabIntake with the Download origin");

        // The router drives the surface.
        BrowserInputRouter router;
        router.UpdatePanel(Panel(false, true));
        const auto forward = [&](const BrowserInputRoute& route, bool keyboardEvent)
        {
            if (route.SendCaptureLost)
                surface.SendMouseCaptureLost();
            for (const int key : route.SyntheticKeyUps)
                surface.SendKey({ BrowserKey::Phase::Up, key, TranslateGlfwKeyToWindowsVirtualKey(key), 0, 0, 0, false });
            if (route.KeyboardOwnerChanged)
                surface.SetFocus(router.OwnsKeyboard());
            if (route.Browser && keyboardEvent)
                surface.SendKey(route.Key);
        };
        const auto click = router.OnMouseButton(BrowserMouseButton::Left, true, 200, 100, 0, 5000);
        surface.SendMouseButton(click.Mouse, click.Button, click.ButtonDown, click.ClickCount);
        forward(router.UpdatePanel(Panel(true, true)), false);
        const auto clickUp = router.OnMouseButton(BrowserMouseButton::Left, false, 200, 100, 0, 5010);
        surface.SendMouseButton(clickUp.Mouse, clickUp.Button, clickUp.ButtonDown, clickUp.ClickCount);
        forward(router.OnKey(90, 44, 1, BrowserModifier::Control), true);
        forward(router.OnChar(U'q', 0), true);
        forward(router.OnKey(90, 44, 0, BrowserModifier::Control), true);
        forward(router.OnWindowFocus(false), false);
        check.Expect(surface.FocusStates == std::vector<bool> { true, false }, "SetFocus follows keyboard ownership");
        check.Expect(surface.Keys.size() == 3 && surface.Keys[0].WindowsVirtualKey == 0x5A && surface.Keys[0].Modifiers == BrowserModifier::Control
                && surface.Keys[1].Kind == BrowserKey::Phase::Char && surface.Keys[1].Codepoint == U'q'
                && surface.Keys[2].Kind == BrowserKey::Phase::Up,
            "the browser saw Ctrl+Z, the typed character, and the key release");
        check.Expect(surface.MouseButtons.size() == 2 && surface.MouseButtons[0].ClickCount == 1 && surface.MouseButtons[0].Down
                && !surface.MouseButtons[1].Down,
            "the browser saw a click pair");

        // Orderly shutdown: close request, pump, listener notified, then Shutdown.
        surface.RequestClose();
        check.Expect(!surface.IsClosed(), "closing is asynchronous: not closed before Pump");
        surface.Pump();
        check.Expect(surface.IsClosed() && listener.ClosedCount == 1, "Pump delivers OnClosed exactly once");
        surface.Pump();
        check.Expect(listener.ClosedCount == 1, "OnClosed is not repeated");
        surface.Shutdown(3000);
        check.Expect(!surface.ShutdownBeforeClose && surface.ShutdownTimeouts == std::vector<u32> { 3000 }, "Shutdown after close with its timeout");
        check.Expect(surface.OutOfPumpCallbacks() == 0, "every listener callback was delivered inside Pump");
        return check.Ok;
    }
}

namespace SpiralTests
{
    bool TestBrowserSwizzleAndPhysicalSize()
    {
        return CheckSwizzleAndPhysicalSize();
    }

    bool TestBrowserFrameMirrorProperty()
    {
        bool ok = CheckDirtyRectsAreTheCopyBoundary();
        ok = RunProperty("frame-mirror", MirrorProperty, 400) && ok;
        return ok;
    }

    bool TestBrowserDirtyRectCoalescing()
    {
        bool ok = CheckCoalesceExamples();
        ok = RunProperty("dirty-coalescing", CoalesceProperty, 500) && ok;
        return ok;
    }

    bool TestBrowserNavigationPolicy()
    {
        return CheckNavigationPolicy();
    }

    bool TestBrowserDownloadPolicy()
    {
        return CheckDownloadPolicy();
    }

    bool TestBrowserKeyTranslation()
    {
        return CheckKeyTranslation();
    }

    bool TestBrowserInputRouterScenarios()
    {
        bool ok = CheckRouterKeyboard();
        ok = CheckRouterMouse() && ok;
        return ok;
    }

    bool TestBrowserInputRouterModel()
    {
        return RunProperty("input-router-model", RouterModelProperty, 600);
    }

    bool TestFabIntakeClassification()
    {
        return CheckIntakeClassification();
    }

    bool TestBrowserSurfaceFakeSession()
    {
        bool ok = CheckNullSurface();
        ok = CheckSurfaceConfigValidation() && ok;
        ok = CheckFakeSession() && ok;
        return ok;
    }
}
