#include "Engine/Renderer/UiTextureService.h"

#include "Engine/Core/Log.h"

#include <algorithm>
#include <limits>

namespace Engine
{
    namespace
    {
        constexpr RHI::ResourceState kSteadyState = RHI::ResourceState::ShaderResource;
        constexpr RHI::Format kUiTextureFormat = RHI::Format::R8G8B8A8Unorm;

        constexpr u64 kHandleSlotMask = 0xFFFFFFFFull;

        UiTextureHandle MakeHandle(u32 slotIndex, u32 generation)
        {
            return (static_cast<u64>(generation) << 32) | (static_cast<u64>(slotIndex) + 1u);
        }

        RHI::TextureDescription MakeDescription(u32 width, u32 height, std::string_view debugName)
        {
            RHI::TextureDescription description;
            description.DebugName = "UiTexture ";
            description.DebugName += debugName;
            description.Extent = { width, height };
            description.TextureFormat = kUiTextureFormat;
            description.Usage = static_cast<RHI::TextureUsage>(
                static_cast<u32>(RHI::TextureUsage::ShaderResource) | static_cast<u32>(RHI::TextureUsage::CopyDest));
            // Equal to the steady state: backends that restore a texture's
            // creation state at list close then agree with the write bracket.
            description.InitialState = kSteadyState;
            return description;
        }
    }

    UiTextureService::UiTextureService(RHI::Device& device, UiTextureNativeBridge& bridge)
        : m_Device(device), m_Bridge(bridge), m_OwnerThread(std::this_thread::get_id())
    {
    }

    UiTextureService::~UiTextureService()
    {
        Shutdown();
    }

