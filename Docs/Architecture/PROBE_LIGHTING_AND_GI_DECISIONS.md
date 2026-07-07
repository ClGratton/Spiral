# Probe Lighting And GI Decisions

Status: Draft v0.1
Date: 2026-07-06

Purpose: decide how probe-based lighting, Detroit: Become Human lessons, screen-space GI/AO fallback, ambient fallback, and static/dynamic shading consistency fit this engine.

## Short Decisions

| Topic | Decision |
| --- | --- |
| Probe lighting | Yes. Use a Detroit-inspired adaptive probe/irradiance volume as the stable indirect-lighting backbone. |
| Static + dynamic consistency | Static and dynamic objects must evaluate the same indirect-lighting function in the same lighting pass. |
| Static lightmaps | Allowed for high-frequency static surface detail, but not as a separate lighting universe. |
| Dynamic objects | Sample the same probe/volume/light-field data as nearby static surfaces, with proxy/per-pixel volume support for large/skinned objects. |
| Screen-space GI | Current-frame local correction only, not the primary GI foundation. |
| AO | Spatial GTAO/XeGTAO plus bent normals/specular occlusion; ray/probe assist where needed. |
| Ambient fallback | Sky SH/SG + probe fallback + AO, with debug warnings when probe coverage is missing. |
| Detroit lessons | Adopt photometric units, material calibration, adaptive sparse probes, virtual probe offsets, portal/zone GI blending, and debug tools. |
| idTech 8 lessons | Adopt world radiance cache / irradiance volume structure as a reference, but not hidden temporal filtering as baseline. |
| Neural Light Grid | Strong authoring/precompute reference for huge scenes; treat learned probe weighting as pipeline acceleration, not required runtime magic. |
| Detroit features to avoid | Their TAA-dependent shadows/volumetrics/SSR are not baseline-compatible with this engine. |

## Detroit: Become Human Findings

The GDC 2018 Detroit lighting talk is very relevant.

Useful lessons:

- Lighting coherency was a main goal.
- They moved to photometric units: directional lights in lux, local lights in lumens, emissive surfaces in EV/luminance terms.
- They built material calibration/debug tools to keep albedo, metallic, glass reflectance, and exposure coherent.
- Detroit uses clustered forward shading, full linear lighting, GGX specular, layered BRDFs, and photometric lights.
- Their indirect lighting moved beyond static-only vertex baking/probes because they needed a unified static/dynamic solution.
- They used specular probe lighting with captured cubemaps, GGX filtering, influence boxes, and parallax boxes.
- For diffuse GI they built an adaptive sparse octree probe volume with artist cues and automatic voxelization for denser placement around walls/objects.
- They found probe rejection based on occlusion created interpolation artifacts, so they kept interpolation stable and used virtual probe offsets instead.
- They used artist attractor/repulsor volumes to move probes away from leaking positions.
- They stored irradiance as 2nd-order spherical harmonics and made sampling efficient through volume textures and hash lookup.
- They explicitly supported blending multiple GI sets for interior/exterior, light switches, lightning, time of day, and curtains/portals.
- Dynamic objects crossing portals sampled both GI sets based on distance and normal direction.
- Some static objects near doors/windows/frames were manually tagged to sample both GIs too.

Things not to copy directly:

- Detroit still used Lambert diffuse; our material default is Callisto/Proxima-style diffuse.
- Their shadows, volumetrics, SSR, and some anti-aliasing relied on temporal supersampling/TAA; those are not allowed as baseline here.
- Their probe system was production-strong for its time, but modern light-field/DDGI-style visibility should improve leaking and dynamic response.

## Probe Lighting Backbone

Decision:

```text
Use an adaptive probe/light-field volume as the stable indirect-lighting backbone.
```

Data layers:

