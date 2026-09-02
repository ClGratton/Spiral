#pragma once

#include "Engine/Assets/TextureArtifact.h"
#include "Engine/RHI/Device.h"

#include <string>
#include <vector>

namespace Engine
{
    struct TextureArtifactUploadSubresource
    {
        u32 MipLevel = 0;
        RHI::Extent2D Extent;
        u64 ByteOffset = 0;
        u64 RowPitchBytes = 0;
        u64 ByteSize = 0;
    };

    // This is an immutable, engine-owned bridge from a validated cooked artifact
    // to the future native multi-subresource upload. It does not select a backend
    // format fallback, allocate a texture, or publish a descriptor-table slot.
    struct TextureArtifactUploadPlan
    {
        AssetHandle Asset = kInvalidAssetHandle;
        std::string SourcePath;
        TextureRole Role = TextureRole::BaseColor;
        TextureColorSpace ColorSpace = TextureColorSpace::Srgb;
        TextureTargetProfile TargetProfile = TextureTargetProfile::RGBAFallback;
        bool HasAlpha = false;
        RHI::TextureDescription Texture;
        std::vector<TextureArtifactUploadSubresource> Subresources;
        Ref<const std::vector<u8>> Payload;
    };

    bool BuildTextureArtifactUploadPlan(const TextureArtifact& artifact,
        TextureArtifactUploadPlan& outPlan, std::string& outError);

    struct TextureArtifactUploadSelection
    {
        TextureArtifactUploadPlan Plan;
        bool UsedExplicitRgbaFallback = false;
        std::string Diagnostic;
    };

    bool BuildTextureUploadBatch(const TextureArtifactUploadPlan& plan,
        RHI::TextureUploadBatch& outUpload, std::string& outError);

    // `rgbaFallback` must be a separately cooked plan for the same semantic
    // texture. The selected device's retained format table is the sole support
    // authority; absence is unsupported rather than assumed support.
    bool SelectTextureArtifactUploadPlan(const RHI::Device& device,
        const TextureArtifactUploadPlan& preferred,
        const TextureArtifactUploadPlan* rgbaFallback,
        TextureArtifactUploadSelection& outSelection,
        std::string& outError);
}