    bool UiTextureService::OnOwnerThread() const
    {
        if (std::this_thread::get_id() == m_OwnerThread)
            return true;
        m_WrongThreadRejections.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    bool UiTextureService::Fail(UiTextureError error)
    {
        m_LastError = error;
        ++m_Counters.Rejected;
        return false;
    }

    UiTextureHandle UiTextureService::FailCreate(UiTextureError error)
    {
        Fail(error);
        return kInvalidUiTextureHandle;
    }

    UiTextureService::Slot* UiTextureService::FindSlot(UiTextureHandle handle)
    {
        const u64 index = (handle & kHandleSlotMask);
        const u32 generation = static_cast<u32>(handle >> 32);
        if (index == 0 || index > m_Slots.size())
            return nullptr;
        Slot& slot = m_Slots[static_cast<size_t>(index - 1)];
        return slot.Live && slot.Generation == generation ? &slot : nullptr;
    }

    const UiTextureService::Slot* UiTextureService::FindSlot(UiTextureHandle handle) const
    {
        return const_cast<UiTextureService*>(this)->FindSlot(handle);
    }

    u32 UiTextureService::RegisteredCount() const
    {
        return m_Counters.LiveTextures + static_cast<u32>(m_Retirements.size());
    }

    UiTextureCounters UiTextureService::GetCounters() const
    {
        if (!OnOwnerThread())
        {
            UiTextureCounters rejected;
            rejected.WrongThreadRejections = m_WrongThreadRejections.load(std::memory_order_relaxed);
            return rejected;
        }
        UiTextureCounters counters = m_Counters;
        counters.PendingRetirements = static_cast<u32>(m_Retirements.size());
        counters.WrongThreadRejections = m_WrongThreadRejections.load(std::memory_order_relaxed);
        return counters;
    }

    UiTextureError UiTextureService::AcquireList(size_t& outIndex)
    {
        for (size_t index = 0; index < m_Lists.size();)
        {
            PooledList& pooled = m_Lists[index];
            if (!pooled.LastSubmission.IsValid())
            {
                outIndex = index;
                return UiTextureError::None;
            }
            const RHI::CompletionStatus status = m_Device.QueryCompletion(pooled.LastSubmission);
            if (status == RHI::CompletionStatus::Complete)
            {
                ++m_Counters.CommandListReuses;
                outIndex = index;
                return UiTextureError::None;
            }
            if (status == RHI::CompletionStatus::Failed || status == RHI::CompletionStatus::Invalid)
            {
                // A list whose submission can never be observed complete cannot
                // be reopened; drop it instead of holding a pool slot forever.
                m_Lists.erase(m_Lists.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }
            ++index;
        }

        if (m_Lists.size() >= kMaximumUiTextureCommandLists)
            return UiTextureError::UpdateBackpressure;
        Scope<RHI::CommandList> list = m_Device.CreateCommandList(RHI::QueueType::Graphics, "UiTextureService Write");
        if (!list)
            return UiTextureError::UpdateRecordFailed;
        ++m_Counters.CommandListsCreated;
        m_Lists.push_back({ std::move(list), {} });
        outIndex = m_Lists.size() - 1;
        return UiTextureError::None;
    }

    UiTextureError UiTextureService::RecordAndSubmit(RHI::Texture& texture, const RHI::TextureWrite& write, bool firstUse,
        RHI::CompletionToken& outToken)
    {
        size_t index = 0;
        const UiTextureError acquired = AcquireList(index);
        if (acquired != UiTextureError::None)
            return acquired;

        RHI::CommandList& list = *m_Lists[index].List;
        bool recorded = list.Begin();
        if (recorded)
        {
            recorded = firstUse
                ? list.TransitionTexture(texture, RHI::ResourceState::CopyDest)
                    && list.WriteTexture(texture, write)
                    && list.TransitionTexture(texture, kSteadyState)
                : RHI::RecordTextureWrite(list, texture, write, kSteadyState);
        }
        recorded = recorded && list.End();
        if (!recorded)
        {
            // A list that failed part-way through recording must not be reused
            // or submitted; the texture's accepted state is unchanged.
            m_Lists.erase(m_Lists.begin() + static_cast<std::ptrdiff_t>(index));
            return UiTextureError::UpdateRecordFailed;
        }

        const RHI::CompletionToken token = m_Device.Submit(list);
        if (!token.IsValid())
        {
            m_Lists.erase(m_Lists.begin() + static_cast<std::ptrdiff_t>(index));
            return UiTextureError::UpdateSubmitFailed;
        }
        m_Lists[index].LastSubmission = token;
        outToken = token;
        return UiTextureError::None;
    }

    UiTextureError UiTextureService::CreateGpuTexture(u32 width, u32 height, std::string_view debugName, GpuTexture& outTexture)
    {
        if (!m_Device.SupportsTextureWrite())
            return UiTextureError::ServiceUnavailable;
        if (RegisteredCount() >= kMaximumRegisteredUiTextures)
            CollectRetired();
        if (RegisteredCount() >= kMaximumRegisteredUiTextures)
            return UiTextureError::TextureLimit;

        GpuTexture texture;
        texture.Resource = m_Device.CreateTexture(MakeDescription(width, height, debugName));
        if (!texture.Resource)
            return UiTextureError::DeviceCreateFailed;
        ++m_Counters.GpuTexturesCreated;
        texture.Width = width;
        texture.Height = height;

        texture.ImGuiId = m_Bridge.Register(*texture.Resource);
        if (texture.ImGuiId == 0)
        {
            ReleaseGpuTexture(texture);
            return UiTextureError::NativeRegistrationFailed;
        }
        ++m_Counters.NativeRegistrations;

        // Registration refers to the steady layout, so the texture must reach
        // it through one accepted submission before anything can sample it.
        const std::vector<u8> transparent(static_cast<size_t>(width) * height * 4u, u8 { 0 });
        RHI::TextureWrite write;
        write.Extent = { width, height };
        write.TextureFormat = kUiTextureFormat;
        write.Data = transparent.data();
        write.DataSizeBytes = transparent.size();
        UiTextureError writeError = UiTextureError::InitialWriteFailed;
        if (RHI::ValidateTextureWrite(texture.Resource->GetDescription(), RHI::ResourceState::CopyDest, write)
                == RHI::TextureWriteStatus::Valid)
        {
            const UiTextureError submitted = RecordAndSubmit(*texture.Resource, write, true, texture.LastWrite);
            if (submitted == UiTextureError::None)
                writeError = UiTextureError::None;
            else if (submitted == UiTextureError::UpdateBackpressure)
                writeError = submitted;
        }
        if (writeError != UiTextureError::None)
        {
            // Nothing was accepted by the queue, so no GPU work can reference
            // the texture and it can be released at once.
            ReleaseGpuTexture(texture);
            return writeError;
        }
        ++m_Counters.InitialWritesSubmitted;
        outTexture = std::move(texture);
        return UiTextureError::None;
    }

    void UiTextureService::ReleaseGpuTexture(GpuTexture& texture)
    {
        if (texture.ImGuiId != 0)
        {
            m_Bridge.Unregister(texture.ImGuiId);
            ++m_Counters.NativeUnregistrations;
        }
        if (texture.Resource)
        {
            texture.Resource.reset();
            ++m_Counters.GpuTexturesReleased;
        }
        texture = {};
    }

    void UiTextureService::QueueRetirement(GpuTexture&& texture)
    {
        m_Retirements.push_back({ std::move(texture), m_Bridge.ReferenceBoundSerial() });
        ++m_Counters.RetirementsQueued;
        m_Counters.PeakPendingRetirements = std::max(m_Counters.PeakPendingRetirements, static_cast<u32>(m_Retirements.size()));
    }

    UiTextureHandle UiTextureService::Create(u32 width, u32 height, std::string_view debugName)
    {
        if (!OnOwnerThread())
            return kInvalidUiTextureHandle;
        if (m_ShutDown)
            return FailCreate(UiTextureError::ServiceUnavailable);
        if (width == 0 || height == 0 || width > kMaximumUiTextureDimension || height > kMaximumUiTextureDimension)
            return FailCreate(UiTextureError::InvalidSize);

        GpuTexture texture;
        const UiTextureError error = CreateGpuTexture(width, height, debugName, texture);
        if (error != UiTextureError::None)
            return FailCreate(error);

        u32 slotIndex = 0;
        if (!m_FreeSlots.empty())
        {
            slotIndex = m_FreeSlots.back();
            m_FreeSlots.pop_back();
        }
        else
        {
            slotIndex = static_cast<u32>(m_Slots.size());
            m_Slots.emplace_back();
        }
        Slot& slot = m_Slots[slotIndex];
        slot.Live = true;
        slot.DebugName = std::string(debugName);
        slot.Texture = std::move(texture);
        ++m_Counters.Created;
        ++m_Counters.LiveTextures;
        m_LastError = UiTextureError::None;
        return MakeHandle(slotIndex, slot.Generation);
    }

    bool UiTextureService::Update(UiTextureHandle handle, const UiTextureUpdate& update)
    {
        if (!OnOwnerThread())
            return false;
        Slot* slot = m_ShutDown ? nullptr : FindSlot(handle);
        if (!slot)
            return Fail(m_ShutDown ? UiTextureError::ServiceUnavailable : UiTextureError::InvalidHandle);

        RHI::TextureWrite write;
        write.X = update.Rect.X;
        write.Y = update.Rect.Y;
        write.Extent = { update.Rect.Width, update.Rect.Height };
        write.TextureFormat = kUiTextureFormat;
        write.RowPitchBytes = update.RowPitchBytes;
        write.Data = update.Pixels;
        write.DataSizeBytes = update.PixelBytes;
        // Rejects bad rectangles, pitches and short data before any list is
        // touched, so a refused update cannot leave a half-recorded list behind.
        if (RHI::ValidateTextureWrite(slot->Texture.Resource->GetDescription(), RHI::ResourceState::CopyDest, write)
                != RHI::TextureWriteStatus::Valid)
            return Fail(UiTextureError::InvalidUpdate);

        RHI::CompletionToken token;
        const UiTextureError error = RecordAndSubmit(*slot->Texture.Resource, write, false, token);
        if (error != UiTextureError::None)
            return Fail(error);
        slot->Texture.LastWrite = token;
        ++m_Counters.UpdatesSubmitted;
        m_Counters.UpdateBytes += static_cast<u64>(update.Rect.Width) * update.Rect.Height * 4u;
        m_LastError = UiTextureError::None;
        return true;
    }

    bool UiTextureService::Resize(UiTextureHandle handle, u32 width, u32 height)
    {
        if (!OnOwnerThread())
            return false;
        Slot* slot = m_ShutDown ? nullptr : FindSlot(handle);
        if (!slot)
            return Fail(m_ShutDown ? UiTextureError::ServiceUnavailable : UiTextureError::InvalidHandle);
        if (width == 0 || height == 0 || width > kMaximumUiTextureDimension || height > kMaximumUiTextureDimension)
            return Fail(UiTextureError::InvalidSize);
        if (slot->Texture.Width == width && slot->Texture.Height == height)
        {
            m_LastError = UiTextureError::None;
            return true;
        }

        GpuTexture replacement;
        const UiTextureError error = CreateGpuTexture(width, height, slot->DebugName, replacement);
        if (error != UiTextureError::None)
            return Fail(error);

        // CreateGpuTexture never invalidates slot references; swap only after
        // the replacement is fully registered and written.
        GpuTexture previous = std::move(slot->Texture);
        slot->Texture = std::move(replacement);
        QueueRetirement(std::move(previous));
        ++m_Counters.Resized;
        m_LastError = UiTextureError::None;
        return true;
    }

    bool UiTextureService::Destroy(UiTextureHandle handle)
    {
        if (!OnOwnerThread())
            return false;
        Slot* slot = m_ShutDown ? nullptr : FindSlot(handle);
        if (!slot)
            return Fail(m_ShutDown ? UiTextureError::ServiceUnavailable : UiTextureError::InvalidHandle);

        const u32 slotIndex = static_cast<u32>(slot - m_Slots.data());
        QueueRetirement(std::move(slot->Texture));
        slot->Texture = {};
        slot->Live = false;
        slot->DebugName.clear();
        // Generation zero is never issued, so a wrapped generation skips it.
        slot->Generation = slot->Generation == std::numeric_limits<u32>::max() ? 1u : slot->Generation + 1u;
        m_FreeSlots.push_back(slotIndex);
        ++m_Counters.Destroyed;
        --m_Counters.LiveTextures;
        m_LastError = UiTextureError::None;
        return true;
    }

    u64 UiTextureService::GetImGuiId(UiTextureHandle handle) const
    {
        if (!OnOwnerThread())
            return 0;
        const Slot* slot = FindSlot(handle);
        return slot ? slot->Texture.ImGuiId : 0;
    }

    bool UiTextureService::GetExtent(UiTextureHandle handle, u32& outWidth, u32& outHeight) const
    {
        if (!OnOwnerThread())
            return false;
        const Slot* slot = FindSlot(handle);
        if (!slot)
            return false;
        outWidth = slot->Texture.Width;
        outHeight = slot->Texture.Height;
        return true;
    }

    u32 UiTextureService::CollectRetired()
    {
        if (!OnOwnerThread() || m_Retirements.empty())
            return 0;

        const u64 completedSerial = m_Bridge.CompletedSerial();
        u32 released = 0;
        for (size_t index = 0; index < m_Retirements.size();)
        {
            Retirement& retirement = m_Retirements[index];
            bool writesDone = true;
            if (retirement.Texture.LastWrite.IsValid())
            {
                // Failed is terminal as well: the queue is finished with it.
                const RHI::CompletionStatus status = m_Device.QueryCompletion(retirement.Texture.LastWrite);
                writesDone = status == RHI::CompletionStatus::Complete || status == RHI::CompletionStatus::Failed;
            }
            const bool presentationDone = completedSerial >= retirement.BoundSerial;
            if (!writesDone)
                ++m_Counters.RetirementHoldsWriteToken;
            if (!presentationDone)
                ++m_Counters.RetirementHoldsPresentation;
            if (!writesDone || !presentationDone)
            {
                ++index;
                continue;
            }

            ReleaseGpuTexture(retirement.Texture);
            m_Retirements.erase(m_Retirements.begin() + static_cast<std::ptrdiff_t>(index));
            ++m_Counters.RetirementsReleased;
            ++released;
        }
        return released;
    }

    void UiTextureService::Shutdown()
    {
        if (m_ShutDown || !OnOwnerThread())
            return;
        m_ShutDown = true;

        for (Retirement& retirement : m_Retirements)
        {
            ReleaseGpuTexture(retirement.Texture);
            ++m_Counters.RetirementsDrainedAtShutdown;
        }
        m_Retirements.clear();

        for (Slot& slot : m_Slots)
        {
            if (!slot.Live)
                continue;
            ReleaseGpuTexture(slot.Texture);
            slot.Live = false;
            --m_Counters.LiveTextures;
            ++m_Counters.LeakedAtShutdown;
        }
        if (m_Counters.LeakedAtShutdown != 0)
            Log::Warn("UI texture service released ", m_Counters.LeakedAtShutdown, " texture(s) that the owner never destroyed");
        m_Lists.clear();
    }
}