| Data | Purpose |
| --- | --- |
| Diffuse irradiance SH/SG | Low-frequency indirect diffuse for static and dynamic objects. |
| Directional/bent normal term | Directionality and occlusion-aware diffuse response. |
| Visibility/distance data | Reduce light leaking and support probe ray marching/cone tests. |
| Specular radiance/probe data | Rough reflection fallback and environment contribution. |
| Zone/portal weights | Blend multiple GI sets in transitions. |
| Validity/confidence | Debug and fallback control. |

Start simple:

```text
adaptive sparse probe volume + SH irradiance + sky visibility + local reflection probes
```

Evolve toward:

```text
light-field probes with visibility/distance + sparse ray residual correction + time-keyed probe sets
```

## Static And Dynamic In Same Pass

Core rule:

```text
Static and dynamic objects must be shaded by the same lighting equation and indirect-lighting API.
```

The shader should not have one disconnected ambient model for dynamic objects and a separate baked-lightmap-only model for static objects.

Use a unified input:

```cpp
struct IndirectLightingSample
{
    float3 diffuseIrradiance;
    float3 bentNormal;
    float  diffuseOcclusion;
    float3 specularRadiance;
    float  specularOcclusion;
    float  confidence;
    uint   sourceFlags; // lightmap, probe, screen-space, ray, fallback
};
```

For static surfaces:

- Primary source can be directional lightmap or baked surface data.
- Also sample probe volume for zone blending, reflection fallback, dynamic consistency, LODs, and debug.
- Near portals/doors/windows, allow static surfaces to blend multiple GI sets like Detroit.

For dynamic/skinned objects:

- Primary source is probe/light-field volume.
- Large objects use proxy/per-pixel/probe-volume sampling, not one object-center probe.
- Characters should sample at multiple anchors: pelvis/chest/head/hands/feet or per-cluster for hero quality.
- Contact AO/shadow/residual terms must be current-frame so characters sit in the environment.

For both:

- Direct dynamic lights are evaluated in the same clustered/deferred lighting pass.
- Indirect sample is fed into the same BRDF code.
- AO/specular occlusion and ray residuals are applied consistently.
- Debug source view shows whether a pixel came from lightmap, probe, screen-space, RT, or fallback.

## Lightmaps Versus Probe Volumes

Use both, but assign jobs clearly.

Lightmaps are good for:

- Static high-frequency indirect detail.
- Interior color bleeding on walls/floors.
- Low runtime cost.
- Lower-end modes.
- Artist-controlled baked scenes.

Probe volumes are good for:

- Dynamic objects.
- Static LODs/HLODs.
- Volumetrics/fog indirect lighting.
- Portal/zone transitions.
- Time-of-day/light-switch variants.
- Consistent ambient/indirect lookup.

Do not let lightmaps make dynamic objects look detached. If static surfaces use baked high-frequency lighting, dynamic objects need probe, contact, and screen-space/ray terms that match the same environment.

## Probe Placement And Leak Control

Adopt Detroit-style adaptive placement:

- Sparse octree or brick volume.
- Higher density near walls, corners, doors, windows, vertical transitions, and high lighting gradients.
- Artist density zones.
- Automatic voxelization/geometry analysis for probe placement.
- Attractor/repulsor volumes for hard cases.
- Portal/zone metadata.

Leak control:

- Avoid simply discarding probes at runtime because it can create interpolation discontinuities.
- Prefer virtual probe offsets, visibility/distance terms, and higher density near problematic geometry.
- Store probe validity and confidence.
- Bright leaks are more noticeable than dim leaks; error metrics should weight them more.
- Add debug views for probe interpolation, leak risk, visibility, and confidence.

Modern improvement:

- Light-field probes store visibility/distance per direction, not only SH irradiance.
- DDGI-style probes add dynamic updates where hardware budget allows.
- Sparse ray residuals can correct visible dynamic changes without making the probe system fully dynamic.

## idTech 8 And Neural Light Grid Lessons

idTech 8's 2025 GI work is important because it replaced heavy pre-baked lighting workflows with a scalable real-time GI structure built around:

