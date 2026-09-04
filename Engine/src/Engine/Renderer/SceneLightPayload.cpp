#include "Engine/Renderer/SceneLightPayload.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace Engine
{
    namespace
    {
        constexpr u32 kMagic = 0x504C5347u; // "GSLP"

        bool Finite(const Math::Vec3& value)
        {
            return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
        }

        bool AddChecked(u32 left, u32 right, u32& out)
        {
            if (right > std::numeric_limits<u32>::max() - left)
                return false;
            out = left + right;
            return true;
        }

        bool MultiplyChecked(u64 left, u64 right, u64& out)
        {
            if (left != 0 && right > std::numeric_limits<u64>::max() / left)
                return false;
            out = left * right;
            return true;
        }

        bool PackedWordCount(u32 scalarCount, u32& out)
        {
            return AddChecked(scalarCount, 3u, out) && ((out /= 4u), true);
        }

        u32 FloatBits(float value) { return std::bit_cast<u32>(value); }
        u32 DoubleLow(double value) { return static_cast<u32>(std::bit_cast<u64>(value)); }
        u32 DoubleHigh(double value) { return static_cast<u32>(std::bit_cast<u64>(value) >> 32u); }

        bool SameFloat(float left, float right)
        {
            return std::bit_cast<u32>(left) == std::bit_cast<u32>(right);
        }

        bool SameVector(const Math::Vec3& left, const Math::Vec3& right)
        {
            return SameFloat(left.X, right.X) && SameFloat(left.Y, right.Y)
                && SameFloat(left.Z, right.Z);
        }

        Math::Vec3 TransformPoint(const Math::Vec3& point, const Math::Mat4& matrix)
        {
            return {
                point.X * matrix.Values[0] + point.Y * matrix.Values[4]
                    + point.Z * matrix.Values[8] + matrix.Values[12],
                point.X * matrix.Values[1] + point.Y * matrix.Values[5]
                    + point.Z * matrix.Values[9] + matrix.Values[13],
                point.X * matrix.Values[2] + point.Y * matrix.Values[6]
                    + point.Z * matrix.Values[10] + matrix.Values[14]
            };
        }

        Math::Vec3 TransformDirection(const Math::Vec3& direction, const Math::Mat4& matrix)
        {
            return {
                direction.X * matrix.Values[0] + direction.Y * matrix.Values[4]
                    + direction.Z * matrix.Values[8],
                direction.X * matrix.Values[1] + direction.Y * matrix.Values[5]
                    + direction.Z * matrix.Values[9],
                direction.X * matrix.Values[2] + direction.Y * matrix.Values[6]
                    + direction.Z * matrix.Values[10]
            };
        }

        Math::Vec3 LightDirection(const Math::Vec3& rotationDegrees)
        {
            const float yaw = Math::DegreesToRadians(rotationDegrees.Y);
            const float pitch = Math::DegreesToRadians(rotationDegrees.X);
            return {
                std::sin(yaw) * std::cos(pitch),
                -std::sin(pitch),
                std::cos(yaw) * std::cos(pitch)
            };
        }

        bool TryProjectionClipPlanes(const Math::Mat4& projection, float& outNear, float& outFar)
        {
            const float zScale = projection.Values[10];
            const float zOffset = projection.Values[14];
            if (!std::isfinite(zScale) || !std::isfinite(zOffset)
                || zScale <= 1.0f || zOffset >= 0.0f)
                return false;
            const float nearClip = -zOffset / zScale;
            const float farClip = -zOffset / (zScale - 1.0f);
            if (!std::isfinite(nearClip) || !std::isfinite(farClip)
                || nearClip <= 0.0f || farClip <= nearClip)
                return false;
            outNear = nearClip;
            outFar = farClip;
            return true;
        }
    }

    bool BuildSceneLightPayload(const SceneRenderSnapshot& snapshot,
        size_t viewIndex, const ClusteredLightGrid& grid,
        const RendererColorPipelineSettings& colorSettings, u64 generation,
        SceneLightPayload& outPayload, std::string& outError)
    {
        const auto fail = [&outError](const char* message) { outError = message; return false; };
        if (generation == 0 || viewIndex >= snapshot.Views.size()
            || !snapshot.Views[viewIndex].Camera.Valid
            || snapshot.Lights.size() != grid.Lights.size()
            || grid.ViewportWidth == 0 || grid.ViewportHeight == 0
            || grid.TileSizePixels == 0 || grid.TileSizePixels > 4096
            || grid.DepthSliceCount == 0 || grid.DepthSliceCount > 128
            || grid.MaximumLocalLightsPerCluster == 0
            || grid.MaximumLocalLightsPerCluster > 1024)
            return fail("scene light payload has invalid snapshot or grid dimensions");
        ScenePreExposureState preExposure;
        if (!TryResolveScenePreExposure(colorSettings, preExposure))
            return fail("scene light payload has invalid color-pipeline exposure");

        const u32 expectedTileCountX = 1u + (grid.ViewportWidth - 1u) / grid.TileSizePixels;
        const u32 expectedTileCountY = 1u + (grid.ViewportHeight - 1u) / grid.TileSizePixels;
        u64 tileCount64 = 0;
        u64 clusterCount64 = 0;
        if (!MultiplyChecked(grid.TileCountX, grid.TileCountY, tileCount64)
            || !MultiplyChecked(tileCount64, grid.DepthSliceCount, clusterCount64))
            return fail("scene light payload cluster count overflows its bounded ABI");
        constexpr u64 maximumClusterCount = 4u * 1024u * 1024u;
        if (grid.TileCountX != expectedTileCountX || grid.TileCountY != expectedTileCountY
            || clusterCount64 == 0
            || clusterCount64 > maximumClusterCount
            || clusterCount64 > std::numeric_limits<u32>::max()
                / static_cast<u64>(grid.MaximumLocalLightsPerCluster)
            || clusterCount64 >= static_cast<u64>(std::numeric_limits<size_t>::max())
            || clusterCount64 + 1u > static_cast<u64>(std::numeric_limits<u32>::max()))
            return fail("scene light payload grid metadata does not match its viewport");
        const size_t clusterCount = static_cast<size_t>(clusterCount64);
        if (grid.ClusterOffsets.size() != clusterCount + 1
            || grid.ClusterOffsets.empty() || grid.ClusterOffsets.front() != 0
            || grid.ClusterOffsets.back() != grid.LocalLightIndices.size())
            return fail("scene light payload has invalid CSR shape");
        if (snapshot.Lights.size() > std::numeric_limits<u32>::max()
            || grid.GlobalLightIndices.size() > std::numeric_limits<u32>::max()
            || grid.GlobalLightIndices.size() > kMaximumDirectionalLightCount
            || grid.ClusterOffsets.size() > std::numeric_limits<u32>::max()
            || grid.LocalLightIndices.size() > std::numeric_limits<u32>::max())
            return fail("scene light payload count exceeds ABI");

        const CameraView& view = snapshot.Views[viewIndex].Camera;
        Math::SectorLocalPosition translationOriginPosition;
        if (view.HasCanonicalTranslationOrigin)
            translationOriginPosition = view.TranslationOriginPosition;
        else if (!Math::TryDecomposeWorldPosition(
            view.TranslationOrigin, snapshot.WorldGridPolicy, translationOriginPosition))
            return fail("scene light payload could not canonicalize its view origin");
        float expectedNear = 0.0f;
        float expectedFar = 0.0f;
        if (!TryProjectionClipPlanes(view.Projection, expectedNear, expectedFar)
            || !SameFloat(grid.NearClip, expectedNear)
            || !SameFloat(grid.FarClip, expectedFar))
            return fail("scene light payload clip planes do not match its snapshot view");

        std::unordered_set<EntityId> sourceEntities;
        std::vector<u32> expectedDirectionalIndices;
        for (size_t index = 0; index < snapshot.Lights.size(); ++index)
        {
            const SceneRenderLight& source = snapshot.Lights[index];
            const ClusteredLightRecord& record = grid.Lights[index];
            const float consumption = static_cast<float>(record.PhotometricValue);
            if (source.SourceEntity == kInvalidEntityId
                || !sourceEntities.insert(source.SourceEntity).second
                || !Finite(source.Color) || source.Color.X < 0.0f
                || source.Color.Y < 0.0f || source.Color.Z < 0.0f
                || !Finite(source.Transform.RotationDegrees)
                || !IsValidLightPhotometricValue(source.Type,
                    source.PhotometricUnit, source.PhotometricValue)
                || !std::isfinite(source.Range) || source.Range < 0.0f
                || !std::isfinite(source.InnerConeDegrees)
                || !std::isfinite(source.OuterConeDegrees)
                || source.InnerConeDegrees < 0.0f
                || source.OuterConeDegrees < source.InnerConeDegrees
                || source.OuterConeDegrees > 180.0f)
                return fail("scene light payload snapshot contains an invalid light");

            Math::DVec3 originRelativePosition;
            if (!Math::TryGetSectorLocalRelativePosition(source.Transform.Position,
                    translationOriginPosition, snapshot.WorldGridPolicy, originRelativePosition))
                return fail("scene light payload could not translate a light into the view origin");
            const double floatLimit = static_cast<double>(std::numeric_limits<float>::max());
            if (originRelativePosition.X < -floatLimit || originRelativePosition.X > floatLimit
                || originRelativePosition.Y < -floatLimit || originRelativePosition.Y > floatLimit
                || originRelativePosition.Z < -floatLimit || originRelativePosition.Z > floatLimit)
                return fail("scene light payload light position exceeds float view space");
            const Math::Vec3 expectedViewPosition = TransformPoint({
                static_cast<float>(originRelativePosition.X),
                static_cast<float>(originRelativePosition.Y),
                static_cast<float>(originRelativePosition.Z)
            }, view.View);
            const Math::Vec3 expectedWorldDirection = LightDirection(
                source.Transform.RotationDegrees);
            const Math::Vec3 expectedViewDirection = TransformDirection(
                expectedWorldDirection, view.View);
            const float expectedInnerCone = std::cos(
                Math::DegreesToRadians(source.InnerConeDegrees));
            const float expectedOuterCone = std::cos(
                Math::DegreesToRadians(source.OuterConeDegrees));
            if (!Finite(record.ViewPosition)
                || !Finite(record.WorldDirection)
                || !Finite(record.ViewDirection)
                || !Finite(record.Color)
                || !std::isfinite(record.Range)
                || !std::isfinite(record.InnerConeCosine)
                || !std::isfinite(record.OuterConeCosine)
                || !Finite(expectedViewPosition)
                || !Finite(expectedWorldDirection)
                || !Finite(expectedViewDirection)
                || !std::isfinite(expectedInnerCone)
                || !std::isfinite(expectedOuterCone)
                || record.SourceEntity != source.SourceEntity || record.Type != source.Type
                || record.PhotometricUnit != source.PhotometricUnit
                || std::bit_cast<u64>(record.PhotometricValue)
                    != std::bit_cast<u64>(source.PhotometricValue)
                || !std::isfinite(consumption)
                || !SameVector(record.ViewPosition, expectedViewPosition)
                || !SameVector(record.WorldDirection, expectedWorldDirection)
                || !SameVector(record.ViewDirection, expectedViewDirection)
                || !SameVector(record.Color, source.Color)
                || !SameFloat(record.Range, source.Range)
                || !SameFloat(record.InnerConeCosine, expectedInnerCone)
                || !SameFloat(record.OuterConeCosine, expectedOuterCone)
                || record.CastsShadows != source.CastsShadows)
                return fail("scene light payload snapshot and grid records disagree or are invalid");
            if (source.Type == LightType::Directional)
                expectedDirectionalIndices.push_back(static_cast<u32>(index));
        }
        if (grid.GlobalLightIndices != expectedDirectionalIndices)
            return fail("scene light payload global list does not exactly match directional lights");
        for (size_t index = 1; index < grid.ClusterOffsets.size(); ++index)
        {
            const u32 previousOffset = grid.ClusterOffsets[index - 1];
            const u32 currentOffset = grid.ClusterOffsets[index];
            if (previousOffset > grid.LocalLightIndices.size()
                || currentOffset > grid.LocalLightIndices.size()
                || currentOffset < previousOffset
                || currentOffset - previousOffset
                    > grid.MaximumLocalLightsPerCluster)
                return fail("scene light payload CSR offsets are out of bounds or not monotonic");
            std::unordered_set<u32> clusterLights;
            for (u32 cursor = previousOffset; cursor < currentOffset; ++cursor)
            {
                if (!clusterLights.insert(grid.LocalLightIndices[cursor]).second)
                    return fail("scene light payload CSR cluster repeats a local light");
            }
        }
        for (u32 index : grid.GlobalLightIndices)
            if (index >= grid.Lights.size() || grid.Lights[index].Type != LightType::Directional)
                return fail("scene light payload global list contains a non-directional or invalid index");
        for (u32 index : grid.LocalLightIndices)
            if (index >= grid.Lights.size() || grid.Lights[index].Type == LightType::Directional)
                return fail("scene light payload CSR contains a directional or invalid index");

        u32 recordsEnd = 0, offsetsOffset = 0, localOffset = 0, total = 0;
        u32 directionalWords = 0, offsetWords = 0, localWords = 0;
        const u32 lightCount = static_cast<u32>(grid.Lights.size());
        if (lightCount > (std::numeric_limits<u32>::max() - SceneLightPayload::HeaderWordCount)
                / SceneLightPayload::LightRecordWordCount
            || !AddChecked(SceneLightPayload::HeaderWordCount,
                lightCount * SceneLightPayload::LightRecordWordCount, recordsEnd)
            || !PackedWordCount(static_cast<u32>(grid.GlobalLightIndices.size()), directionalWords))
            return fail("scene light payload size overflows its bounded ABI");
        const u32 directionalOffset = recordsEnd;
        if (!PackedWordCount(static_cast<u32>(grid.ClusterOffsets.size()), offsetWords)
            || !PackedWordCount(static_cast<u32>(grid.LocalLightIndices.size()), localWords)
            || !AddChecked(directionalOffset, directionalWords, offsetsOffset)
            || !AddChecked(offsetsOffset, offsetWords, localOffset)
            || !AddChecked(localOffset, localWords, total)
            || total > SceneLightPayload::MaximumWordCount)
            return fail("scene light payload size overflows its bounded ABI");

        SceneLightPayload candidate;
        candidate.Generation = generation;
        candidate.ColorSettings = colorSettings;
        candidate.PreExposure = preExposure;
        candidate.Words.resize(total, { 0, 0, 0, 0 });
        candidate.Words[0] = { kMagic, SceneLightPayload::Version, total, SceneLightPayload::HeaderWordCount };
        candidate.Words[1] = { SceneLightPayload::HeaderWordCount, lightCount, directionalOffset, static_cast<u32>(grid.GlobalLightIndices.size()) };
        candidate.Words[2] = { offsetsOffset, static_cast<u32>(grid.ClusterOffsets.size()), localOffset, static_cast<u32>(grid.LocalLightIndices.size()) };
        candidate.Words[3] = { grid.ViewportWidth, grid.ViewportHeight, grid.TileSizePixels, grid.DepthSliceCount };
        candidate.Words[4] = { grid.TileCountX, grid.TileCountY, FloatBits(grid.NearClip), FloatBits(grid.FarClip) };
        candidate.Words[5] = { grid.MaximumLocalLightsPerCluster,
            grid.OverflowedLocalLightReferences,
            SceneLightPayload::LightRecordWordCount,
            FloatBits(preExposure.Scale) };
        u32 cursor = SceneLightPayload::HeaderWordCount;
        for (const ClusteredLightRecord& record : grid.Lights)
        {
            const float consumption = static_cast<float>(record.PhotometricValue);
            candidate.Words[cursor++] = { record.SourceEntity, static_cast<u32>(record.Type), static_cast<u32>(record.PhotometricUnit), record.CastsShadows ? 1u : 0u };
            candidate.Words[cursor++] = { DoubleLow(record.PhotometricValue), DoubleHigh(record.PhotometricValue), FloatBits(consumption), 0 };
            candidate.Words[cursor++] = { FloatBits(record.ViewPosition.X), FloatBits(record.ViewPosition.Y), FloatBits(record.ViewPosition.Z), FloatBits(record.Range) };
            candidate.Words[cursor++] = { FloatBits(record.WorldDirection.X), FloatBits(record.WorldDirection.Y), FloatBits(record.WorldDirection.Z), FloatBits(record.InnerConeCosine) };
            candidate.Words[cursor++] = { FloatBits(record.ViewDirection.X), FloatBits(record.ViewDirection.Y), FloatBits(record.ViewDirection.Z), FloatBits(record.OuterConeCosine) };
            candidate.Words[cursor++] = { FloatBits(record.Color.X), FloatBits(record.Color.Y), FloatBits(record.Color.Z), 0 };
        }
        for (size_t index = 0; index < grid.GlobalLightIndices.size(); ++index) candidate.Words[directionalOffset + index / 4u][index % 4u] = grid.GlobalLightIndices[index];
        for (size_t index = 0; index < grid.ClusterOffsets.size(); ++index) candidate.Words[offsetsOffset + index / 4u][index % 4u] = grid.ClusterOffsets[index];
        for (size_t index = 0; index < grid.LocalLightIndices.size(); ++index) candidate.Words[localOffset + index / 4u][index % 4u] = grid.LocalLightIndices[index];
        outPayload = std::move(candidate);
        outError.clear();
        return true;
    }

    bool SceneLightPayloadPublication::Acquire(RHI::Device& device,
        const SceneRenderSnapshot& snapshot, size_t viewIndex, const ClusteredLightGrid& grid,
        const RendererColorPipelineSettings& colorSettings, u64 generation,
        Ref<SceneLightPayloadSlot>& outSlot, std::string& outError)
    {
        if (m_Device && m_Device != &device)
        {
            outError = "scene light payload publication cannot cross devices";
            return false;
        }
        if (m_LastAcceptedSlot && m_LastAcceptedSlot->Payload
            && generation <= m_LastAcceptedSlot->Payload->Generation)
        {
            outError = "scene light payload generation is not newer than the accepted publication";
            return false;
        }
        SceneLightPayload packed;
        if (!BuildSceneLightPayload(snapshot, viewIndex, grid,
            colorSettings, generation, packed, outError))
            return false;
        const u64 sizeBytes = packed.Words.size() * sizeof(SceneLightPayloadWord);
        if (sizeBytes == 0 || sizeBytes > std::numeric_limits<u32>::max())
        {
            outError = "scene light payload byte size is invalid";
            return false;
        }
        size_t slotIndex = Capacity;
        bool canReuse = false;
        for (size_t index = 0; index < Capacity; ++index)
        {
            const Ref<SceneLightPayloadSlot>& slot = m_Slots[index];
            if (slot && slot.use_count() == 1 && slot->Staging && slot->Gpu
                && slot->Staging->GetDescription().SizeBytes == sizeBytes
                && slot->Gpu->GetDescription().SizeBytes == sizeBytes)
            {
                slotIndex = index;
                canReuse = true;
                break;
            }
        }
        if (slotIndex == Capacity)
        {
            for (size_t index = 0; index < Capacity; ++index)
            {
                if (!m_Slots[index])
                {
                    slotIndex = index;
                    break;
                }
            }
        }
        if (slotIndex == Capacity)
        {
            for (size_t index = 0; index < Capacity; ++index)
            {
                if (m_Slots[index].use_count() == 1)
                {
                    slotIndex = index;
                    break;
                }
            }
        }
        if (slotIndex == Capacity)
        {
            ++m_Diagnostics.CapacityRejectionCount;
            outError = "scene light payload slots are retained at bounded capacity";
            return false;
        }
        Ref<SceneLightPayloadSlot>& existing = m_Slots[slotIndex];
        const Ref<const SceneLightPayload> packedPayload =
            CreateRef<const SceneLightPayload>(std::move(packed));
        RHI::BufferDescription stagingDescription;
        stagingDescription.DebugName = "Scene Light Payload Staging";
        stagingDescription.SizeBytes = sizeBytes;
        stagingDescription.Usage = RHI::BufferUsage::CopySource;
        stagingDescription.CpuAccess = RHI::BufferCpuAccess::Write;
        stagingDescription.InitialState = RHI::ResourceState::CopySource;
        RHI::BufferDescription gpuDescription;
        gpuDescription.DebugName = "Scene Light Payload GPU";
        gpuDescription.SizeBytes = sizeBytes;
        gpuDescription.StrideBytes = RHI::kFixedReadOnlyStructuredBufferStrideBytes;
        gpuDescription.Usage = static_cast<RHI::BufferUsage>(
            static_cast<u32>(RHI::BufferUsage::Structured) | static_cast<u32>(RHI::BufferUsage::CopyDest));
        gpuDescription.InitialState = RHI::ResourceState::CopyDest;
        Ref<SceneLightPayloadSlot> candidate;
        if (canReuse)
            candidate = existing;
        else
        {
            Scope<RHI::Buffer> staging = device.CreateBuffer(stagingDescription);
            Scope<RHI::Buffer> gpu = device.CreateBuffer(gpuDescription);
            if (!staging || !gpu)
            {
                outError = "scene light payload buffer allocation failed";
                return false;
            }
            candidate = CreateRef<SceneLightPayloadSlot>();
            candidate->Staging = Ref<RHI::Buffer>(staging.release());
            candidate->Gpu = Ref<RHI::Buffer>(gpu.release());
        }
        void* mapped = candidate->Staging->Map();
        if (!mapped)
        {
            outError = "scene light payload staging map failed";
            return false;
        }
        std::memcpy(mapped, packedPayload->Words.data(), static_cast<size_t>(sizeBytes));
        candidate->Staging->Unmap();
        candidate->Payload = packedPayload;
        if (!canReuse)
            existing = candidate;
        if (canReuse)
            ++m_Diagnostics.ReuseCount;
        else
            ++m_Diagnostics.AllocationCount;
        m_Device = &device;
        outSlot = candidate;
        outError.clear();
        return true;
    }

    bool SceneLightPayloadPublication::Commit(
        const Ref<SceneLightPayloadSlot>& slot, std::string& outError)
    {
        if (!slot || !slot->Payload || !slot->Staging || !slot->Gpu)
        {
            outError = "scene light payload commit requires a complete acquired slot";
            return false;
        }
        const bool owned = std::any_of(m_Slots.begin(), m_Slots.end(),
            [&slot](const Ref<SceneLightPayloadSlot>& candidate)
            {
                return candidate.get() == slot.get();
            });
        if (!owned || (m_LastAcceptedSlot && m_LastAcceptedSlot->Payload
            && slot->Payload->Generation <= m_LastAcceptedSlot->Payload->Generation))
        {
            outError = "scene light payload commit is stale or not owned by this publication";
            return false;
        }
        m_LastAcceptedSlot = slot;
        ++m_Diagnostics.CommitCount;
        outError.clear();
        return true;
    }

    void SceneLightPayloadPublication::ClearAfterDeviceIdle()
    {
        m_Slots = {};
        m_LastAcceptedSlot.reset();
        m_Device = nullptr;
        m_Diagnostics = {};
    }
}
