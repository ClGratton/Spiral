#include "Engine/Renderer/TextureArtifactUploadPlan.h"

#include <limits>
#include <utility>

namespace Engine
{
    namespace
    {
        bool MapCookedFormat(TextureCookedFormat cooked, RHI::Format& outFormat)
        {
            switch (cooked)
            {
                case TextureCookedFormat::R8G8B8A8Unorm: outFormat = RHI::Format::R8G8B8A8Unorm; return true;
                case TextureCookedFormat::R8G8B8A8Srgb: outFormat = RHI::Format::R8G8B8A8UnormSrgb; return true;
                case TextureCookedFormat::Bc5Unorm: outFormat = RHI::Format::BC5Unorm; return true;
                case TextureCookedFormat::Bc7Unorm: outFormat = RHI::Format::BC7Unorm; return true;
                case TextureCookedFormat::Bc7Srgb: outFormat = RHI::Format::BC7UnormSrgb; return true;
                case TextureCookedFormat::Astc4x4Unorm: outFormat = RHI::Format::ASTC4x4Unorm; return true;
                case TextureCookedFormat::Astc4x4Srgb: outFormat = RHI::Format::ASTC4x4UnormSrgb; return true;
            }
            return false;
        }

        bool DeviceSupportsUploadFormat(const RHI::Device& device, RHI::Format format)
        {
            const RHI::FormatUsage required = RHI::FormatUsage::Sampled | RHI::FormatUsage::CopyDestination;
            for (const RHI::FormatCapability& capability : device.GetCapabilities().Formats)
            {
                if (capability.Value == format && RHI::HasAllFormatUsages(capability.Usages, required))
                    return true;
            }
            return false;
        }

        bool HasMatchingSemantics(const TextureArtifactUploadPlan& preferred,
            const TextureArtifactUploadPlan& fallback)
        {
            return fallback.TargetProfile == TextureTargetProfile::RGBAFallback
                && (fallback.Texture.TextureFormat == RHI::Format::R8G8B8A8Unorm
                    || fallback.Texture.TextureFormat == RHI::Format::R8G8B8A8UnormSrgb)
                && fallback.Asset == preferred.Asset
                && fallback.SourcePath == preferred.SourcePath
                && fallback.Role == preferred.Role
                && fallback.ColorSpace == preferred.ColorSpace
                && fallback.HasAlpha == preferred.HasAlpha
                && fallback.Texture.Extent.Width == preferred.Texture.Extent.Width
                && fallback.Texture.Extent.Height == preferred.Texture.Extent.Height
                && fallback.Texture.MipLevels == preferred.Texture.MipLevels;
        }

        bool HasConsistentPlanFormat(const TextureArtifactUploadPlan& plan)
        {
            if (plan.Asset == kInvalidAssetHandle || plan.SourcePath.empty())
                return false;
            const bool semanticColorSpace = plan.Role == TextureRole::BaseColor || plan.Role == TextureRole::Emissive
                ? plan.ColorSpace == TextureColorSpace::Srgb
                : (plan.Role == TextureRole::Normal || plan.Role == TextureRole::Orm || plan.Role == TextureRole::Mask)
                    && plan.ColorSpace == TextureColorSpace::Linear;
            if (!semanticColorSpace)
                return false;
            if (plan.TargetProfile == TextureTargetProfile::RGBAFallback)
                return plan.Texture.TextureFormat == (plan.ColorSpace == TextureColorSpace::Srgb
                    ? RHI::Format::R8G8B8A8UnormSrgb : RHI::Format::R8G8B8A8Unorm);
            if (plan.TargetProfile == TextureTargetProfile::Astc)
                return plan.Texture.TextureFormat == (plan.ColorSpace == TextureColorSpace::Srgb
                    ? RHI::Format::ASTC4x4UnormSrgb : RHI::Format::ASTC4x4Unorm);
            if (plan.TargetProfile != TextureTargetProfile::DesktopBC)
                return false;
            if (plan.Role == TextureRole::Normal)
                return plan.ColorSpace == TextureColorSpace::Linear && plan.Texture.TextureFormat == RHI::Format::BC5Unorm;
            return plan.Texture.TextureFormat == (plan.ColorSpace == TextureColorSpace::Srgb
                ? RHI::Format::BC7UnormSrgb : RHI::Format::BC7Unorm);
        }
    }