- world visibility sampling,
- a world radiance cache,
- irradiance volume/probe atlas storage,
- spatial denoise and bilateral upscale,
- screen-space, world-cache, and volume fallbacks.

The structure fits this engine well. The caution is that idTech 8 also uses previous-frame data and temporal filtering in parts of the pipeline. We should borrow the world-space cache hierarchy and probe atlas ideas, but enforce our own rule:

```text
World-space caches may update over time with explicit validity.
Pixel color must not rely on hidden multi-frame convergence.
```

Activision's Neural Light Grid is also relevant. Its best lesson for us is production workflow: use learned/offline tools to generate compact expressive probe weighting quickly for very large scenes. That can help small teams because lighting data can become more automatic and less hand-placed.

Use Neural Light Grid-style ideas for:

- faster probe/bake generation,
- compact probe influence representation,
- leak reduction and artist iteration,
- large-world lighting metadata.

Do not use it as:

- the only runtime GI path,
- an opaque system that artists cannot debug,
- a replacement for current-frame contact, hero, and dynamic-light correction.

## GI Transitions And Zones

Detroit's portal/zone GI blending fits our needs very well.

Use explicit lighting zones:

- Interior.
- Exterior.
- Room/scene zone.
- Portal/window/door transition.
- Time-of-day key.
- Weather/light-switch variant.

Rules:

- Dynamic objects crossing zones sample both GI sets.
- Blend weight depends on distance to portal/volume, normal direction, and camera/receiver context.
- Static objects normally belong to one zone, but door frames, windows, partially exterior props, and transition architecture can be tagged to sample both.
- Transitions must be deterministic and current-frame, not temporally accumulated.
- Editor displays GI blend volumes and transition weights.

## Screen-Space GI/AO Role

Screen-space GI is useful but cannot be the foundation.

Use screen-space effects for:

- Contact bounce and near-field color bleed.
- Dynamic object/environment interaction.
- Small missing occlusion not represented by probes.
- Current-frame correction where the screen has valid data.

Do not use screen-space GI for:

- Off-screen lighting.
- Primary ambient fallback.
- Long-range indirect.
- Stable lighting through camera cuts.
- Hidden temporal accumulation.

Recommended stack:

```text
baked/probe indirect backbone
  + spatial GTAO/XeGTAO
  + screen-space GI/contact bounce where valid
  + sparse RT residual for important visible misses
  + sky/ambient fallback only when probe coverage is missing
```

The screen-space component should output confidence and validity. Invalid regions fall back to probe/sky/ray, not stale history.

## Ambient Fallback

Fallback order:

1. Time-keyed probe/light-field volume.
2. Static lightmap or directional lightmap if available.
3. Local parallax-corrected reflection/irradiance probe.
4. Sky SH/SG with sky visibility.
5. Neutral calibrated ambient debug fallback.

AO fallback:

1. Bent normal/specular occlusion from bake/probe.
2. Spatial GTAO/XeGTAO.
3. Screen-space contact occlusion.
4. Sparse RT occlusion for hero/difficult cases.

If an object reaches fallback 4 or 5 in normal gameplay, the editor should warn about missing probe coverage.

## Volumetrics

Detroit lit volumetrics with direct lights and diffuse probe grid data. This fits us.

Rules:

- Volumetric fog samples the same probe/sky indirect data as surfaces.
- Use min/max depth or occupancy information to reduce light leaking through walls.
- Volumetric indirect lighting must respect lighting zones/portals.
- No TAA-dependent volumetric baseline. Checkerboard/low-res paths need same-frame spatial resolve that is acceptable without temporal buildup.

## Daylight And Probe Sets

Tie this to the time-of-day decision:

- Bake adaptive time-keyed probe sets.
- Blend probe/lightmap keys in linear HDR.
- Use dynamic sun/moon direct lighting current-frame.
- Probe keys are chosen by lighting-error, not equal time spacing.
- Reflection probes and diffuse probes share key selection where possible.

