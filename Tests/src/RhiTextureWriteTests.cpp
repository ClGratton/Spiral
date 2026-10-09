#include "RhiTextureWriteTests.h"

#include "Engine/RHI/Device.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace Engine;
    namespace RHI = Engine::RHI;

    bool Check(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "RHI texture write test failed: " << message << '\n';
        return condition;
    }

    constexpr RHI::ResourceState kShaderResource = RHI::ResourceState::ShaderResource;
    constexpr RHI::ResourceState kCopyDest = RHI::ResourceState::CopyDest;

    constexpr RHI::TextureUsage Usage(RHI::TextureUsage a, RHI::TextureUsage b)
    {
        return static_cast<RHI::TextureUsage>(static_cast<u32>(a) | static_cast<u32>(b));
    }

    RHI::TextureDescription MakeDescription(u32 width, u32 height, u32 mipLevels = 1, u32 layers = 1)
    {
        RHI::TextureDescription description;
        description.DebugName = "RhiTextureWriteTest";
        description.Extent = { width, height };
        description.TextureFormat = RHI::Format::R8G8B8A8Unorm;
        description.Usage = Usage(RHI::TextureUsage::ShaderResource, RHI::TextureUsage::CopyDest);
        description.InitialState = kShaderResource;
        description.MipLevels = mipLevels;
        description.ArrayLayers = layers;
        return description;
    }

    // One analytic texel per (frame, x, y); expected images and uploaded
    // buffers are built from it with different loops.
    std::array<u8, 4> Texel(u32 frame, u32 x, u32 y)
    {
        return { static_cast<u8>(31u * frame + 7u * x + 3u * y + 1u), static_cast<u8>(97u * frame + 5u * x + 11u * y + 2u),
            static_cast<u8>(13u * frame + 17u * x + 29u * y + 3u), static_cast<u8>(0xF0u - 16u * frame + (x & 3u)) };
    }

    struct Rect
    {
        u32 X = 0;
        u32 Y = 0;
        u32 Width = 0;
        u32 Height = 0;
    };

    std::vector<u8> MakeSource(u32 frame, const Rect& rect, u32 pitchBytes)
    {
        std::vector<u8> bytes(static_cast<size_t>(pitchBytes) * rect.Height, 0xA5u);
        for (u32 row = 0; row < rect.Height; ++row)
            for (u32 column = 0; column < rect.Width; ++column)
            {
                const std::array<u8, 4> value = Texel(frame, rect.X + column, rect.Y + row);
                std::copy(value.begin(), value.end(), bytes.begin() + static_cast<std::ptrdiff_t>(row) * pitchBytes + column * 4);
            }
        return bytes;
    }

    RHI::TextureWrite MakeWrite(const Rect& rect, const std::vector<u8>& bytes, u32 pitchBytes, u32 mip = 0)
    {
        RHI::TextureWrite write;
        write.MipLevel = mip;
        write.X = rect.X;
        write.Y = rect.Y;
        write.Extent = { rect.Width, rect.Height };
        write.TextureFormat = RHI::Format::R8G8B8A8Unorm;
        write.RowPitchBytes = pitchBytes == rect.Width * 4u ? 0u : pitchBytes;
        write.Data = bytes.data();
        write.DataSizeBytes = bytes.size();
        return write;
    }

    // ---- Fake device -------------------------------------------------------

    class FakeCommandList;

    class FakeTexture final : public RHI::Texture
    {
    public:
        FakeTexture(RHI::TextureDescription description, u64 owner)
            : m_Description(std::move(description)), m_Owner(owner)
        {
            State = m_Description.InitialState;
            RHI::Extent2D extent = m_Description.Extent;
            for (u32 mip = 0; mip < m_Description.MipLevels; ++mip)
            {
                Mips.emplace_back(static_cast<size_t>(extent.Width) * extent.Height * 4u, u8 { 0 });
                extent.Width = std::max(extent.Width / 2, 1u);
                extent.Height = std::max(extent.Height / 2, 1u);
            }
        }
        const RHI::TextureDescription& GetDescription() const override { return m_Description; }
        u64 Owner() const { return m_Owner; }

        std::vector<std::vector<u8>> Mips;
        RHI::ResourceState State = RHI::ResourceState::Unknown;

    private:
        RHI::TextureDescription m_Description;
        u64 m_Owner;
    };

    // Bytes are copied out of the caller's buffer when the write is recorded
    // and live in the device until the submission completes.
    struct RecordedWrite
    {
        FakeTexture* Texture = nullptr;
        RHI::TextureWrite Write;
        std::vector<u8> PackedRows;
    };

    class FakeDevice final : public RHI::Device
    {
    public:
        explicit FakeDevice(bool writable) : m_Writable(writable) {}

        const RHI::DeviceDescription& GetDescription() const override { return m_Description; }
        const RHI::DeviceCapabilities& GetCapabilities() const override { return m_Capabilities; }
        Scope<RHI::Buffer> CreateBuffer(const RHI::BufferDescription&) override { return nullptr; }
        Scope<RHI::Texture> CreateTexture(const RHI::TextureDescription& description) override
        {
            return CreateScope<FakeTexture>(description, kDeviceId);
        }
        bool OwnsResource(const RHI::Buffer*) const override { return false; }
        bool OwnsResource(const RHI::Texture* resource) const override
        {
            const auto* texture = dynamic_cast<const FakeTexture*>(resource);
            return texture && texture->Owner() == kDeviceId;
        }
        bool QueryResourceState(const RHI::Buffer*, RHI::ResourceState&) const override { return false; }
        bool QueryResourceState(const RHI::Texture* resource, RHI::ResourceState& state) const override
        {
            const auto* texture = dynamic_cast<const FakeTexture*>(resource);
            if (!texture || !OwnsResource(resource))
                return false;
            state = texture->State;
            return true;
        }
        Scope<RHI::Shader> CreateShader(const RHI::ShaderDescription&) override { return nullptr; }
        Scope<RHI::Pipeline> CreatePipeline(const RHI::PipelineDescription&) override { return nullptr; }
        Scope<RHI::QueryPool> CreateQueryPool(const RHI::QueryPoolDescription&) override { return nullptr; }
        Scope<RHI::CommandList> CreateCommandList(RHI::QueueType queueType, std::string_view debugName) override;
        bool UploadBuffer(RHI::Buffer&, const void*, u64, u64) override { return false; }
        bool ReadbackTexture(RHI::Texture&, RHI::TextureReadback&) override { return false; }
        bool SupportsTextureWrite() const override { return m_Writable; }

        RHI::CompletionToken Submit(RHI::CommandList& commandList) override;
        RHI::CompletionStatus QueryCompletion(const RHI::CompletionToken& token) override
        {
            if (token.DeviceId != kDeviceId)
                return RHI::CompletionStatus::Invalid;
            const auto found = m_Submissions.find(token.SubmissionId);
            if (found == m_Submissions.end())
                return RHI::CompletionStatus::Invalid;
            return found->second.Executed ? RHI::CompletionStatus::Complete : RHI::CompletionStatus::Incomplete;
        }
        bool WaitForCompletion(const RHI::CompletionToken&, u32) override { ++WaitCalls; return false; }
        bool SubmitAndWait(RHI::CommandList&) override { ++WaitCalls; return false; }
        void WaitIdle() override { ++WaitCalls; }

        // Test-controlled "GPU": applies every pending submission in order.
        void ExecutePending()
        {
            for (auto& [id, submission] : m_Submissions)
            {
                (void)id;
                if (submission.Executed)
                    continue;
                for (const RecordedWrite& recorded : submission.Writes)
                    Apply(recorded);
                submission.Writes.clear();
                submission.Executed = true;
            }
        }
        size_t RetainedWriteCount() const
        {
            size_t count = 0;
            for (const auto& [id, submission] : m_Submissions)
            {
                (void)id;
                count += submission.Writes.size();
            }
            return count;
        }

        static constexpr u64 kDeviceId = 77;
        u32 WaitCalls = 0;

    private:
        struct Submission
        {
            std::vector<RecordedWrite> Writes;
            bool Executed = false;
        };

        static void Apply(const RecordedWrite& recorded)
        {
            const RHI::Extent2D mipExtent {
                std::max(recorded.Texture->GetDescription().Extent.Width >> recorded.Write.MipLevel, 1u),
                std::max(recorded.Texture->GetDescription().Extent.Height >> recorded.Write.MipLevel, 1u) };
            std::vector<u8>& image = recorded.Texture->Mips[recorded.Write.MipLevel];
            const size_t tightRow = static_cast<size_t>(recorded.Write.Extent.Width) * 4u;
            for (u32 row = 0; row < recorded.Write.Extent.Height; ++row)
                std::copy_n(recorded.PackedRows.begin() + static_cast<std::ptrdiff_t>(row * tightRow), tightRow,
                    image.begin() + static_cast<std::ptrdiff_t>((static_cast<size_t>(recorded.Write.Y + row) * mipExtent.Width + recorded.Write.X) * 4u));
        }

        RHI::DeviceDescription m_Description;
        RHI::DeviceCapabilities m_Capabilities;
        bool m_Writable;
        u64 m_NextSubmission = 1;
        std::map<u64, Submission> m_Submissions;
    };

    class FakeCommandList final : public RHI::CommandList
    {
    public:
        FakeCommandList(FakeDevice& device, RHI::QueueType queueType, bool writable)
            : m_Device(device), m_QueueType(queueType), m_Writable(writable) {}

        RHI::QueueType GetQueueType() const override { return m_QueueType; }
        bool Begin() override
        {
            if (m_State == State::Recording)
                return false;
            if (m_State == State::Submitted && m_Device.QueryCompletion(m_LastToken) != RHI::CompletionStatus::Complete)
                return false;
            m_State = State::Recording;
            m_Staged.clear();
            m_Writes.clear();
            m_OperationCount = 0;
            return true;
        }
        bool End() override
        {
            if (m_State != State::Recording)
                return false;
            m_State = State::Closed;
            return true;
        }
        void BeginDebugMarker(std::string_view) override {}
        void EndDebugMarker() override {}
        bool BindViewportOutputs(RHI::Texture&, RHI::Texture*) override { return false; }
        bool ClearViewportOutputs(const RHI::ViewportClear&) override { return false; }
        bool TransitionTexture(RHI::Texture& texture, RHI::ResourceState destination) override
        {
            auto* fake = dynamic_cast<FakeTexture*>(&texture);
            if (m_State != State::Recording || !fake || destination == RHI::ResourceState::Unknown)
                return false;
            if (CurrentState(*fake) == destination)
                return true;
            Stage(*fake).State = destination;
            ++m_OperationCount;
            return true;
        }
        bool TransitionTexture(RHI::Texture& texture, RHI::ResourceState expectedBefore, RHI::ResourceState destination) override
        {
            auto* fake = dynamic_cast<FakeTexture*>(&texture);
            if (m_State != State::Recording || !fake || expectedBefore == RHI::ResourceState::Unknown
                || destination == RHI::ResourceState::Unknown)
                return false;
            StagedState& staged = Stage(*fake);
            if (staged.Expected == RHI::ResourceState::Unknown)
                staged.Expected = expectedBefore;
            staged.State = destination;
            ++m_OperationCount;
            return true;
        }
        bool TransitionBuffer(RHI::Buffer&, RHI::ResourceState) override { return false; }
        void SetGraphicsPipeline(RHI::Pipeline&) override {}
        void SetGraphicsConstantBuffer(u32, RHI::Buffer&) override {}
        void SetViewport(const RHI::Viewport&) override {}
        void SetScissorRect(const RHI::ScissorRect&) override {}
        void SetVertexBuffer(u32, RHI::Buffer&) override {}
        void SetIndexBuffer(RHI::Buffer&, RHI::IndexFormat) override {}
        bool CopyBuffer(RHI::Buffer&, u64, RHI::Buffer&, u64, u64) override { return false; }
        void DrawIndexed(u32, u32, u32, int, u32) override {}
        bool ResetQueryPool(RHI::QueryPool&, u32, u32) override { return false; }
        bool WriteTimestamp(RHI::QueryPool&, u32) override { return false; }
        bool ResolveQueryPool(RHI::QueryPool&, u32, u32) override { return false; }

        bool WriteTexture(RHI::Texture& texture, const RHI::TextureWrite& write) override
        {
            auto* fake = dynamic_cast<FakeTexture*>(&texture);
            RHI::TextureWritePlan plan;
            if (!m_Writable || m_State != State::Recording || m_QueueType != RHI::QueueType::Graphics || !fake
                || !m_Device.OwnsResource(&texture)
                || RHI::ValidateTextureWrite(texture.GetDescription(), CurrentState(*fake), write, &plan) != RHI::TextureWriteStatus::Valid)
                return false;
            RecordedWrite recorded;
            recorded.Texture = fake;
            recorded.Write = write;
            recorded.Write.Data = nullptr;
            const u8* source = static_cast<const u8*>(write.Data);
            const size_t tightRow = static_cast<size_t>(write.Extent.Width) * 4u;
            for (u32 row = 0; row < write.Extent.Height; ++row)
            {
                const u8* rowStart = source + static_cast<size_t>(row) * static_cast<size_t>(plan.RowPitchBytes);
                recorded.PackedRows.insert(recorded.PackedRows.end(), rowStart, rowStart + tightRow);
            }
            m_Writes.push_back(std::move(recorded));
            if (m_Staged.find(fake) == m_Staged.end())
                m_Staged[fake] = { kCopyDest, kCopyDest };
            ++m_OperationCount;
            return true;
        }

        bool Ready() const { return m_State == State::Closed; }
        void MarkSubmitted(const RHI::CompletionToken& token)
        {
            m_LastToken = token;
            m_State = State::Submitted;
        }
        size_t OperationCount() const { return m_OperationCount; }
        size_t StagedCount() const { return m_Staged.size(); }
        std::vector<RecordedWrite> TakeWrites() { return std::move(m_Writes); }

        struct StagedState
        {
            RHI::ResourceState State = RHI::ResourceState::Unknown;
            RHI::ResourceState Expected = RHI::ResourceState::Unknown;
        };
        const std::map<FakeTexture*, StagedState>& Staged() const { return m_Staged; }

    private:
        enum class State { Ready, Recording, Closed, Submitted };

        StagedState& Stage(FakeTexture& texture)
        {
            auto found = m_Staged.find(&texture);
            if (found == m_Staged.end())
                found = m_Staged.emplace(&texture, StagedState { texture.State, RHI::ResourceState::Unknown }).first;
            return found->second;
        }
        RHI::ResourceState CurrentState(const FakeTexture& texture) const
        {
            const auto found = m_Staged.find(const_cast<FakeTexture*>(&texture));
            return found == m_Staged.end() ? texture.State : found->second.State;
        }

        FakeDevice& m_Device;
        RHI::QueueType m_QueueType;
        bool m_Writable;
        State m_State = State::Ready;
        RHI::CompletionToken m_LastToken;
        std::map<FakeTexture*, StagedState> m_Staged;
        std::vector<RecordedWrite> m_Writes;
        size_t m_OperationCount = 0;
    };

    Scope<RHI::CommandList> FakeDevice::CreateCommandList(RHI::QueueType queueType, std::string_view)
    {
        return CreateScope<FakeCommandList>(*this, queueType, m_Writable);
    }

    RHI::CompletionToken FakeDevice::Submit(RHI::CommandList& commandList)
    {
        auto* list = dynamic_cast<FakeCommandList*>(&commandList);
        if (!list || !list->Ready())
            return {};
        for (const auto& [texture, staged] : list->Staged())
            if (staged.Expected != RHI::ResourceState::Unknown && texture->State != staged.Expected)
                return {};
        for (const auto& [texture, staged] : list->Staged())
            texture->State = staged.State;
        const RHI::CompletionToken token { kDeviceId, m_NextSubmission++ };
        Submission submission;
        submission.Writes = list->TakeWrites();
        m_Submissions[token.SubmissionId] = std::move(submission);
        list->MarkSubmitted(token);
        return token;
    }

    // Independent expected image: last writer per texel, then Texel() again.
    struct Oracle
    {
        struct Level
        {
            u32 Width = 0;
            u32 Height = 0;
            std::vector<int> Frame;
        };
        std::vector<Level> Levels;

        Oracle(u32 width, u32 height, u32 mips)
        {
            for (u32 mip = 0; mip < mips; ++mip)
            {
                Levels.push_back({ width, height, std::vector<int>(static_cast<size_t>(width) * height, -1) });
                width = std::max(width / 2, 1u);
                height = std::max(height / 2, 1u);
            }
        }
        void Write(u32 mip, u32 frame, const Rect& rect)
        {
            Level& level = Levels[mip];
            for (u32 y = rect.Y; y < rect.Y + rect.Height; ++y)
                for (u32 x = rect.X; x < rect.X + rect.Width; ++x)
                    level.Frame[static_cast<size_t>(y) * level.Width + x] = static_cast<int>(frame);
        }
        bool Matches(const FakeTexture& texture) const
        {
            for (size_t mip = 0; mip < Levels.size(); ++mip)
                for (u32 y = 0; y < Levels[mip].Height; ++y)
                    for (u32 x = 0; x < Levels[mip].Width; ++x)
                    {
                        const int frame = Levels[mip].Frame[static_cast<size_t>(y) * Levels[mip].Width + x];
                        const std::array<u8, 4> expected = frame < 0 ? std::array<u8, 4> { 0, 0, 0, 0 } : Texel(static_cast<u32>(frame), x, y);
                        for (u32 channel = 0; channel < 4; ++channel)
                            if (texture.Mips[mip][(static_cast<size_t>(y) * Levels[mip].Width + x) * 4u + channel] != expected[channel])
                                return false;
                    }
            return true;
        }
    };

    bool SubmitClosed(FakeDevice& device, RHI::CommandList& list, RHI::CompletionToken& token)
    {
        token = device.Submit(list);
        return token.IsValid();
    }

    // ---- Oracle for the shared validator ------------------------------------

    bool OracleAccepts(const RHI::TextureDescription& description, RHI::ResourceState state, const RHI::TextureWrite& write)
    {
        if (!write.Data || write.TextureFormat != RHI::Format::R8G8B8A8Unorm || description.TextureFormat != write.TextureFormat)
            return false;
        if (write.MipLevel >= description.MipLevels || write.ArrayLayer >= description.ArrayLayers || state != kCopyDest)
            return false;
        long long width = description.Extent.Width;
        long long height = description.Extent.Height;
        for (u32 mip = 0; mip < write.MipLevel; ++mip)
        {
            width = std::max(width / 2, 1ll);
            height = std::max(height / 2, 1ll);
        }
        const long long w = write.Extent.Width;
        const long long h = write.Extent.Height;
        if (w == 0 || h == 0 || write.X + w > width || write.Y + h > height)
            return false;
        const long long tight = w * 4;
        const long long pitch = write.RowPitchBytes == 0 ? tight : static_cast<long long>(write.RowPitchBytes);
        if (pitch < tight)
            return false;
        return (h - 1) * pitch + tight <= static_cast<long long>(write.DataSizeBytes);
    }
}

