#pragma once

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/RHI/TextureBindingTable.h"
#include "Engine/Renderer/TextureGpuResourceCache.h"

#include <optional>
#include <vector>

namespace Engine
{
    // Owns stable asset-to-table publication after T4B has produced a complete
    // exact-device bundle. Mutation is serialized with accepted frame use.
    class TextureTablePublication final
    {
    public:
        static constexpr size_t RetainedFrameCapacity = 4;

        static Scope<TextureTablePublication> Create(RHI::Device& device,
            const RHI::TextureBindingTableDescription& description);
        ~TextureTablePublication();

        RHI::TextureBindingHandle GetErrorHandle() const { return m_ErrorHandle; }
        RHI::TextureBindingHandle Resolve(AssetHandle asset) const;
        // Exposed for command binding and published-slot inspection only;
        // this coordinator remains the sole table-mutation authority.
        RHI::TextureBindingTable* GetBindingTable() const { return m_Table.get(); }

        // Failure returns the declared error handle and does not alter an
        // existing asset publication or another table slot.
        RHI::TextureBindingHandle Publish(AssetHandle asset,
            const Ref<const TextureGpuResourceBundle>& bundle,
            RHI::TextureSampler sampler, std::string& outError);
        bool ReplaceUnaccepted(AssetHandle asset,
            const Ref<const TextureGpuResourceBundle>& replacement,
            RHI::TextureSampler sampler, std::string& outError);
        bool RemoveUnaccepted(AssetHandle asset, std::string& outError);

        // Calls must follow submission-acceptance order. Pending assets are
        // frozen so later frames cannot extend the operation's last-use token.
        bool RetainAcceptedFrame(const RHI::CompletionToken& token,
            const std::vector<RHI::TextureBindingHandle>& handles,
            std::string& outError);
        bool QueueReplacement(AssetHandle asset,
            const Ref<const TextureGpuResourceBundle>& replacement,
            RHI::TextureSampler sampler, std::string& outError);
        bool QueueRemoval(AssetHandle asset, std::string& outError);
        bool Retire(const RHI::CompletionToken& token, std::string& outError);

        // The caller establishes device idle before releasing the table and
        // every retained bundle, and calls this before device destruction.
        void ReleaseAfterDeviceIdle();

        size_t GetAssetCount() const;
        size_t GetRetainedFrameCount() const;
        size_t GetPendingOperationCount() const;

    private:
        struct PendingOperation;
        struct Entry;
        struct RetainedFrame;

        TextureTablePublication(RHI::Device& device, Scope<RHI::TextureBindingTable> table);
        bool IsPublishable(AssetHandle asset, const Ref<const TextureGpuResourceBundle>& bundle,
            std::string& outError) const;
        Entry* FindEntry(AssetHandle asset);
        const Entry* FindEntry(AssetHandle asset) const;
        Entry* FindEntry(RHI::TextureBindingHandle handle);

        RHI::Device& m_Device;
        Scope<RHI::TextureBindingTable> m_Table;
        RHI::TextureBindingHandle m_ErrorHandle;
        std::vector<Entry> m_Entries;
        std::vector<RetainedFrame> m_Frames;
    };
}