Probe set blending is the same mechanism for:

- Time of day.
- Light switches.
- Lightning.
- Curtains/blinds/doors opening.
- Interior/exterior transitions.

## Editor Workflow

Guided workflow:

```text
mark static geometry -> choose lighting zones -> generate adaptive probes -> bake -> inspect leaks/confidence -> tag transition objects -> validate dynamic character path
```

Required debug views:

- Probe density.
- Probe SH/radiance.
- Probe confidence.
- Virtual probe offsets.
- Leak risk heatmap.
- GI source per pixel.
- Static/dynamic mismatch heatmap.
- Portal/zone blend weights.
- Screen-space GI validity.
- AO/bent-normal/specular-occlusion source.
- Character/environment contact confidence.

Automation:

- AI can suggest density zones, portal volumes, probe attractors/repulsors, and transition tags.
- The system must explain why a dynamic object looks mismatched: missing probes, bad zone, insufficient anchors, no contact occlusion, wrong exposure, wrong material calibration, or fallback ambient.

## First Implementation Order

1. Photometric lighting units and exposure debug tools.
2. Material calibration debug views.
3. Basic adaptive probe volume with SH irradiance.
4. Static/dynamic unified `IndirectLightingSample` shader path.
5. Dynamic object multi-anchor/proxy-volume sampling.
6. Spatial GTAO/XeGTAO contact layer.
7. Portal/zone GI blending.
8. Lightmap + probe unified static shading.
9. Reflection probe/local IBL integration.
10. Probe leak debug and virtual offsets.
11. Time-keyed probe set blending.
12. Light-field probe visibility/distance data.
13. Sparse RT residual correction for visible GI/occlusion misses.

## Sources

- The Lighting Technology of Detroit: Become Human PDF: https://media.gdcvault.com/gdc2018/presentations/CAURANT_GUILLAUME_The_Lighting_Technology.pdf
- GDC Vault Detroit lighting talk: https://www.gdcvault.com/play/1025339/The-Lighting-Technology-of-Detroit
- Cluster Forward Rendering and Anti-Aliasing in Detroit: Become Human: https://gdcvault.com/play/1025420/Cluster-Forward-Rendering-and-Anti
- Porting Detroit Become Human from PlayStation 4 to PC, Part 2: https://gpuopen.com/learn/porting-detroit-2/
- Unity Light Probes for moving objects: https://docs.unity3d.com/6000.5/Documentation/Manual/LightProbes-MovingObjects.html
- Unity Adaptive Probe Volumes: https://docs.unity3d.com/6000.0/Documentation/Manual/urp/probevolumes-concept.html
- Unity Light Probe Proxy Volume: https://docs.unity3d.com/560/Documentation/Manual/class-LightProbeProxyVolume.html
- Unreal Volumetric Lightmaps: https://dev.epicgames.com/documentation/unreal-engine/volumetric-lightmaps-in-unreal-engine
- Unreal Indirect Lighting Cache: https://dev.epicgames.com/documentation/unreal-engine/indirect-lighting-cache-in-unreal-engine
- Real-Time Global Illumination using Precomputed Light Field Probes: https://research.nvidia.com/publication/2017-02_real-time-global-illumination-using-precomputed-light-field-probes
- Light Field Probes PDF: https://research.nvidia.com/sites/default/files/pubs/2017-02_Real-Time-Global-Illumination/light-field-probes-final.pdf
- Scaling Probe-Based Real-Time Dynamic Global Illumination for Production: https://arxiv.org/abs/2009.10796
- idTech 8 Global Illumination slides: https://advances.realtimerendering.com/s2025/content/SOUSA_SIGGRAPH_2025_Final.pdf
- Activision Neural Light Grid: https://research.activision.com/publications/2024/08/Neural_Light_Grid
- XeGTAO: https://github.com/GameTechDev/XeGTAO
