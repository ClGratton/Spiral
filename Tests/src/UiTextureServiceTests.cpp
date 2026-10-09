#include "UiTextureServiceTests.h"

#include "Engine/Renderer/UiTextureService.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    using namespace Engine;
    namespace RHI = Engine::RHI;

    bool Check(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "UI texture service test failed: " << message << '\n';
        return condition;
    }

    constexpr u8 kUninitializedByte = 0xAB;

    // ---- Fake device -------------------------------------------------------

    class FakeUiDevice;

    class FakeUiTexture final : public RHI::Texture
    {
    public:
        FakeUiTexture(RHI::TextureDescription description, FakeUiDevice& owner);
        ~FakeUiTexture() override;

        const RHI::TextureDescription& GetDescription() const override { return m_Description; }

        // Filled with a sentinel so a missing or partial initial write shows.
        std::vector<u8> Pixels;
        RHI::ResourceState State = RHI::ResourceState::Unknown;
        // Models a never-submitted image, whose native layout is undefined: only
        // the two-argument transition may start it.
        bool NeverSubmitted = true;

    private:
        RHI::TextureDescription m_Description;
        FakeUiDevice& m_Owner;
    };

    struct RecordedWrite
    {
        FakeUiTexture* Texture = nullptr;
        RHI::TextureWrite Write;
        std::vector<u8> PackedRows;
    };

    class FakeUiCommandList;

    class FakeUiDevice final : public RHI::Device
    {
    public:
        const RHI::DeviceDescription& GetDescription() const override { return m_Description; }
        const RHI::DeviceCapabilities& GetCapabilities() const override { return m_Capabilities; }
        Scope<RHI::Buffer> CreateBuffer(const RHI::BufferDescription&) override { return nullptr; }
        Scope<RHI::Texture> CreateTexture(const RHI::TextureDescription& description) override
        {
            if (FailCreateTexture)
                return nullptr;
            ++TexturesCreated;
            return CreateScope<FakeUiTexture>(description, *this);
        }
        bool OwnsResource(const RHI::Buffer*) const override { return false; }
        bool OwnsResource(const RHI::Texture* resource) const override { return IsAlive(resource); }
        bool QueryResourceState(const RHI::Buffer*, RHI::ResourceState&) const override { return false; }
        bool QueryResourceState(const RHI::Texture* resource, RHI::ResourceState& state) const override
        {
            const auto* texture = dynamic_cast<const FakeUiTexture*>(resource);
            if (!texture || !IsAlive(resource))
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
        bool SupportsTextureWrite() const override { return Writable; }

        RHI::CompletionToken Submit(RHI::CommandList& commandList) override;
        RHI::CompletionStatus QueryCompletion(const RHI::CompletionToken& token) override
        {
            if (token.DeviceId != kDeviceId)
                return RHI::CompletionStatus::Invalid;
            const auto found = m_Submissions.find(token.SubmissionId);
            if (found == m_Submissions.end())
                return RHI::CompletionStatus::Invalid;
            if (found->second.Failed)
                return RHI::CompletionStatus::Failed;
            return found->second.Executed ? RHI::CompletionStatus::Complete : RHI::CompletionStatus::Incomplete;
        }
        // The service must never reach any of these.
        bool WaitForCompletion(const RHI::CompletionToken&, u32) override { ++WaitCalls; return false; }
        bool SubmitAndWait(RHI::CommandList&) override { ++WaitCalls; return false; }
        void WaitIdle() override { ++WaitCalls; }

        // Test-controlled "GPU".
        void ExecuteAll()
        {
            for (auto& [id, submission] : m_Submissions)
            {
                (void)id;
                Execute(submission);
            }
        }
        bool ExecuteOldest()
        {
            for (auto& [id, submission] : m_Submissions)
            {
                (void)id;
                if (submission.Executed || submission.Failed)
                    continue;
                Execute(submission);
                return true;
            }
            return false;
        }
        bool FailOldest()
        {
            for (auto& [id, submission] : m_Submissions)
            {
                (void)id;
                if (submission.Executed || submission.Failed)
                    continue;
                submission.Failed = true;
                submission.Writes.clear();
                return true;
            }
            return false;
        }
        size_t IncompleteCount() const
        {
            size_t count = 0;
            for (const auto& [id, submission] : m_Submissions)
            {
                (void)id;
                count += submission.Executed || submission.Failed ? 0 : 1;
            }
            return count;
        }
        RHI::CompletionToken LastToken() const { return m_LastToken; }
        bool IsAlive(const RHI::Texture* texture) const
        {
            return std::find(m_Alive.begin(), m_Alive.end(), texture) != m_Alive.end();
        }
        void NoteCreated(const RHI::Texture* texture) { m_Alive.push_back(texture); }
        void NoteDestroyed(const RHI::Texture* texture)
        {
            m_Alive.erase(std::remove(m_Alive.begin(), m_Alive.end(), texture), m_Alive.end());
            ++TexturesDestroyed;
        }

        static constexpr u64 kDeviceId = 91;
        bool Writable = true;
        bool FailCreateTexture = false;
        bool FailCreateList = false;
        bool FailRecordNext = false;
        bool FailSubmitNext = false;
        u32 WaitCalls = 0;
        u32 TexturesCreated = 0;
        u32 TexturesDestroyed = 0;
        u32 ListsCreated = 0;
        u32 ListsAlive = 0;
        u32 SubmitCount = 0;

    private:
        struct Submission
        {
            std::vector<RecordedWrite> Writes;
            bool Executed = false;
            bool Failed = false;
        };

        friend class FakeUiCommandList;

        static void Execute(Submission& submission)
        {
            if (submission.Executed || submission.Failed)
                return;
            for (const RecordedWrite& recorded : submission.Writes)
            {
                const RHI::Extent2D extent = recorded.Texture->GetDescription().Extent;
                const size_t tightRow = static_cast<size_t>(recorded.Write.Extent.Width) * 4u;
                for (u32 row = 0; row < recorded.Write.Extent.Height; ++row)
                    std::copy_n(recorded.PackedRows.begin() + static_cast<std::ptrdiff_t>(row * tightRow), tightRow,
                        recorded.Texture->Pixels.begin()
                            + static_cast<std::ptrdiff_t>((static_cast<size_t>(recorded.Write.Y + row) * extent.Width + recorded.Write.X) * 4u));
            }
            submission.Writes.clear();
            submission.Executed = true;
        }

        RHI::DeviceDescription m_Description;
        RHI::DeviceCapabilities m_Capabilities;
        u64 m_NextSubmission = 1;
        RHI::CompletionToken m_LastToken;
        std::map<u64, Submission> m_Submissions;
        std::vector<const RHI::Texture*> m_Alive;
    };

    FakeUiTexture::FakeUiTexture(RHI::TextureDescription description, FakeUiDevice& owner)
        : m_Description(std::move(description)), m_Owner(owner)
    {
        State = m_Description.InitialState;
        Pixels.assign(static_cast<size_t>(m_Description.Extent.Width) * m_Description.Extent.Height * 4u, kUninitializedByte);
        m_Owner.NoteCreated(this);
    }

    FakeUiTexture::~FakeUiTexture()
    {
        m_Owner.NoteDestroyed(this);
    }

    class FakeUiCommandList final : public RHI::CommandList
    {
    public:
        explicit FakeUiCommandList(FakeUiDevice& device) : m_Device(device) { ++m_Device.ListsAlive; }
        ~FakeUiCommandList() override { --m_Device.ListsAlive; }

        RHI::QueueType GetQueueType() const override { return RHI::QueueType::Graphics; }
        bool Begin() override
        {
            if (m_State == State::Recording)
                return false;
            if (m_State == State::Submitted && m_Device.QueryCompletion(m_LastToken) != RHI::CompletionStatus::Complete)
                return false;
            m_State = State::Recording;
            m_Staged.clear();
            m_Writes.clear();
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
            auto* fake = dynamic_cast<FakeUiTexture*>(&texture);
            if (m_State != State::Recording || !fake || destination == RHI::ResourceState::Unknown)
                return false;
            if (CurrentState(*fake) != destination)
                Stage(*fake).State = destination;
            return true;
        }
        bool TransitionTexture(RHI::Texture& texture, RHI::ResourceState expectedBefore, RHI::ResourceState destination) override
        {
            auto* fake = dynamic_cast<FakeUiTexture*>(&texture);
            if (m_State != State::Recording || !fake || expectedBefore == RHI::ResourceState::Unknown
                || destination == RHI::ResourceState::Unknown)
                return false;
            // A texture no accepted submission has touched has no defined layout
            // for an explicit expected-before state to describe.
            if (fake->NeverSubmitted && m_Staged.find(fake) == m_Staged.end())
                return false;
            StagedState& staged = Stage(*fake);
            if (staged.Expected == RHI::ResourceState::Unknown)
                staged.Expected = expectedBefore;
            staged.State = destination;
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
            auto* fake = dynamic_cast<FakeUiTexture*>(&texture);
            RHI::TextureWritePlan plan;
            if (m_Device.FailRecordNext)
            {
                m_Device.FailRecordNext = false;
                return false;
            }
            if (m_State != State::Recording || !fake || !m_Device.IsAlive(&texture)
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
                m_Staged[fake] = { RHI::ResourceState::CopyDest, RHI::ResourceState::CopyDest };
            return true;
        }

        bool Ready() const { return m_State == State::Closed; }
        void MarkSubmitted(const RHI::CompletionToken& token)
        {
            m_LastToken = token;
            m_State = State::Submitted;
        }
        std::vector<RecordedWrite> TakeWrites() { return std::move(m_Writes); }

        struct StagedState
        {
            RHI::ResourceState State = RHI::ResourceState::Unknown;
            RHI::ResourceState Expected = RHI::ResourceState::Unknown;
        };
        const std::map<FakeUiTexture*, StagedState>& Staged() const { return m_Staged; }

    private:
        enum class State { Ready, Recording, Closed, Submitted };

        StagedState& Stage(FakeUiTexture& texture)
        {
            auto found = m_Staged.find(&texture);
            if (found == m_Staged.end())
                found = m_Staged.emplace(&texture, StagedState { texture.State, RHI::ResourceState::Unknown }).first;
            return found->second;
        }
        RHI::ResourceState CurrentState(const FakeUiTexture& texture) const
        {
            const auto found = m_Staged.find(const_cast<FakeUiTexture*>(&texture));
            return found == m_Staged.end() ? texture.State : found->second.State;
        }

        FakeUiDevice& m_Device;
        State m_State = State::Ready;
        RHI::CompletionToken m_LastToken;
        std::map<FakeUiTexture*, StagedState> m_Staged;
        std::vector<RecordedWrite> m_Writes;
    };

    Scope<RHI::CommandList> FakeUiDevice::CreateCommandList(RHI::QueueType, std::string_view)
    {
        if (FailCreateList)
            return nullptr;
        ++ListsCreated;
        return CreateScope<FakeUiCommandList>(*this);
    }

    RHI::CompletionToken FakeUiDevice::Submit(RHI::CommandList& commandList)
    {
        auto* list = dynamic_cast<FakeUiCommandList*>(&commandList);
        if (FailSubmitNext)
        {
            FailSubmitNext = false;
            return {};
        }
        if (!list || !list->Ready())
            return {};
        for (const auto& [texture, staged] : list->Staged())
            if (staged.Expected != RHI::ResourceState::Unknown && texture->State != staged.Expected)
                return {};
        for (const auto& [texture, staged] : list->Staged())
        {
            texture->State = staged.State;
            texture->NeverSubmitted = false;
        }
        const RHI::CompletionToken token { kDeviceId, m_NextSubmission++ };
        Submission submission;
        submission.Writes = list->TakeWrites();
        m_Submissions[token.SubmissionId] = std::move(submission);
        m_LastToken = token;
        ++SubmitCount;
        list->MarkSubmitted(token);
        return token;
    }

    // ---- Fake native bridge ------------------------------------------------

    class FakeBridge final : public UiTextureNativeBridge
    {
    public:
        u64 Register(RHI::Texture& texture) override
        {
            if (FailRegister)
                return 0;
            const u64 id = 0x1000u + ++m_Counter;
            m_Registered[id] = &texture;
            ++Registrations;
            return id;
        }
        void Unregister(u64 imGuiId) override
        {
            const auto found = m_Registered.find(imGuiId);
            if (found == m_Registered.end())
            {
                ++UnknownUnregisters;
                return;
            }
            if (IsAlive && !IsAlive(found->second))
                ++UnregisterAfterDestroy;
            m_Registered.erase(found);
            ++Unregistrations;
        }
        u64 ReferenceBoundSerial() override { return Submitted + (FrameOpen ? 1u : 0u); }
        u64 CompletedSerial() override { return Completed; }

        const RHI::Texture* TextureFor(u64 imGuiId) const
        {
            const auto found = m_Registered.find(imGuiId);
            return found == m_Registered.end() ? nullptr : found->second;
        }
        size_t LiveRegistrations() const { return m_Registered.size(); }

        bool FailRegister = false;
        bool FrameOpen = false;
        u64 Submitted = 0;
        u64 Completed = 0;
        u32 Registrations = 0;
        u32 Unregistrations = 0;
        u32 UnknownUnregisters = 0;
        u32 UnregisterAfterDestroy = 0;
        std::function<bool(const RHI::Texture*)> IsAlive;

    private:
        u64 m_Counter = 0;
        std::map<u64, RHI::Texture*> m_Registered;
    };

    // ---- Harness and oracle ------------------------------------------------

    struct Harness
    {
        Harness()
            : Service(CreateScope<UiTextureService>(Device, Bridge))
        {
            Bridge.IsAlive = [this](const RHI::Texture* texture) { return Device.IsAlive(texture); };
        }

        FakeUiDevice Device;
        FakeBridge Bridge;
        Scope<UiTextureService> Service;
    };

    struct Lcg
    {
        explicit Lcg(u64 seed) : State(seed) {}
        u32 Next()
        {
            State = State * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<u32>(State >> 33);
        }
        u32 Below(u32 count) { return count == 0 ? 0 : Next() % count; }
        u64 State;
    };

    // Everything the fake device and bridge saw must agree with the service's
    // own accounting at every observation point.
    bool AccountingConsistent(Harness& harness, std::string_view where)
    {
        const UiTextureCounters counters = harness.Service->GetCounters();
        const size_t outstanding = counters.LiveTextures + counters.PendingRetirements;
        bool ok = Check(counters.IsBalanced(), std::string(where) + ": counters balanced");
        ok = Check(harness.Device.TexturesCreated == counters.GpuTexturesCreated, std::string(where) + ": GPU texture creations match") && ok;
        ok = Check(harness.Device.TexturesDestroyed == counters.GpuTexturesReleased, std::string(where) + ": GPU texture releases match") && ok;
        ok = Check(harness.Bridge.Registrations == counters.NativeRegistrations
                && harness.Bridge.Unregistrations == counters.NativeUnregistrations,
            std::string(where) + ": registrations match") && ok;
        ok = Check(harness.Bridge.LiveRegistrations() == outstanding, std::string(where) + ": live registrations equal outstanding textures") && ok;
        ok = Check(harness.Bridge.UnknownUnregisters == 0 && harness.Bridge.UnregisterAfterDestroy == 0,
            std::string(where) + ": registration removed exactly once and before its texture died") && ok;
        ok = Check(harness.Device.WaitCalls == 0, std::string(where) + ": the service never waits for the GPU") && ok;
        return ok;
    }

    FakeUiTexture* TextureOf(Harness& harness, UiTextureHandle handle)
    {
        return const_cast<FakeUiTexture*>(dynamic_cast<const FakeUiTexture*>(
            harness.Bridge.TextureFor(harness.Service->GetImGuiId(handle))));
    }

    UiTextureUpdate WholeUpdate(const std::vector<u8>& bytes, u32 width, u32 height)
    {
        UiTextureUpdate update;
        update.Rect = { 0, 0, width, height };
        update.Pixels = bytes.data();
        update.PixelBytes = bytes.size();
        return update;
    }

    std::vector<u8> Solid(u32 width, u32 height, u8 value)
    {
        return std::vector<u8>(static_cast<size_t>(width) * height * 4u, value);
    }
}

namespace SpiralTests
{
    bool TestUiTextureServiceHandleLifecycleAndLimits()
    {
        auto harness = CreateScope<Harness>();
        UiTextureService& service = *harness->Service;
        bool ok = true;

        ok = Check(service.Create(0, 0, "zero") == kInvalidUiTextureHandle && service.GetLastError() == UiTextureError::InvalidSize, "zero size rejected") && ok;
        ok = Check(service.Create(kMaximumUiTextureDimension + 1, 4, "wide") == kInvalidUiTextureHandle
                && service.GetLastError() == UiTextureError::InvalidSize, "oversize width rejected") && ok;
        ok = Check(service.Create(4, kMaximumUiTextureDimension + 1, "tall") == kInvalidUiTextureHandle, "oversize height rejected") && ok;
        ok = Check(harness->Device.TexturesCreated == 0 && harness->Bridge.Registrations == 0, "size rejection creates nothing") && ok;

        const UiTextureHandle first = service.Create(8, 6, "first");
        ok = Check(first != kInvalidUiTextureHandle && service.GetLastError() == UiTextureError::None, "create succeeds") && ok;
        u32 width = 0;
        u32 height = 0;
        ok = Check(service.GetExtent(first, width, height) && width == 8 && height == 6, "extent reported") && ok;
        const u64 firstId = service.GetImGuiId(first);
        ok = Check(firstId != 0 && harness->Bridge.TextureFor(firstId) != nullptr, "ImGui id registered with the bridge") && ok;

        // Create returned without a completion wait: the zero-fill is still queued.
        FakeUiTexture* firstTexture = TextureOf(*harness, first);
        ok = Check(firstTexture && harness->Device.IncompleteCount() == 1
                && std::all_of(firstTexture->Pixels.begin(), firstTexture->Pixels.end(), [](u8 value) { return value == kUninitializedByte; }),
            "create submits the initial write without waiting for it") && ok;
        harness->Device.ExecuteAll();
        ok = Check(firstTexture && std::all_of(firstTexture->Pixels.begin(), firstTexture->Pixels.end(), [](u8 value) { return value == 0; }),
            "a new texture is transparent black once its first write retires") && ok;
        ok = Check(firstTexture && firstTexture->State == RHI::ResourceState::ShaderResource && !firstTexture->NeverSubmitted,
            "a new texture settles in its ShaderResource steady state through the first-use bracket") && ok;

        const UiTextureHandle second = service.Create(4, 4, "second");
        ok = Check(second != kInvalidUiTextureHandle && second != first && service.GetImGuiId(second) != firstId, "handles and ids are distinct") && ok;

        // Stale handle after destroy, and slot reuse never revives it.
        ok = Check(service.Destroy(first), "destroy succeeds") && ok;
        ok = Check(service.GetImGuiId(first) == 0 && !service.GetExtent(first, width, height), "destroyed handle resolves to nothing") && ok;
        ok = Check(!service.Update(first, WholeUpdate(Solid(8, 6, 1), 8, 6)) && service.GetLastError() == UiTextureError::InvalidHandle,
            "update through a destroyed handle is rejected") && ok;
        ok = Check(!service.Destroy(first) && service.GetLastError() == UiTextureError::InvalidHandle, "double destroy is rejected") && ok;
        ok = Check(!service.Resize(first, 3, 3) && service.GetLastError() == UiTextureError::InvalidHandle, "resize through a destroyed handle is rejected") && ok;
        const UiTextureHandle reused = service.Create(5, 5, "reused-slot");
        ok = Check(reused != kInvalidUiTextureHandle && reused != first, "slot reuse issues a fresh generation") && ok;
        ok = Check(service.GetImGuiId(first) == 0 && !service.Destroy(first), "the old handle stays dead after its slot is reused") && ok;
        ok = Check(service.GetExtent(reused, width, height) && width == 5 && height == 5, "reused slot holds the new texture") && ok;

        // Forged handles.
        const UiTextureHandle forged[] = { kInvalidUiTextureHandle, ~0ull, 0x1'0000'0000ull, (1ull << 32) | 999u,
            (static_cast<u64>(77) << 32) | (second & 0xFFFFFFFFull), second ^ (1ull << 32) };
        for (const UiTextureHandle handle : forged)
        {
            ok = Check(service.GetImGuiId(handle) == 0 && !service.GetExtent(handle, width, height) && !service.Destroy(handle)
                    && !service.Resize(handle, 2, 2) && !service.Update(handle, WholeUpdate(Solid(4, 4, 1), 4, 4)),
                "forged handle is rejected everywhere") && ok;
        }
        ok = Check(service.GetImGuiId(second) != 0, "forged-handle probing disturbed nothing") && ok;
        ok = AccountingConsistent(*harness, "lifecycle") && ok;
        ok = Check(service.Destroy(second) && service.Destroy(reused), "cleanup destroy") && ok;

        // The limit counts live textures and textures still awaiting retirement.
        Harness limited;
        UiTextureService& crowded = *limited.Service;
        limited.Bridge.Submitted = 1;
        limited.Bridge.Completed = 0;
        std::vector<UiTextureHandle> handles;
        for (u32 index = 0; index < kMaximumRegisteredUiTextures; ++index)
        {
            const UiTextureHandle handle = crowded.Create(2, 2, "limit");
            limited.Device.ExecuteAll();
            if (handle == kInvalidUiTextureHandle)
                break;
            handles.push_back(handle);
        }
        ok = Check(handles.size() == kMaximumRegisteredUiTextures, "creates up to the limit") && ok;
        const u32 createdAtLimit = limited.Device.TexturesCreated;
        const u32 registrationsAtLimit = limited.Bridge.Registrations;
        ok = Check(crowded.Create(2, 2, "over") == kInvalidUiTextureHandle && crowded.GetLastError() == UiTextureError::TextureLimit,
            "creation beyond the limit is rejected") && ok;
        ok = Check(limited.Device.TexturesCreated == createdAtLimit && limited.Bridge.Registrations == registrationsAtLimit,
            "a limit rejection creates and registers nothing") && ok;
        ok = Check(crowded.Destroy(handles.back()), "destroy one at the limit") && ok;
        ok = Check(crowded.Create(2, 2, "still-over") == kInvalidUiTextureHandle && crowded.GetLastError() == UiTextureError::TextureLimit,
            "a pending retirement still occupies its slot while the presentation has not finished") && ok;
        limited.Bridge.Completed = 1;
        const UiTextureHandle afterReclaim = crowded.Create(2, 2, "after-reclaim");
        ok = Check(afterReclaim != kInvalidUiTextureHandle && crowded.GetCounters().RetirementsReleased == 1,
            "creation reclaims capacity from a completed retirement") && ok;
        ok = AccountingConsistent(limited, "limit") && ok;
        ok = AccountingConsistent(*harness, "lifecycle-tail") && ok;
        return ok;
    }

    bool TestUiTextureServiceDirtyRectContentMatchesIndependentOracle()
    {
        bool ok = true;
        for (const u64 seed : { 1ull, 0x5EEDull, 0xC0FFEEull })
        {
            auto harness = CreateScope<Harness>();
            UiTextureService& service = *harness->Service;
            Lcg random(seed);
            u32 width = 37;
            u32 height = 23;
            const UiTextureHandle handle = service.Create(width, height, "oracle");
            if (!Check(handle != kInvalidUiTextureHandle, "oracle create"))
                return false;
            // Independent expected image; the service never sees it.
            std::vector<u8> expected(static_cast<size_t>(width) * height * 4u, 0);
            u64 acceptedBytes = 0;
            u32 backpressure = 0;
            u32 accepted = 0;
            bool seedOk = true;

            for (u32 iteration = 0; iteration < 400 && seedOk; ++iteration)
            {
                if (iteration == 150)
                {
                    // Resize mid-stream: contents restart transparent at the new size.
                    harness->Device.ExecuteAll();
                    width = 20;
                    height = 31;
                    seedOk = Check(service.Resize(handle, width, height), "oracle resize");
                    expected.assign(static_cast<size_t>(width) * height * 4u, 0);
                }

                UiTextureRect rect;
                rect.X = random.Below(width);
                rect.Y = random.Below(height);
                rect.Width = 1 + random.Below(width - rect.X);
                rect.Height = 1 + random.Below(height - rect.Y);
                const u32 padding = random.Below(3) == 0 ? 0 : random.Below(40);
                const u64 pitch = static_cast<u64>(rect.Width) * 4u + padding;
                const u64 leadingBytes = random.Below(64);
                std::vector<u8> memory(static_cast<size_t>(leadingBytes + pitch * rect.Height + random.Below(9)), 0x5A);
                for (u32 row = 0; row < rect.Height; ++row)
                    for (u32 byte = 0; byte < rect.Width * 4u; ++byte)
                        memory[static_cast<size_t>(leadingBytes + row * pitch + byte)] = static_cast<u8>(random.Next());

                UiTextureUpdate update;
                update.Rect = rect;
                update.Pixels = memory.data() + leadingBytes;
                update.RowPitchBytes = padding == 0 && random.Below(2) == 0 ? 0 : pitch;
                update.PixelBytes = memory.size() - leadingBytes;
                bool updated = service.Update(handle, update);
                if (!updated && service.GetLastError() == UiTextureError::UpdateBackpressure)
                {
                    ++backpressure;
                    seedOk = Check(harness->Device.ExecuteOldest(), "backpressure implies an incomplete submission");
                    updated = service.Update(handle, update);
                }
                seedOk = Check(updated, "oracle update accepted") && seedOk;
                if (!updated)
                    break;
                ++accepted;
                acceptedBytes += static_cast<u64>(rect.Width) * rect.Height * 4u;
                for (u32 row = 0; row < rect.Height; ++row)
                    std::copy_n(memory.begin() + static_cast<std::ptrdiff_t>(leadingBytes + row * pitch),
                        static_cast<std::ptrdiff_t>(rect.Width) * 4,
                        expected.begin() + static_cast<std::ptrdiff_t>((static_cast<size_t>(rect.Y + row) * width + rect.X) * 4u));
                // The service copied the pixels while recording.
                std::fill(memory.begin(), memory.end(), 0xEE);

                if (random.Below(5) == 0)
                    harness->Device.ExecuteOldest();
                if (random.Below(23) == 0)
                    harness->Device.ExecuteAll();
            }

            harness->Device.ExecuteAll();
            FakeUiTexture* texture = TextureOf(*harness, handle);
            seedOk = Check(texture && texture->Pixels == expected, "pixels equal the independent oracle after replaying all writes") && seedOk;
            const UiTextureCounters counters = service.GetCounters();
            seedOk = Check(counters.UpdatesSubmitted == accepted && counters.UpdateBytes == acceptedBytes, "update counters equal the oracle's tally") && seedOk;
            seedOk = Check(backpressure > 0, "the bounded command-list pool pushed back at least once") && seedOk;
            seedOk = Check(harness->Device.ListsCreated <= kMaximumUiTextureCommandLists && counters.CommandListReuses > 0,
                "command lists are pooled, bounded, and reused") && seedOk;
            seedOk = AccountingConsistent(*harness, "oracle") && seedOk;
            if (!seedOk)
                std::cerr << "UI texture service oracle failure, seed=" << seed << '\n';
            ok = seedOk && ok;
        }
        return ok;
    }

    bool TestUiTextureServiceDeferredRetirementWaitsForWriteTokensAndPresentation()
    {
        bool ok = true;
        const auto aliveAndRegistered = [](Harness& harness, const RHI::Texture* texture, u64 id)
        {
            return harness.Device.IsAlive(texture) && harness.Bridge.TextureFor(id) == texture;
        };

        // (a) Closed frame: the bound is the last issued serial.
        {
            Harness harness;
            UiTextureService& service = *harness.Service;
            harness.Bridge.Submitted = 5;
            harness.Bridge.Completed = 5;
            const UiTextureHandle handle = service.Create(6, 6, "a");
            const u64 id = service.GetImGuiId(handle);
            const RHI::Texture* texture = harness.Bridge.TextureFor(id);
            const std::vector<u8> bytes = Solid(6, 6, 9);
            ok = Check(service.Update(handle, WholeUpdate(bytes, 6, 6)), "a: update") && ok;
            harness.Bridge.Submitted = 7;
            ok = Check(service.Destroy(handle), "a: destroy") && ok;
            ok = Check(service.CollectRetired() == 0 && aliveAndRegistered(harness, texture, id), "a: held while writes and presentation are both incomplete") && ok;
            harness.Device.ExecuteAll();
            harness.Bridge.Completed = 6;
            ok = Check(service.CollectRetired() == 0 && aliveAndRegistered(harness, texture, id), "a: held by presentation serial 6 < bound 7 although the write token completed") && ok;
            harness.Bridge.Completed = 7;
            ok = Check(service.CollectRetired() == 1 && !harness.Device.IsAlive(texture) && harness.Bridge.TextureFor(id) == nullptr,
                "a: released once the write token and presentation serial 7 are complete") && ok;
            const UiTextureCounters counters = service.GetCounters();
            ok = Check(counters.RetirementHoldsWriteToken >= 1 && counters.RetirementHoldsPresentation >= 2 && counters.RetirementsReleased == 1,
                "a: hold reasons were counted separately") && ok;
            ok = AccountingConsistent(harness, "retire-a") && ok;
        }

        // (b) Open frame: the frame's own submission may already name the texture.
        {
            Harness harness;
            UiTextureService& service = *harness.Service;
            harness.Bridge.Submitted = 9;
            harness.Bridge.Completed = 9;
            const UiTextureHandle handle = service.Create(4, 4, "b");
            harness.Device.ExecuteAll();
            harness.Bridge.FrameOpen = true;
            const u64 id = service.GetImGuiId(handle);
            const RHI::Texture* texture = harness.Bridge.TextureFor(id);
            ok = Check(service.Destroy(handle), "b: destroy in an open frame") && ok;
            ok = Check(service.CollectRetired() == 0 && aliveAndRegistered(harness, texture, id),
                "b: everything before the open frame's submission has completed, which is not enough") && ok;
            harness.Bridge.FrameOpen = false;
            harness.Bridge.Submitted = 10;
            harness.Bridge.Completed = 10;
            ok = Check(service.CollectRetired() == 1 && !harness.Device.IsAlive(texture), "b: released after the open frame's submission completes") && ok;
            ok = AccountingConsistent(harness, "retire-b") && ok;
        }

        // (c) Presentation done but a write token outstanding; (d) failed token is terminal.
        {
            Harness harness;
            UiTextureService& service = *harness.Service;
            const UiTextureHandle held = service.Create(3, 3, "c");
            const UiTextureHandle failing = service.Create(3, 3, "d");
            const RHI::Texture* heldTexture = harness.Bridge.TextureFor(service.GetImGuiId(held));
            const RHI::Texture* failingTexture = harness.Bridge.TextureFor(service.GetImGuiId(failing));
            ok = Check(service.Destroy(held) && service.Destroy(failing), "c: destroy both") && ok;
            ok = Check(service.CollectRetired() == 0 && harness.Device.IsAlive(heldTexture) && harness.Device.IsAlive(failingTexture),
                "c: held by incomplete write tokens with the presentation already complete") && ok;
            ok = Check(harness.Device.ExecuteOldest(), "c: first token completes") && ok;
            ok = Check(service.CollectRetired() == 1 && !harness.Device.IsAlive(heldTexture) && harness.Device.IsAlive(failingTexture),
                "c: only the texture whose token completed is released") && ok;
            ok = Check(harness.Device.FailOldest(), "d: second token fails") && ok;
            ok = Check(service.CollectRetired() == 1 && !harness.Device.IsAlive(failingTexture),
                "d: a failed token is terminal and releases the texture") && ok;
            ok = AccountingConsistent(harness, "retire-cd") && ok;
        }

        // (e) Resize retires only the old texture; (f) partial release by bound; (h) later update extends the hold.
        {
            Harness harness;
            UiTextureService& service = *harness.Service;
            harness.Bridge.Submitted = 2;
            harness.Bridge.Completed = 2;
            const UiTextureHandle handle = service.Create(8, 8, "e");
            harness.Device.ExecuteAll();
            const u64 oldId = service.GetImGuiId(handle);
            const RHI::Texture* oldTexture = harness.Bridge.TextureFor(oldId);
            ok = Check(service.Resize(handle, 16, 4), "e: resize") && ok;
            const u64 newId = service.GetImGuiId(handle);
            const RHI::Texture* newTexture = harness.Bridge.TextureFor(newId);
            u32 width = 0;
            u32 height = 0;
            ok = Check(newId != 0 && newId != oldId && newTexture != oldTexture && service.GetExtent(handle, width, height) && width == 16 && height == 4,
                "e: the handle stays valid with a new texture, id and extent") && ok;
            ok = Check(harness.Device.IsAlive(oldTexture) && harness.Bridge.TextureFor(oldId) == oldTexture, "e: old texture and registration live until retirement") && ok;
            ok = Check(service.GetCounters().Resized == 1 && service.GetCounters().PendingRetirements == 1, "e: exactly the old texture is pending") && ok;
            ok = Check(service.Resize(handle, 16, 4) && service.GetImGuiId(handle) == newId && service.GetCounters().Resized == 1,
                "e: a same-size resize changes nothing") && ok;

            // The resized handle is writable and the oracle sees the transparent restart.
            const std::vector<u8> bytes = Solid(16, 4, 0x77);
            ok = Check(service.Update(handle, WholeUpdate(bytes, 16, 4)), "e: update after resize") && ok;
            harness.Bridge.Submitted = 3;
            harness.Device.ExecuteAll();
            const UiTextureHandle other = service.Create(2, 2, "f");
            harness.Bridge.Submitted = 4;
            ok = Check(service.Destroy(other), "f: second retirement with a later bound") && ok;
            harness.Bridge.Completed = 3;
            // Old texture bound 2 is complete; `other` has bound 4.
            ok = Check(service.CollectRetired() == 1 && !harness.Device.IsAlive(oldTexture), "f: only the retirement whose bound is complete is released") && ok;
            ok = Check(service.GetCounters().PendingRetirements == 1 && service.GetCounters().PeakPendingRetirements == 2, "f: pending and peak counters track retirements") && ok;
            harness.Bridge.Completed = 4;
            harness.Device.ExecuteAll();
            ok = Check(service.CollectRetired() == 1 && harness.Device.IsAlive(newTexture), "f: remaining retirement released, live texture untouched") && ok;

            // (h) An update submitted after the previous token extends the hold.
            const UiTextureHandle busy = service.Create(2, 2, "h");
            harness.Device.ExecuteAll();
            const RHI::Texture* busyTexture = harness.Bridge.TextureFor(service.GetImGuiId(busy));
            const std::vector<u8> busyBytes = Solid(2, 2, 3);
            ok = Check(service.Update(busy, WholeUpdate(busyBytes, 2, 2)) && service.Destroy(busy), "h: update then destroy") && ok;
            ok = Check(service.CollectRetired() == 0 && harness.Device.IsAlive(busyTexture), "h: the most recent write token governs the hold") && ok;
            harness.Device.ExecuteAll();
            ok = Check(service.CollectRetired() == 1, "h: released after the most recent write completes") && ok;
            ok = AccountingConsistent(harness, "retire-efh") && ok;
        }
        return ok;
    }

    bool TestUiTextureServiceFailuresAreAtomicAndNeverRegisterPartialTextures()
    {
        bool ok = true;
        const auto untouched = [](Harness& harness, const char* where)
        {
            const UiTextureCounters counters = harness.Service->GetCounters();
            return Check(counters.LiveTextures == 0 && counters.PendingRetirements == 0 && harness.Bridge.LiveRegistrations() == 0
                    && harness.Device.TexturesCreated == harness.Device.TexturesDestroyed && counters.IsBalanced(),
                where);
        };

        // Create failures leave no texture, registration, or live handle.
        {
            Harness harness;
            harness.Device.Writable = false;
            ok = Check(harness.Service->Create(4, 4, "x") == kInvalidUiTextureHandle
                    && harness.Service->GetLastError() == UiTextureError::ServiceUnavailable && harness.Device.TexturesCreated == 0,
                "a device without WriteTexture is reported unavailable before any allocation") && ok;
            harness.Device.Writable = true;
            harness.Device.FailCreateTexture = true;
            ok = Check(harness.Service->Create(4, 4, "x") == kInvalidUiTextureHandle
                    && harness.Service->GetLastError() == UiTextureError::DeviceCreateFailed, "device create failure is reported") && ok;
            harness.Device.FailCreateTexture = false;
            harness.Bridge.FailRegister = true;
            ok = Check(harness.Service->Create(4, 4, "x") == kInvalidUiTextureHandle
                    && harness.Service->GetLastError() == UiTextureError::NativeRegistrationFailed, "registration failure is reported") && ok;
            ok = Check(harness.Device.TexturesCreated == 1 && harness.Device.TexturesDestroyed == 1, "the unregistered GPU texture is released immediately") && ok;
            harness.Bridge.FailRegister = false;
            harness.Device.FailRecordNext = true;
            ok = Check(harness.Service->Create(4, 4, "x") == kInvalidUiTextureHandle
                    && harness.Service->GetLastError() == UiTextureError::InitialWriteFailed, "initial write record failure is reported") && ok;
            harness.Device.FailSubmitNext = true;
            ok = Check(harness.Service->Create(4, 4, "x") == kInvalidUiTextureHandle
                    && harness.Service->GetLastError() == UiTextureError::InitialWriteFailed, "initial submit failure is reported") && ok;
            harness.Device.FailCreateList = true;
            ok = Check(harness.Service->Create(4, 4, "x") == kInvalidUiTextureHandle
                    && harness.Service->GetLastError() == UiTextureError::InitialWriteFailed, "command-list creation failure is reported") && ok;
            harness.Device.FailCreateList = false;
            ok = untouched(harness, "every create failure leaves nothing registered, live or pending") && ok;
            ok = Check(harness.Bridge.Registrations == harness.Bridge.Unregistrations, "registrations balance after create failures") && ok;
            ok = Check(harness.Service->GetCounters().Rejected == 6, "each failed call is counted once") && ok;
            // The service stays usable, and a failed list was dropped rather than reused.
            const UiTextureHandle recovered = harness.Service->Create(4, 4, "recovered");
            ok = Check(recovered != kInvalidUiTextureHandle, "creation succeeds again after the failures") && ok;
            ok = AccountingConsistent(harness, "create-failures") && ok;
        }

        // Update failures keep the handle, texture and content.
        {
            Harness harness;
            UiTextureService& service = *harness.Service;
            const UiTextureHandle handle = service.Create(8, 8, "u");
            const std::vector<u8> base = Solid(8, 8, 0x11);
            ok = Check(service.Update(handle, WholeUpdate(base, 8, 8)), "u: base update") && ok;
            harness.Device.ExecuteAll();
            const u32 submitsBefore = harness.Device.SubmitCount;
            const std::vector<u8> bytes = Solid(8, 8, 0x99);
            const auto refused = [&](UiTextureUpdate update, const char* what)
            {
                return Check(!service.Update(handle, update) && service.GetLastError() == UiTextureError::InvalidUpdate, what);
            };
            UiTextureUpdate outOfBounds = WholeUpdate(bytes, 8, 8);
            outOfBounds.Rect.X = 1;
            UiTextureUpdate empty = WholeUpdate(bytes, 8, 8);
            empty.Rect.Height = 0;
            UiTextureUpdate shortData = WholeUpdate(bytes, 8, 8);
            shortData.PixelBytes -= 1;
            UiTextureUpdate shortPitch = WholeUpdate(bytes, 8, 8);
            shortPitch.RowPitchBytes = 8u * 4u - 1u;
            UiTextureUpdate noPixels = WholeUpdate(bytes, 8, 8);
            noPixels.Pixels = nullptr;
            UiTextureUpdate overflow = WholeUpdate(bytes, 8, 8);
            overflow.Rect = { 0xFFFFFFFFu, 0, 2, 1 };
            ok = refused(outOfBounds, "u: rectangle outside the texture") && refused(empty, "u: empty rectangle")
                && refused(shortData, "u: too little readable data") && refused(shortPitch, "u: row pitch below the tight width")
                && refused(noPixels, "u: null pixels") && refused(overflow, "u: rectangle that overflows 32 bits") && ok;
            ok = Check(harness.Device.SubmitCount == submitsBefore, "u: refused updates submit nothing") && ok;

            harness.Device.FailRecordNext = true;
            ok = Check(!service.Update(handle, WholeUpdate(bytes, 8, 8)) && service.GetLastError() == UiTextureError::UpdateRecordFailed, "u: record failure") && ok;
            harness.Device.FailSubmitNext = true;
            ok = Check(!service.Update(handle, WholeUpdate(bytes, 8, 8)) && service.GetLastError() == UiTextureError::UpdateSubmitFailed, "u: submit failure") && ok;
            harness.Device.FailCreateList = true;
            // Pool lists were dropped by the failures above, so a new one is needed.
            ok = Check(!service.Update(handle, WholeUpdate(bytes, 8, 8)) && service.GetLastError() == UiTextureError::UpdateRecordFailed, "u: list creation failure") && ok;
            harness.Device.FailCreateList = false;
            harness.Device.ExecuteAll();
            FakeUiTexture* texture = TextureOf(harness, handle);
            ok = Check(texture && texture->Pixels == base && texture->State == RHI::ResourceState::ShaderResource,
                "u: no failed update changed the content or the steady state") && ok;
            ok = Check(service.GetImGuiId(handle) != 0 && service.GetCounters().UpdatesSubmitted == 1, "u: the handle stays valid and failures are not counted as updates") && ok;
            ok = Check(service.Update(handle, WholeUpdate(bytes, 8, 8)), "u: update works again after the failures") && ok;
            harness.Device.ExecuteAll();
            ok = Check(texture && texture->Pixels == bytes, "u: and lands exactly") && ok;

            // Backpressure: every pooled list busy.
            u32 accepted = 0;
            while (service.Update(handle, WholeUpdate(base, 8, 8)))
                ++accepted;
            ok = Check(service.GetLastError() == UiTextureError::UpdateBackpressure && accepted < kMaximumUiTextureCommandLists + 1,
                "u: update is refused, not delayed, while every command list is busy") && ok;
            ok = Check(harness.Device.WaitCalls == 0 && harness.Device.ListsAlive <= kMaximumUiTextureCommandLists, "u: backpressure neither waits nor grows the pool") && ok;
            harness.Device.ExecuteAll();
            ok = AccountingConsistent(harness, "update-failures") && ok;
        }

        // Resize failures keep the old texture exactly as it was.
        {
            Harness harness;
            UiTextureService& service = *harness.Service;
            const UiTextureHandle handle = service.Create(6, 5, "r");
            const std::vector<u8> bytes = Solid(6, 5, 0x42);
            ok = Check(service.Update(handle, WholeUpdate(bytes, 6, 5)), "r: base update") && ok;
            harness.Device.ExecuteAll();
            const u64 id = service.GetImGuiId(handle);
            const u32 registrationsBefore = harness.Bridge.Registrations;
            const auto unchanged = [&](const char* what)
            {
                u32 width = 0;
                u32 height = 0;
                FakeUiTexture* texture = TextureOf(harness, handle);
                const UiTextureCounters counters = service.GetCounters();
                return Check(service.GetImGuiId(handle) == id && service.GetExtent(handle, width, height) && width == 6 && height == 5
                        && texture && texture->Pixels == bytes && counters.PendingRetirements == 0 && counters.Resized == 0
                        && harness.Bridge.Registrations - harness.Bridge.Unregistrations == 1 && harness.Bridge.LiveRegistrations() == 1,
                    what);
            };
            ok = Check(!service.Resize(handle, 0, 5) && service.GetLastError() == UiTextureError::InvalidSize, "r: zero size") && unchanged("r: zero size leaves the texture") && ok;
            harness.Device.FailCreateTexture = true;
            ok = Check(!service.Resize(handle, 9, 9) && service.GetLastError() == UiTextureError::DeviceCreateFailed, "r: device create failure") && unchanged("r: device failure leaves the texture") && ok;
            harness.Device.FailCreateTexture = false;
            harness.Bridge.FailRegister = true;
            ok = Check(!service.Resize(handle, 9, 9) && service.GetLastError() == UiTextureError::NativeRegistrationFailed, "r: registration failure") && unchanged("r: registration failure leaves the texture") && ok;
            harness.Bridge.FailRegister = false;
            harness.Device.FailRecordNext = true;
            ok = Check(!service.Resize(handle, 9, 9) && service.GetLastError() == UiTextureError::InitialWriteFailed, "r: initial write failure") && unchanged("r: initial write failure leaves the texture") && ok;
            ok = Check(harness.Bridge.Registrations > registrationsBefore && harness.Bridge.Registrations == harness.Bridge.Unregistrations + 1,
                "r: the abandoned replacement was unregistered") && ok;
            ok = Check(service.Resize(handle, 9, 9) && service.GetCounters().Resized == 1, "r: resize succeeds after the failures") && ok;
            ok = AccountingConsistent(harness, "resize-failures") && ok;
        }
        return ok;
    }

    bool TestUiTextureServiceShutdownDrainThreadRulesAndNoStall()
    {
        bool ok = true;

        // Shutdown drains pending retirements and counts live textures as leaks.
        {
            auto harness = CreateScope<Harness>();
            UiTextureService& service = *harness->Service;
            const UiTextureHandle keep = service.Create(4, 4, "keep");
            const UiTextureHandle leak = service.Create(4, 4, "leak");
            const UiTextureHandle gone = service.Create(4, 4, "gone");
            const std::vector<u8> bytes = Solid(4, 4, 1);
            ok = Check(service.Update(keep, WholeUpdate(bytes, 4, 4)), "shutdown: update accepted while the GPU is behind") && ok;
            ok = Check(harness->Device.IncompleteCount() > 0, "shutdown: updates return with the GPU still behind (no stall)") && ok;
            ok = Check(service.Destroy(gone), "shutdown: destroy") && ok;
            ok = Check(keep != leak && service.GetCounters().PendingRetirements == 1, "shutdown: one retirement pending") && ok;
            service.Shutdown();
            const UiTextureCounters counters = service.GetCounters();
            ok = Check(counters.RetirementsDrainedAtShutdown == 1 && counters.LeakedAtShutdown == 2 && counters.IsDrained() && counters.IsBalanced(),
                "shutdown: drained retirement, counted leaks, balanced") && ok;
            ok = Check(harness->Device.TexturesCreated == harness->Device.TexturesDestroyed && harness->Bridge.LiveRegistrations() == 0
                    && harness->Bridge.UnknownUnregisters == 0 && harness->Bridge.UnregisterAfterDestroy == 0,
                "shutdown: every texture released and every registration removed in order") && ok;
            service.Shutdown();
            ok = Check(service.GetCounters().RetirementsDrainedAtShutdown == 1, "shutdown: idempotent") && ok;
            ok = Check(service.Create(4, 4, "late") == kInvalidUiTextureHandle && service.GetLastError() == UiTextureError::ServiceUnavailable
                    && !service.Update(keep, WholeUpdate(bytes, 4, 4)) && service.GetLastError() == UiTextureError::ServiceUnavailable
                    && !service.Destroy(keep) && !service.Resize(keep, 2, 2) && service.GetImGuiId(keep) == 0,
                "shutdown: further calls are rejected") && ok;
            ok = Check(harness->Device.WaitCalls == 0, "shutdown: the service itself never waited") && ok;
        }

        // Destroying the service without Shutdown still releases everything.
        {
            Harness harness;
            const UiTextureHandle handle = harness.Service->Create(4, 4, "destructor");
            ok = Check(handle != kInvalidUiTextureHandle, "destructor: create") && ok;
            harness.Service.reset();
            ok = Check(harness.Device.TexturesCreated == harness.Device.TexturesDestroyed && harness.Bridge.LiveRegistrations() == 0,
                "destructor: texture released and registration removed") && ok;
        }

        // Main-thread-only: every other thread is rejected and nothing is touched.
        {
            Harness harness;
            UiTextureService& service = *harness.Service;
            const UiTextureHandle handle = service.Create(4, 4, "owner");
            const std::vector<u8> bytes = Solid(4, 4, 7);
            bool created = true;
            bool updated = true;
            bool resized = true;
            bool destroyed = true;
            bool collected = true;
            u64 id = 1;
            UiTextureCounters foreign;
            std::thread other([&]
            {
                created = service.Create(2, 2, "foreign") != kInvalidUiTextureHandle;
                updated = service.Update(handle, WholeUpdate(bytes, 4, 4));
                resized = service.Resize(handle, 2, 2);
                destroyed = service.Destroy(handle);
                collected = service.CollectRetired() != 0;
                id = service.GetImGuiId(handle);
                foreign = service.GetCounters();
                service.Shutdown();
            });
            other.join();
            ok = Check(!created && !updated && !resized && !destroyed && !collected && id == 0, "threads: foreign-thread calls are rejected") && ok;
            ok = Check(foreign.Created == 0 && foreign.WrongThreadRejections == 7, "threads: a foreign counter read exposes only the rejection tally") && ok;
            u32 width = 0;
            u32 height = 0;
            const UiTextureCounters counters = service.GetCounters();
            ok = Check(!service.IsShutDown() && service.GetExtent(handle, width, height) && width == 4 && height == 4
                    && counters.Created == 1 && counters.Rejected == 0 && counters.UpdatesSubmitted == 0 && counters.WrongThreadRejections == 8,
                "threads: the owner's state is untouched by foreign calls") && ok;
            ok = AccountingConsistent(harness, "threads") && ok;
        }

        // Whole-life no-stall: a full create/update/resize/destroy/collect cycle never waits.
        {
            Harness harness;
            UiTextureService& service = *harness.Service;
            const UiTextureHandle handle = service.Create(16, 16, "cycle");
            const std::vector<u8> bytes = Solid(16, 4, 5);
            for (u32 frame = 0; frame < 20; ++frame)
            {
                ok = Check(service.Update(handle, WholeUpdate(bytes, 16, 4)) || service.GetLastError() == UiTextureError::UpdateBackpressure,
                    "cycle: update never blocks") && ok;
                if (frame == 9)
                {
                    harness.Device.ExecuteAll();
                    ok = Check(service.Resize(handle, 24, 8), "cycle: resize") && ok;
                }
                if (frame % 3 == 0)
                    harness.Device.ExecuteOldest();
                harness.Bridge.Submitted = frame;
                harness.Bridge.Completed = frame > 0 ? frame - 1 : 0;
                service.CollectRetired();
            }
            ok = Check(service.Destroy(handle), "cycle: destroy") && ok;
            harness.Device.ExecuteAll();
            harness.Bridge.Completed = harness.Bridge.Submitted + 1;
            ok = Check(service.CollectRetired() >= 1 && service.GetCounters().IsDrained(), "cycle: everything retires without a wait") && ok;
            ok = AccountingConsistent(harness, "cycle") && ok;
        }
        return ok;
    }
}
