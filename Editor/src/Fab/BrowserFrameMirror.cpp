#include "BrowserFrameMirror.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace Fab
{
    namespace
    {
        using Engine::u32;
        using Engine::u64;
        using Engine::u8;

        constexpr size_t kMaximumRectsBeforeCollapse = 64;

        u64 AreaOf(const BrowserDirtyRect& rect)
        {
            return static_cast<u64>(rect.Width) * static_cast<u64>(rect.Height);
        }

        bool Intersects(const BrowserDirtyRect& a, const BrowserDirtyRect& b)
        {
            return a.X < b.X + b.Width && b.X < a.X + a.Width && a.Y < b.Y + b.Height && b.Y < a.Y + a.Height;
        }

        BrowserDirtyRect BoundingBox(const BrowserDirtyRect& a, const BrowserDirtyRect& b)
        {
            const int left = std::min(a.X, b.X);
            const int top = std::min(a.Y, b.Y);
            const int right = std::max(a.X + a.Width, b.X + b.Width);
            const int bottom = std::max(a.Y + a.Height, b.Y + b.Height);
            return { left, top, right - left, bottom - top };
        }

        bool Clip(BrowserDirtyRect rect, u32 width, u32 height, BrowserDirtyRect& out)
        {
            if (rect.Width <= 0 || rect.Height <= 0)
                return false;
            const long long left = std::max<long long>(rect.X, 0);
            const long long top = std::max<long long>(rect.Y, 0);
            const long long right = std::min<long long>(static_cast<long long>(rect.X) + rect.Width, width);
            const long long bottom = std::min<long long>(static_cast<long long>(rect.Y) + rect.Height, height);
            if (right <= left || bottom <= top)
                return false;
            out = { static_cast<int>(left), static_cast<int>(top), static_cast<int>(right - left),
                static_cast<int>(bottom - top) };
            return true;
        }

        void MergeOverlaps(std::vector<BrowserDirtyRect>& rects)
        {
            bool merged = true;
            while (merged)
            {
                merged = false;
                for (size_t first = 0; first < rects.size() && !merged; ++first)
                {
                    for (size_t second = first + 1; second < rects.size(); ++second)
                    {
                        if (!Intersects(rects[first], rects[second]))
                            continue;
                        rects[first] = BoundingBox(rects[first], rects[second]);
                        rects.erase(rects.begin() + static_cast<std::ptrdiff_t>(second));
                        merged = true;
                        break;
                    }
                }
            }
        }
    }

    BrowserPixelSize ComputePhysicalSize(const BrowserViewSize& view, u32 maximumDimension)
    {
        float scale = view.DeviceScale;
        if (!std::isfinite(scale) || scale <= 0.0f)
            scale = 1.0f;
        scale = std::clamp(scale, 0.25f, 8.0f);
        const auto axis = [&](u32 logical)
        {
            if (logical == 0)
                logical = 1;
            const double physical = std::round(static_cast<double>(logical) * static_cast<double>(scale));
            return static_cast<u32>(std::clamp(physical, 1.0, static_cast<double>(std::max<u32>(maximumDimension, 1))));
        };
        return { axis(view.Width), axis(view.Height) };
    }

    void SwizzleBgraToRgba(const u8* source, u8* destination, size_t pixelCount)
    {
        size_t index = 0;
        if constexpr (std::endian::native == std::endian::little)
        {
            for (; index < pixelCount; ++index)
            {
                u32 pixel = 0;
                std::memcpy(&pixel, source + index * 4, 4);
                pixel = (pixel & 0xFF00FF00u) | ((pixel & 0xFFu) << 16) | ((pixel >> 16) & 0xFFu);
                std::memcpy(destination + index * 4, &pixel, 4);
            }
            return;
        }
        for (; index < pixelCount; ++index)
        {
            const u8* in = source + index * 4;
            u8* out = destination + index * 4;
            out[0] = in[2];
            out[1] = in[1];
            out[2] = in[0];
            out[3] = in[3];
        }
    }

    std::vector<BrowserDirtyRect> CoalesceDirtyRects(
        std::span<const BrowserDirtyRect> rects, u32 width, u32 height, size_t maximumRects)
    {
        maximumRects = std::max<size_t>(maximumRects, 1);
        std::vector<BrowserDirtyRect> clipped;
        clipped.reserve(std::min(rects.size(), kMaximumRectsBeforeCollapse));
        bool collapse = rects.size() > kMaximumRectsBeforeCollapse;
        BrowserDirtyRect bounds {};
        bool haveBounds = false;
        for (const BrowserDirtyRect& rect : rects)
        {
            BrowserDirtyRect inside;
            if (!Clip(rect, width, height, inside))
                continue;
            if (collapse)
            {
                bounds = haveBounds ? BoundingBox(bounds, inside) : inside;
                haveBounds = true;
            }
            else
            {
                clipped.push_back(inside);
            }
        }
        if (collapse)
            return haveBounds ? std::vector<BrowserDirtyRect> { bounds } : std::vector<BrowserDirtyRect> {};

        MergeOverlaps(clipped);
        while (clipped.size() > maximumRects)
        {
            size_t bestFirst = 0;
            size_t bestSecond = 1;
            u64 bestWaste = std::numeric_limits<u64>::max();
            for (size_t first = 0; first < clipped.size(); ++first)
            {
                for (size_t second = first + 1; second < clipped.size(); ++second)
                {
                    const u64 merged = AreaOf(BoundingBox(clipped[first], clipped[second]));
                    const u64 parts = AreaOf(clipped[first]) + AreaOf(clipped[second]);
                    const u64 waste = merged > parts ? merged - parts : 0;
                    if (waste < bestWaste)
                    {
                        bestWaste = waste;
                        bestFirst = first;
                        bestSecond = second;
                    }
                }
            }
            clipped[bestFirst] = BoundingBox(clipped[bestFirst], clipped[bestSecond]);
            clipped.erase(clipped.begin() + static_cast<std::ptrdiff_t>(bestSecond));
            MergeOverlaps(clipped);
        }
        return clipped;
    }

    BrowserFrameMirror::BrowserFrameMirror(u32 maximumDimension)
        : m_MaximumDimension(std::clamp<u32>(maximumDimension, 1, 16384))
    {
    }

    BrowserFrameMirror::ApplyResult BrowserFrameMirror::ApplyFrame(const BrowserFrameView& frame)
    {
        if (frame.Bgra == nullptr || frame.Width == 0 || frame.Height == 0 || frame.Width > m_MaximumDimension
            || frame.Height > m_MaximumDimension || frame.StrideBytes < frame.Width * 4u)
        {
            return ApplyResult::Rejected;
        }

        const bool resized = frame.Width != m_Width || frame.Height != m_Height;
        std::vector<BrowserDirtyRect> copy;
        if (!resized)
            copy = CoalesceDirtyRects(frame.Dirty, frame.Width, frame.Height);
        if (copy.empty())
            copy.push_back({ 0, 0, static_cast<int>(frame.Width), static_cast<int>(frame.Height) });

        if (resized)
        {
            m_Pixels.assign(static_cast<size_t>(frame.Width) * frame.Height * 4u, 0);
            m_Width = frame.Width;
            m_Height = frame.Height;
            m_Dirty.clear();
            ++m_ResizeSerial;
        }

        for (const BrowserDirtyRect& rect : copy)
        {
            for (int row = rect.Y; row < rect.Y + rect.Height; ++row)
            {
                const u8* source = frame.Bgra + static_cast<size_t>(row) * frame.StrideBytes + static_cast<size_t>(rect.X) * 4u;
                u8* destination = m_Pixels.data() + (static_cast<size_t>(row) * m_Width + static_cast<size_t>(rect.X)) * 4u;
                SwizzleBgraToRgba(source, destination, static_cast<size_t>(rect.Width));
            }
        }

        std::vector<BrowserDirtyRect> accumulated = std::move(m_Dirty);
        accumulated.insert(accumulated.end(), copy.begin(), copy.end());
        m_Dirty = CoalesceDirtyRects(accumulated, m_Width, m_Height);
        ++m_Generation;
        return resized ? ApplyResult::Resized : ApplyResult::Applied;
    }

    void BrowserFrameMirror::TakeDirty(std::vector<BrowserDirtyRect>& out)
    {
        out = std::move(m_Dirty);
        m_Dirty.clear();
    }
}