namespace SpiralTests
{
    bool TestRhiTextureWriteValidationMatchesIndependentOracle()
    {
        bool ok = true;
        const std::array<u8, 1> byte { 0 };

        // Exhaustive small-space comparison against a signed-integer oracle.
        u64 evaluated = 0;
        u64 accepted = 0;
        for (const u32 layers : { 1u, 2u })
        {
            RHI::TextureDescription description = MakeDescription(5, 3, 3, layers);
            for (const RHI::ResourceState state : { kCopyDest, kShaderResource })
                for (u32 mip = 0; mip <= 3; ++mip)
                    for (u32 layer = 0; layer <= 2; ++layer)
                        for (u32 x = 0; x <= 6; ++x)
                            for (u32 y = 0; y <= 4; ++y)
                                for (u32 w = 0; w <= 6; ++w)
                                    for (u32 h = 0; h <= 4; ++h)
                                        for (const u64 pitchDelta : { 0ull, 1ull, 2ull, 5ull, 256ull })
                                        {
                                            RHI::TextureWrite write;
                                            write.MipLevel = mip;
                                            write.ArrayLayer = layer;
                                            write.X = x;
                                            write.Y = y;
                                            write.Extent = { w, h };
                                            write.TextureFormat = RHI::Format::R8G8B8A8Unorm;
                                            // 0 = tight; 1 is one below tight; the rest are at or above it.
                                            write.RowPitchBytes = pitchDelta == 0 ? 0 : pitchDelta == 1 ? static_cast<u64>(w) * 4u - 1u
                                                : static_cast<u64>(w) * 4u + pitchDelta - 2u;
                                            const u64 pitch = write.RowPitchBytes != 0 ? write.RowPitchBytes : static_cast<u64>(w) * 4u;
                                            const u64 exact = h == 0 ? 0 : (h - 1) * pitch + static_cast<u64>(w) * 4u;
                                            for (const u64 size : { exact, exact == 0 ? 0 : exact - 1, exact + 9, u64 { 0 } })
                                            {
                                                write.Data = byte.data();
                                                write.DataSizeBytes = size;
                                                RHI::TextureWritePlan plan;
                                                const bool actual = RHI::ValidateTextureWrite(description, state, write, &plan)
                                                    == RHI::TextureWriteStatus::Valid;
                                                const bool expected = OracleAccepts(description, state, write);
                                                ++evaluated;
                                                accepted += expected ? 1 : 0;
                                                if (actual != expected)
                                                {
                                                    ok = Check(false, "validator disagrees with oracle at mip=" + std::to_string(mip)
                                                        + " layer=" + std::to_string(layer) + " x=" + std::to_string(x) + " y=" + std::to_string(y)
                                                        + " w=" + std::to_string(w) + " h=" + std::to_string(h)
                                                        + " pitch=" + std::to_string(write.RowPitchBytes) + " size=" + std::to_string(size)) && ok;
                                                    return false;
                                                }
                                                if (actual)
                                                {
                                                    const long long mw = std::max(5ll >> mip, 1ll);
                                                    const long long mh = std::max(3ll >> mip, 1ll);
                                                    const bool whole = x == 0 && y == 0 && w == mw && h == mh;
                                                    if (plan.WholeSubresource != whole || plan.RowPitchBytes != pitch
                                                        || plan.RequiredSourceBytes != exact || plan.MipExtent.Width != mw
                                                        || plan.MipExtent.Height != mh)
                                                        return Check(false, "validated plan disagrees with the oracle plan");
                                                }
                                            }
                                        }
        }
        ok = Check(accepted > 1000 && evaluated > 100000, "exhaustive sweep covered both accepted and rejected writes") && ok;

        // Deterministic single-fault statuses.
        const auto statusOf = [&](const RHI::TextureDescription& description, RHI::ResourceState state, const RHI::TextureWrite& write)
        {
            return RHI::ValidateTextureWrite(description, state, write);
        };
        const RHI::TextureDescription base = MakeDescription(8, 8, 4);
        RHI::TextureWrite good;
        good.Extent = { 4, 4 };
        good.TextureFormat = RHI::Format::R8G8B8A8Unorm;
        good.Data = byte.data();
        good.DataSizeBytes = 64;
        ok = Check(statusOf(base, kCopyDest, good) == RHI::TextureWriteStatus::Valid, "baseline write is valid") && ok;
        const auto expectStatus = [&](const char* name, const RHI::TextureDescription& description, RHI::ResourceState state,
            const RHI::TextureWrite& write, RHI::TextureWriteStatus expected)
        {
            const RHI::TextureWriteStatus actual = statusOf(description, state, write);
            return Check(actual == expected, std::string(name) + ": got " + RHI::ToString(actual) + " expected " + RHI::ToString(expected));
        };
        auto with = [&](auto edit) { RHI::TextureWrite write = good; edit(write); return write; };
        auto describe = [&](auto edit) { RHI::TextureDescription description = base; edit(description); return description; };

        ok = expectStatus("null data", base, kCopyDest, with([](RHI::TextureWrite& w) { w.Data = nullptr; }), RHI::TextureWriteStatus::NullData) && ok;
        ok = expectStatus("unsupported write format", base, kCopyDest, with([](RHI::TextureWrite& w) { w.TextureFormat = RHI::Format::R8Unorm; }), RHI::TextureWriteStatus::UnsupportedFormat) && ok;
        ok = expectStatus("srgb write into unorm texture", base, kCopyDest, with([](RHI::TextureWrite& w) { w.TextureFormat = RHI::Format::R8G8B8A8UnormSrgb; }), RHI::TextureWriteStatus::FormatMismatch) && ok;
        ok = expectStatus("srgb texture accepts srgb write", describe([](RHI::TextureDescription& d) { d.TextureFormat = RHI::Format::R8G8B8A8UnormSrgb; }), kCopyDest,
            with([](RHI::TextureWrite& w) { w.TextureFormat = RHI::Format::R8G8B8A8UnormSrgb; }), RHI::TextureWriteStatus::Valid) && ok;
        ok = expectStatus("missing copy-dest usage", describe([](RHI::TextureDescription& d) { d.Usage = RHI::TextureUsage::ShaderResource; }), kCopyDest, good, RHI::TextureWriteStatus::MissingCopyDestUsage) && ok;
        ok = expectStatus("depth-stencil usage", describe([](RHI::TextureDescription& d) { d.Usage = Usage(d.Usage, RHI::TextureUsage::DepthStencil); }), kCopyDest, good, RHI::TextureWriteStatus::UnsupportedTexture) && ok;
        ok = expectStatus("multisampled", describe([](RHI::TextureDescription& d) { d.SampleCount = 4; }), kCopyDest, good, RHI::TextureWriteStatus::UnsupportedTexture) && ok;
        ok = expectStatus("zero mips", describe([](RHI::TextureDescription& d) { d.MipLevels = 0; }), kCopyDest, good, RHI::TextureWriteStatus::UnsupportedTexture) && ok;
        ok = expectStatus("too many mips", describe([](RHI::TextureDescription& d) { d.MipLevels = 5; }), kCopyDest, good, RHI::TextureWriteStatus::UnsupportedTexture) && ok;
        ok = expectStatus("zero extent texture", describe([](RHI::TextureDescription& d) { d.Extent.Width = 0; }), kCopyDest, good, RHI::TextureWriteStatus::UnsupportedTexture) && ok;
        ok = expectStatus("zero layers", describe([](RHI::TextureDescription& d) { d.ArrayLayers = 0; }), kCopyDest, good, RHI::TextureWriteStatus::UnsupportedTexture) && ok;
        ok = expectStatus("mip out of range", base, kCopyDest, with([](RHI::TextureWrite& w) { w.MipLevel = 4; }), RHI::TextureWriteStatus::MipOutOfRange) && ok;
        ok = expectStatus("layer out of range", base, kCopyDest, with([](RHI::TextureWrite& w) { w.ArrayLayer = 1; }), RHI::TextureWriteStatus::ArrayLayerOutOfRange) && ok;
        ok = expectStatus("empty width", base, kCopyDest, with([](RHI::TextureWrite& w) { w.Extent.Width = 0; }), RHI::TextureWriteStatus::EmptyRegion) && ok;
        ok = expectStatus("empty height", base, kCopyDest, with([](RHI::TextureWrite& w) { w.Extent.Height = 0; }), RHI::TextureWriteStatus::EmptyRegion) && ok;
        ok = expectStatus("region past right edge", base, kCopyDest, with([](RHI::TextureWrite& w) { w.X = 5; }), RHI::TextureWriteStatus::RegionOutOfBounds) && ok;
        ok = expectStatus("origin overflow does not wrap", base, kCopyDest,
            with([](RHI::TextureWrite& w) { w.X = std::numeric_limits<u32>::max(); w.Extent.Width = 2; }), RHI::TextureWriteStatus::RegionOutOfBounds) && ok;
        ok = expectStatus("region larger than mip", base, kCopyDest, with([](RHI::TextureWrite& w) { w.MipLevel = 3; }), RHI::TextureWriteStatus::RegionOutOfBounds) && ok;
        ok = expectStatus("wrong state", base, kShaderResource, good, RHI::TextureWriteStatus::WrongTextureState) && ok;
        ok = expectStatus("unknown state", base, RHI::ResourceState::Unknown, good, RHI::TextureWriteStatus::WrongTextureState) && ok;
        ok = expectStatus("short pitch", base, kCopyDest, with([](RHI::TextureWrite& w) { w.RowPitchBytes = 15; }), RHI::TextureWriteStatus::RowPitchTooSmall) && ok;
        ok = expectStatus("short data", base, kCopyDest, with([](RHI::TextureWrite& w) { w.DataSizeBytes = 63; }), RHI::TextureWriteStatus::DataTooSmall) && ok;
        ok = expectStatus("last row needs only tight bytes", base, kCopyDest,
            with([](RHI::TextureWrite& w) { w.RowPitchBytes = 32; w.DataSizeBytes = 3 * 32 + 16; }), RHI::TextureWriteStatus::Valid) && ok;
        ok = expectStatus("padded last row is not required", base, kCopyDest,
            with([](RHI::TextureWrite& w) { w.RowPitchBytes = 32; w.DataSizeBytes = 3 * 32 + 15; }), RHI::TextureWriteStatus::DataTooSmall) && ok;

        // Overflow and staging-bound table; the data size claims every byte is readable
        // so only the arithmetic and the staging cap can reject.
        const u32 big = std::numeric_limits<u32>::max();
        RHI::TextureDescription huge = MakeDescription(big, big);
        const u64 everything = std::numeric_limits<u64>::max();
        RHI::TextureWrite wide = good;
        wide.Extent = { big, big };
        wide.DataSizeBytes = everything;
        ok = expectStatus("u32-max by u32-max overflows u64 byte count", huge, kCopyDest, wide, RHI::TextureWriteStatus::SizeOverflow) && ok;
        RHI::TextureWrite hugePitch = good;
        hugePitch.Extent = { 4, 2 };
        hugePitch.RowPitchBytes = everything;
        hugePitch.DataSizeBytes = everything;
        ok = expectStatus("pitch times rows overflows", base, kCopyDest, hugePitch, RHI::TextureWriteStatus::SizeOverflow) && ok;
        hugePitch.RowPitchBytes = 1ull << 63;
        hugePitch.Extent = { 4, 3 };
        ok = expectStatus("2^63 pitch over three rows overflows", base, kCopyDest, hugePitch, RHI::TextureWriteStatus::SizeOverflow) && ok;
        hugePitch.Extent = { 4, 1 };
        hugePitch.DataSizeBytes = 16;
        ok = expectStatus("single row ignores an enormous pitch", base, kCopyDest, hugePitch, RHI::TextureWriteStatus::Valid) && ok;
        RHI::TextureDescription capacity = MakeDescription(8192, 8193);
        RHI::TextureWrite atCap = good;
        atCap.Extent = { 8192, 8192 };
        atCap.DataSizeBytes = everything;
        ok = expectStatus("staging cap is inclusive", capacity, kCopyDest, atCap, RHI::TextureWriteStatus::Valid) && ok;
        atCap.Extent.Height = 8193;
        ok = expectStatus("one row over the staging cap", capacity, kCopyDest, atCap, RHI::TextureWriteStatus::StagingTooLarge) && ok;
        RHI::TextureWrite tall = good;
        tall.Extent = { 1, big };
        tall.DataSizeBytes = everything;
        ok = expectStatus("tall column over the staging cap", huge, kCopyDest, tall, RHI::TextureWriteStatus::StagingTooLarge) && ok;
        return ok;
    }

