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
}