    bool BuildTextureArtifactUploadPlan(const TextureArtifact& artifact,
        TextureArtifactUploadPlan& outPlan, std::string& outError)
    {
        if (!ValidateTextureArtifact(artifact, outError))
            return false;
        if (artifact.Mips.size() > std::numeric_limits<u32>::max())
        {
            outError = "texture artifact mip count exceeds the RHI limit";
            return false;
        }

        RHI::Format format = RHI::Format::Unknown;
        if (!MapCookedFormat(artifact.CookedFormat, format))
        {
            outError = "texture artifact cooked format has no RHI mapping";
            return false;
        }

        TextureArtifactUploadPlan candidate;
        candidate.Asset = artifact.Asset;
        candidate.SourcePath = artifact.SourcePath;
        candidate.Role = artifact.Role;
        candidate.ColorSpace = artifact.ColorSpace;
        candidate.TargetProfile = artifact.TargetProfile;
        candidate.HasAlpha = artifact.HasAlpha;
        candidate.Texture.DebugName = "TextureArtifact " + std::to_string(artifact.Asset);
        candidate.Texture.Extent = { artifact.Mips.front().Width, artifact.Mips.front().Height };
        candidate.Texture.TextureFormat = format;
        candidate.Texture.Usage = static_cast<RHI::TextureUsage>(
            static_cast<u32>(RHI::TextureUsage::CopyDest) | static_cast<u32>(RHI::TextureUsage::ShaderResource));
        candidate.Texture.InitialState = RHI::ResourceState::CopyDest;
        candidate.Texture.MipLevels = static_cast<u32>(artifact.Mips.size());
        candidate.Texture.ArrayLayers = 1;
        candidate.Texture.SampleCount = 1;
        candidate.Subresources.reserve(artifact.Mips.size());

        for (size_t index = 0; index < artifact.Mips.size(); ++index)
        {
            const TextureArtifactMip& mip = artifact.Mips[index];
            u64 rowPitch = 0;
            u64 byteCount = 0;
            if (!RHI::CalculateTextureSubresourceStorage(format, { mip.Width, mip.Height }, rowPitch, byteCount)
                || byteCount != mip.ByteSize)
            {
                outError = "texture artifact mip layout does not match its RHI format";
                return false;
            }
            candidate.Subresources.push_back({ static_cast<u32>(index), { mip.Width, mip.Height },
                mip.ByteOffset, rowPitch, mip.ByteSize });
        }

        candidate.Payload = CreateRef<std::vector<u8>>(artifact.Payload);
        outPlan = std::move(candidate);
        outError.clear();
        return true;
    }

    bool BuildTextureUploadBatch(const TextureArtifactUploadPlan& plan,
        RHI::TextureUploadBatch& outUpload, std::string& outError)
    {
        if (!HasConsistentPlanFormat(plan))
        {
            outError = "texture artifact upload plan format does not match its preserved target semantics";
            return false;
        }
        RHI::TextureUploadBatch candidate;
        candidate.TextureFormat = plan.Texture.TextureFormat;
        candidate.Bytes = plan.Payload;
        candidate.Subresources.reserve(plan.Subresources.size());
        for (const TextureArtifactUploadSubresource& subresource : plan.Subresources)
        {
            candidate.Subresources.push_back({ subresource.MipLevel, 0, subresource.Extent,
                subresource.ByteOffset, subresource.RowPitchBytes, subresource.ByteSize });
        }
        if (!RHI::IsReadOnlyTextureUploadCompatible(plan.Texture, candidate))
        {
            outError = "texture artifact upload plan is not a complete compatible mip batch";
            return false;
        }
        outUpload = std::move(candidate);
        outError.clear();
        return true;
    }

    bool SelectTextureArtifactUploadPlan(const RHI::Device& device,
        const TextureArtifactUploadPlan& preferred,
        const TextureArtifactUploadPlan* rgbaFallback,
        TextureArtifactUploadSelection& outSelection,
        std::string& outError)
    {
        RHI::TextureUploadBatch preferredUpload;
        if (!BuildTextureUploadBatch(preferred, preferredUpload, outError))
            return false;

        TextureArtifactUploadSelection candidate;
        if (DeviceSupportsUploadFormat(device, preferred.Texture.TextureFormat))
        {
            candidate.Plan = preferred;
            candidate.Diagnostic = std::string("selected exact-device preferred format ")
                + RHI::ToString(preferred.Texture.TextureFormat);
        }
        else
        {
            RHI::TextureUploadBatch fallbackUpload;
            if (!rgbaFallback || !HasMatchingSemantics(preferred, *rgbaFallback)
                || !BuildTextureUploadBatch(*rgbaFallback, fallbackUpload, outError))
            {
                outError = std::string("exact device does not support ") + RHI::ToString(preferred.Texture.TextureFormat)
                    + " and no matching explicit RGBA fallback is available";
                return false;
            }
            if (!DeviceSupportsUploadFormat(device, rgbaFallback->Texture.TextureFormat))
            {
                outError = std::string("exact device supports neither preferred ") + RHI::ToString(preferred.Texture.TextureFormat)
                    + " nor explicit RGBA fallback " + RHI::ToString(rgbaFallback->Texture.TextureFormat);
                return false;
            }
            candidate.Plan = *rgbaFallback;
            candidate.UsedExplicitRgbaFallback = true;
            candidate.Diagnostic = std::string("preferred format ") + RHI::ToString(preferred.Texture.TextureFormat)
                + " unsupported; selected separately cooked " + RHI::ToString(rgbaFallback->Texture.TextureFormat) + " fallback";
        }

        outSelection = std::move(candidate);
        outError.clear();
        return true;
    }
}