    bool TestRhiTextureWriteFakeDeviceContract()
    {
        bool ok = true;
        FakeDevice device(true);
        ok = Check(device.SupportsTextureWrite(), "fake device advertises the capability") && ok;

        // 16x8 with three mips; frames land in mip 0, then mip 1.
        Scope<RHI::Texture> created = device.CreateTexture(MakeDescription(16, 8, 4));
        auto* texture = dynamic_cast<FakeTexture*>(created.get());
        if (!texture)
            return Check(false, "fake texture creation");
        Oracle oracle(16, 8, 4);
        Scope<RHI::CommandList> listOwner = device.CreateCommandList(RHI::QueueType::Graphics, "write");
        RHI::CommandList& list = *listOwner;
        RHI::CompletionToken token;

        // Frame 0: whole mip, padded pitch, caller scribbles the source right after recording.
        {
            const Rect whole { 0, 0, 16, 8 };
            std::vector<u8> bytes = MakeSource(0, whole, 16 * 4 + 12);
            ok = Check(list.Begin(), "begin frame 0") && ok;
            ok = Check(RHI::RecordTextureWrite(list, *texture, MakeWrite(whole, bytes, 16 * 4 + 12)), "frame 0 bracket records") && ok;
            std::fill(bytes.begin(), bytes.end(), 0xEEu);
            ok = Check(list.End() && SubmitClosed(device, list, token), "frame 0 submits") && ok;
            oracle.Write(0, 0, whole);
            ok = Check(device.QueryCompletion(token) == RHI::CompletionStatus::Incomplete, "write is asynchronous: submission stays incomplete") && ok;
            ok = Check(device.RetainedWriteCount() == 1, "recorded bytes are retained until completion") && ok;
            ok = Check(texture->State == kShaderResource, "bracket publishes the steady state") && ok;
            ok = Check(!list.Begin(), "list cannot be reset while its submission is in flight") && ok;
            ok = Check(std::all_of(texture->Mips[0].begin(), texture->Mips[0].end(), [](u8 value) { return value == 0; }),
                "nothing reaches the texture before the GPU runs") && ok;
            device.ExecutePending();
            ok = Check(device.QueryCompletion(token) == RHI::CompletionStatus::Complete && device.RetainedWriteCount() == 0,
                "completion retires the retained bytes") && ok;
            ok = Check(oracle.Matches(*texture), "frame 0 content equals the oracle despite the overwritten source") && ok;
        }

        // Frame 1: smaller region, different content, tight pitch.
        {
            const Rect region { 3, 2, 5, 3 };
            std::vector<u8> bytes = MakeSource(1, region, 5 * 4);
            ok = Check(list.Begin() && RHI::RecordTextureWrite(list, *texture, MakeWrite(region, bytes, 5 * 4)), "frame 1 records after the first completed") && ok;
            std::fill(bytes.begin(), bytes.end(), 0xEEu);
            ok = Check(list.End() && SubmitClosed(device, list, token), "frame 1 submits") && ok;
            device.ExecutePending();
            oracle.Write(0, 1, region);
            ok = Check(oracle.Matches(*texture), "frame 1 region replaces only its rectangle") && ok;
        }

        // Frame 2: overlapping padded regions in one list, later write wins; plus mip 1.
        {
            const Rect first { 4, 3, 4, 3 };
            const Rect second { 6, 4, 6, 3 };
            const Rect mipRegion { 1, 1, 4, 2 };
            std::vector<u8> firstBytes = MakeSource(2, first, 4 * 4);
            std::vector<u8> secondBytes = MakeSource(3, second, 6 * 4 + 8);
            std::vector<u8> mipBytes = MakeSource(4, mipRegion, 4 * 4 + 4);
            ok = Check(list.Begin(), "begin frame 2") && ok;
            ok = Check(RHI::RecordTextureWrite(list, *texture, MakeWrite(first, firstBytes, 4 * 4)), "frame 2 first region") && ok;
            ok = Check(RHI::RecordTextureWrite(list, *texture, MakeWrite(second, secondBytes, 6 * 4 + 8)), "frame 2 second region") && ok;
            ok = Check(RHI::RecordTextureWrite(list, *texture, MakeWrite(mipRegion, mipBytes, 4 * 4 + 4, 1)), "frame 2 mip 1 region") && ok;
            std::fill(firstBytes.begin(), firstBytes.end(), 0xEEu);
            std::fill(secondBytes.begin(), secondBytes.end(), 0xEEu);
            std::fill(mipBytes.begin(), mipBytes.end(), 0xEEu);
            ok = Check(list.End() && SubmitClosed(device, list, token), "frame 2 submits") && ok;
            device.ExecutePending();
            oracle.Write(0, 2, first);
            oracle.Write(0, 3, second);
            oracle.Write(1, 4, mipRegion);
            ok = Check(oracle.Matches(*texture), "overlap order, padded pitch, and mip addressing match the oracle") && ok;
        }

        // Rejections record nothing, stage no state, and never reach the texture.
        {
            const Rect inside { 1, 1, 2, 2 };
            std::vector<u8> bytes = MakeSource(5, inside, 8);
            ok = Check(list.Begin(), "begin rejection list") && ok;
            auto* fake = static_cast<FakeCommandList*>(&list);
            ok = Check(!list.WriteTexture(*texture, MakeWrite(inside, bytes, 8)), "write without a CopyDest transition is rejected") && ok;
            RHI::TextureWrite outOfBounds = MakeWrite({ 15, 7, 2, 2 }, bytes, 8);
            RHI::TextureWrite truncated = MakeWrite(inside, bytes, 8);
            truncated.DataSizeBytes = bytes.size() - 1;
            ok = Check(!RHI::RecordTextureWrite(list, *texture, outOfBounds), "bracket rejects an out-of-bounds region") && ok;
            ok = Check(!RHI::RecordTextureWrite(list, *texture, truncated), "bracket rejects truncated source data") && ok;
            ok = Check(!RHI::RecordTextureWrite(list, *texture, MakeWrite(inside, bytes, 8), RHI::ResourceState::CopyDest),
                "bracket rejects a CopyDest steady state") && ok;
            ok = Check(!RHI::RecordTextureWrite(list, *texture, MakeWrite(inside, bytes, 8), RHI::ResourceState::Unknown),
                "bracket rejects an Unknown steady state") && ok;
            ok = Check(fake->OperationCount() == 0 && fake->StagedCount() == 0,
                "validation failures record no transition, no write, and stage no state") && ok;
            ok = Check(list.TransitionTexture(*texture, kShaderResource, kCopyDest), "explicit transition into CopyDest") && ok;
            ok = Check(!list.WriteTexture(*texture, outOfBounds) && !list.WriteTexture(*texture, truncated),
                "direct writes still reject after the transition") && ok;
            ok = Check(fake->OperationCount() == 1, "only the transition was recorded") && ok;
            ok = Check(list.TransitionTexture(*texture, kCopyDest, kShaderResource) && list.End() && SubmitClosed(device, list, token),
                "rejection list submits") && ok;
            device.ExecutePending();
            ok = Check(oracle.Matches(*texture) && texture->State == kShaderResource, "content and state unchanged after rejections") && ok;
        }

        // Submit-time expected-state validation: a stale bracket is refused whole.
        {
            const Rect region { 0, 0, 2, 2 };
            std::vector<u8> bytes = MakeSource(6, region, 8);
            Scope<RHI::CommandList> staleOwner = device.CreateCommandList(RHI::QueueType::Graphics, "stale");
            ok = Check(staleOwner->Begin() && RHI::RecordTextureWrite(*staleOwner, *texture, MakeWrite(region, bytes, 8)) && staleOwner->End(),
                "stale bracket records") && ok;
            // Another accepted submission moves the texture before the stale list is submitted.
            ok = Check(list.Begin() && list.TransitionTexture(*texture, kShaderResource, RHI::ResourceState::CopySource)
                && list.End() && SubmitClosed(device, list, token), "intervening state change") && ok;
            const RHI::CompletionToken stale = device.Submit(*staleOwner);
            ok = Check(!stale.IsValid(), "submission with a wrong expected-before state is refused") && ok;
            device.ExecutePending();
            ok = Check(oracle.Matches(*texture) && texture->State == RHI::ResourceState::CopySource,
                "refused submission left content and state untouched") && ok;
            // A write recorded while the texture already sits in CopyDest (no in-list
            // transition) still carries an expected state of CopyDest.
            ok = Check(list.Begin() && list.TransitionTexture(*texture, RHI::ResourceState::CopySource, kCopyDest)
                && list.End() && SubmitClosed(device, list, token), "move texture to CopyDest") && ok;
            device.ExecutePending();
            Scope<RHI::CommandList> directOwner = device.CreateCommandList(RHI::QueueType::Graphics, "direct");
            ok = Check(directOwner->Begin() && directOwner->WriteTexture(*texture, MakeWrite(region, bytes, 8)) && directOwner->End(),
                "direct write into a texture left in CopyDest") && ok;
            ok = Check(SubmitClosed(device, *directOwner, token), "direct write submits while the live state matches") && ok;
            device.ExecutePending();
            oracle.Write(0, 6, region);
            ok = Check(oracle.Matches(*texture), "direct write landed") && ok;
        }

        // Non-graphics queues are rejected before any recording.
        {
            Scope<RHI::CommandList> computeOwner = device.CreateCommandList(RHI::QueueType::Compute, "compute");
            const Rect region { 0, 0, 1, 1 };
            std::vector<u8> bytes = MakeSource(7, region, 4);
            ok = Check(computeOwner->Begin() && computeOwner->TransitionTexture(*texture, kCopyDest)
                && !computeOwner->WriteTexture(*texture, MakeWrite(region, bytes, 4)), "write on a compute-queue list is rejected") && ok;
        }

        ok = Check(device.WaitCalls == 0, "no write path called WaitIdle, WaitForCompletion, or SubmitAndWait") && ok;
        return ok;
    }

