#pragma once

#include "BrowserSurface.h"

#include <span>
#include <vector>

namespace Fab
{
    // Physical pixel size of a view: round(logical * scale), each axis clamped
    // to [1, maximumDimension]. A zero logical axis, or a non-finite or
    // non-positive scale, is treated as 1. The scale is limited to [0.25, 8].
    BrowserPixelSize ComputePhysicalSize(const BrowserViewSize& view, Engine::u32 maximumDimension);

    // BGRA to RGBA for pixelCount tightly packed 4-byte pixels, alpha kept.
    // source and destination must not overlap.
    void SwizzleBgraToRgba(const Engine::u8* source, Engine::u8* destination, size_t pixelCount);

    // Clips every rectangle to [0,width) x [0,height), drops empty ones, and
    // merges until the output is pairwise disjoint and holds at most
    // maximumRects rectangles. The union of the output always contains the
    // union of the clipped input: merging only ever grows coverage, so a
    // changed pixel can never be lost. Inputs beyond 64 rectangles collapse to
    // their bounding box.
    std::vector<BrowserDirtyRect> CoalesceDirtyRects(
        std::span<const BrowserDirtyRect> rects, Engine::u32 width, Engine::u32 height, size_t maximumRects = 8);

    // Persistent tightly packed RGBA8 shadow of the browser's BGRA frames. The
    // GPU upload path reads Pixels() and TakeDirty(); nothing here touches a
    // graphics API.
    class BrowserFrameMirror
    {
    public:
        enum class ApplyResult
        {
            Applied,
            // The frame size differs from the mirror (first frame or resize):
            // storage was replaced and the whole frame copied.
            Resized,
            // Invalid frame; the mirror, its dirty set, and its counters are untouched.
            Rejected
        };

        explicit BrowserFrameMirror(Engine::u32 maximumDimension = 4096);

        // Copies only the pixels inside the coalesced dirty rectangles. An
        // empty dirty list, or one that clips to nothing, copies the whole
        // frame: the caller declared no usable change set, so none is trusted.
        ApplyResult ApplyFrame(const BrowserFrameView& frame);

        Engine::u32 Width() const { return m_Width; }
        Engine::u32 Height() const { return m_Height; }
        Engine::u32 StrideBytes() const { return m_Width * 4u; }
        std::span<const Engine::u8> Pixels() const { return m_Pixels; }

        bool HasDirty() const { return !m_Dirty.empty(); }
        // Moves the accumulated, coalesced dirty rectangles to out and clears them.
        void TakeDirty(std::vector<BrowserDirtyRect>& out);

        // Increments on every Applied or Resized frame.
        Engine::u64 Generation() const { return m_Generation; }
        // Increments whenever storage is replaced; a GPU texture of the old size is stale.
        Engine::u64 ResizeSerial() const { return m_ResizeSerial; }

    private:
        Engine::u32 m_MaximumDimension;
        Engine::u32 m_Width = 0;
        Engine::u32 m_Height = 0;
        std::vector<Engine::u8> m_Pixels;
        std::vector<BrowserDirtyRect> m_Dirty;
        Engine::u64 m_Generation = 0;
        Engine::u64 m_ResizeSerial = 0;
    };
}
