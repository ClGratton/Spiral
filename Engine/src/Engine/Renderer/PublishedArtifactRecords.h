#pragma once

#include "Engine/Assets/MeshArtifact.h"
#include "Engine/Assets/TextureArtifact.h"
#include "Engine/Core/Base.h"

namespace Engine
{
    // One decoded mesh artifact retained by an immutable artifact-resolver
    // snapshot. The snapshot parses each cooked mesh at most once, so every
    // instance and every frame that retains the snapshot shares the same
    // immutable payload instead of re-reading the file. Bounds cover every
    // vertex position and are computed with the decode, not per frame.
    struct PublishedMeshRecord
    {
        Ref<const MeshArtifact> Artifact;
        float BoundsMinimum[3] {};
        float BoundsMaximum[3] {};
    };

    // The role and color space of a texture's preferred cooked variant. This is
    // all a material binding needs to validate a slot, so the large payload is
    // not retained by the snapshot.
    struct PublishedTextureSemantics
    {
        TextureRole Role = TextureRole::BaseColor;
        TextureColorSpace ColorSpace = TextureColorSpace::Srgb;
    };

    // Diagnostic counters of one resolver snapshot's retained decode cache.
    // A load decodes a cooked artifact from disk; a hit reuses the snapshot's
    // retained result (including a retained failure).
    struct ArtifactResolverCacheStats
    {
        u64 MeshLoads = 0;
        u64 MeshHits = 0;
        u64 TextureSemanticLoads = 0;
        u64 TextureSemanticHits = 0;
    };
}