    bool TestRhiTextureWriteDefaultsRejectWithoutBackendSupport()
    {
        bool ok = true;
        FakeDevice unsupportedDevice(false);
        ok = Check(!unsupportedDevice.SupportsTextureWrite(), "device that opts out reports no support") && ok;

        // A list that never overrides the new virtuals must compile and reject.
        class LegacyList final : public RHI::CommandList
        {
        public:
            RHI::QueueType GetQueueType() const override { return RHI::QueueType::Graphics; }
            bool Begin() override { return true; }
            bool End() override { return true; }
            void BeginDebugMarker(std::string_view) override {}
            void EndDebugMarker() override {}
            bool BindViewportOutputs(RHI::Texture&, RHI::Texture*) override { return false; }
            bool ClearViewportOutputs(const RHI::ViewportClear&) override { return false; }
            bool TransitionTexture(RHI::Texture&, RHI::ResourceState) override { ++Transitions; return true; }
            bool TransitionBuffer(RHI::Buffer&, RHI::ResourceState) override { return false; }
            void SetGraphicsPipeline(RHI::Pipeline&) override {}
            void SetGraphicsConstantBuffer(u32, RHI::Buffer&) override {}
            void SetViewport(const RHI::Viewport&) override {}
            void SetScissorRect(const RHI::ScissorRect&) override {}
            void SetVertexBuffer(u32, RHI::Buffer&) override {}
            void SetIndexBuffer(RHI::Buffer&, RHI::IndexFormat) override {}
            bool CopyBuffer(RHI::Buffer&, u64, RHI::Buffer&, u64, u64) override { return false; }
            void DrawIndexed(u32, u32, u32, int, u32) override {}
            bool ResetQueryPool(RHI::QueryPool&, u32, u32) override { return false; }
            bool WriteTimestamp(RHI::QueryPool&, u32) override { return false; }
            bool ResolveQueryPool(RHI::QueryPool&, u32, u32) override { return false; }
            u32 Transitions = 0;
        };
        LegacyList legacy;
        Scope<RHI::Texture> created = unsupportedDevice.CreateTexture(MakeDescription(4, 4));
        const Rect whole { 0, 0, 4, 4 };
        std::vector<u8> bytes = MakeSource(0, whole, 16);
        ok = Check(!legacy.WriteTexture(*created, MakeWrite(whole, bytes, 16)), "default WriteTexture rejects") && ok;
        ok = Check(!RHI::RecordTextureWrite(legacy, *created, MakeWrite(whole, bytes, 16)),
            "bracket helper fails closed on a list without explicit expected-state transitions") && ok;
        ok = Check(legacy.Transitions == 0, "helper recorded no legacy transition before failing") && ok;

        // A fake list that opted out of the capability rejects even a valid write.
        Scope<RHI::CommandList> optedOut = unsupportedDevice.CreateCommandList(RHI::QueueType::Graphics, "opted out");
        ok = Check(optedOut->Begin() && optedOut->TransitionTexture(*created, kCopyDest)
            && !optedOut->WriteTexture(*created, MakeWrite(whole, bytes, 16)), "opted-out list rejects a valid write") && ok;
        return ok;
    }
}
