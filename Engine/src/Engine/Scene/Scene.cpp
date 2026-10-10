#include "Engine/Scene/Scene.h"

#include "Engine/Core/AtomicFile.h"
#include "Engine/Core/Log.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace Engine
{
    namespace
    {
        constexpr int kSceneFormatVersion = 5;
        // No legitimate record approaches this (the longest carry a name); it only
        // bounds the memory a hostile single-line file can make the loader hold.
        constexpr size_t kMaximumSceneLineBytes = 1024 * 1024;

        void WriteVec3(std::ostream& stream, std::string_view name, const Math::Vec3& value)
        {
            stream << name << ' ' << value.X << ' ' << value.Y << ' ' << value.Z << '\n';
        }

        bool ReadVec3(std::istringstream& stream, Math::Vec3& outValue)
        {
            return static_cast<bool>(stream >> outValue.X >> outValue.Y >> outValue.Z);
        }

        bool ReadDVec3(std::istringstream& stream, Math::DVec3& outValue)
        {
            return static_cast<bool>(stream >> outValue.X >> outValue.Y >> outValue.Z);
        }

        const char* ToString(Math::WorldOriginMode mode)
        {
            switch (mode)
            {
                case Math::WorldOriginMode::ExactCamera: return "ExactCamera";
                case Math::WorldOriginMode::SectorSnapped: return "SectorSnapped";
            }

            return "ExactCamera";
        }

        bool ParseWorldOriginMode(std::string_view value, Math::WorldOriginMode& outMode)
        {
            if (value == "ExactCamera")
            {
                outMode = Math::WorldOriginMode::ExactCamera;
                return true;
            }
            if (value == "SectorSnapped")
            {
                outMode = Math::WorldOriginMode::SectorSnapped;
                return true;
            }

            return false;
        }

        bool ParseBoolean(std::string_view value, bool& outValue)
        {
            if (value == "true")
            {
                outValue = true;
                return true;
            }
            if (value == "false")
            {
                outValue = false;
                return true;
            }

            return false;
        }

        bool IsFinite(const Math::Vec3& value)
        {
            return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
        }

        // Scale components must be normal floats: a denormal such as 1e-39 is
        // positive but its reciprocal overflows to infinity, which the raster
        // preparation rejects (and then blanks the whole viewport).
        bool HasStrictlyPositiveScale(const Math::Vec3& scale)
        {
            constexpr float smallest = std::numeric_limits<float>::min();
            return IsFinite(scale) && scale.X >= smallest && scale.Y >= smallest && scale.Z >= smallest;
        }

        // Raises the counter above `id`, never lowering it. The largest id has no
        // successor: "id + 1" would wrap to zero and silently leave the counter
        // unchanged, so the counter is simply left alone (it can never be issued
        // anyway, see CreateEntity).
        void RaiseNextEntityId(EntityId& counter, EntityId id)
        {
            if (id != std::numeric_limits<EntityId>::max())
                counter = std::max(counter, id + 1);
        }

        bool ContainsLineBreak(std::string_view text)
        {
            return text.find_first_of("\n\r") != std::string_view::npos;
        }

        // Reads one unsigned 32-bit decimal token. Stream extraction into an
        // unsigned type accepts "-1" (wrapping to 4294967295); ids and counters
        // must be plain digits within range.
        bool ReadUnsigned32(std::istringstream& stream, u32& outValue)
        {
            std::string token;
            if (!(stream >> token) || token.empty() || token.size() > 10)
                return false;
            u64 value = 0;
            for (const char digit : token)
            {
                if (digit < '0' || digit > '9')
                    return false;
                value = value * 10 + static_cast<u64>(digit - '0');
            }
            if (value > std::numeric_limits<u32>::max())
                return false;
            outValue = static_cast<u32>(value);
            return true;
        }

        bool HasUnitScale(const Math::Vec3& scale)
        {
            return scale.X == 1.0f && scale.Y == 1.0f && scale.Z == 1.0f;
        }

        bool IsInsertableEntity(const SceneEntity& entity, const Math::WorldGridPolicy& policy)
        {
            return entity.EntityHandle.IsValid()
                && Math::IsCanonical(entity.Transform.GetPosition(), policy)
                && IsFinite(entity.Transform.RotationDegrees)
                && HasStrictlyPositiveScale(entity.Transform.Scale)
                && (!entity.Camera || HasUnitScale(entity.Transform.Scale))
                && (!entity.Camera || IsValidCameraComponent(*entity.Camera))
                && (!entity.Light || IsValidLightComponent(*entity.Light));
        }

        // Everything Save writes must load back: the insertion invariants plus
        // text that cannot break the line-framed format.
        bool IsPersistableEntity(const SceneEntity& entity, const Math::WorldGridPolicy& policy)
        {
            return IsInsertableEntity(entity, policy)
                && !ContainsLineBreak(entity.Name)
                && (!entity.MeshRenderer || !ContainsLineBreak(entity.MeshRenderer->MeshName));
        }
    }

    Scene::Scene(std::string name, Math::WorldGridPolicy worldGridPolicy)
        : m_Name(std::move(name)), m_WorldGridPolicy(worldGridPolicy)
    {
        if (!Math::IsWorldGridPolicyValid(m_WorldGridPolicy))
            throw std::invalid_argument("Scene requires a valid immutable world-grid policy");

        m_MainCameraEntity = CreateEntity("Main Camera");
        SetEntityWorldPosition(m_MainCameraEntity, { 0.0, 0.0, -3.35 });
        SetMainCamera(m_MainCamera);
    }

    void Scene::OnUpdate(Timestep timestep)
    {
        (void)timestep;
    }

    SceneRenderSnapshot Scene::ExtractRenderSnapshot(u64 frameIndex, const CameraView& renderView) const
    {
        SceneRenderSnapshot snapshot;
        snapshot.FrameIndex = frameIndex;
        snapshot.MainCameraEntity = m_MainCameraEntity.Id;
        snapshot.WorldGridPolicy = m_WorldGridPolicy;
        if (renderView.Valid)
            snapshot.Views.push_back({ renderView });

        // Size the three lists exactly once: this runs every frame, and growing
        // them by push_back reallocated each of them repeatedly.
        size_t meshCount = 0;
        size_t lightCount = 0;
        size_t cameraCount = 0;
        for (const SceneEntity& entity : m_Entities)
        {
            meshCount += entity.MeshRenderer && entity.MeshRenderer->Visible ? 1 : 0;
            lightCount += entity.Light ? 1 : 0;
            cameraCount += entity.Camera ? 1 : 0;
        }
        snapshot.Meshes.reserve(meshCount);
        snapshot.Lights.reserve(lightCount);
        snapshot.Cameras.reserve(cameraCount);

        for (const SceneEntity& entity : m_Entities)
        {
            SceneRenderTransform transform;
            transform.Position = entity.Transform.GetPosition();
            transform.RotationDegrees = entity.Transform.RotationDegrees;
            transform.Scale = entity.Transform.Scale;

            if (entity.MeshRenderer && entity.MeshRenderer->Visible)
            {
                SceneRenderMesh mesh;
                mesh.SourceEntity = entity.EntityHandle.Id;
                mesh.Transform = transform;
                mesh.MeshAsset = entity.MeshRenderer->MeshAsset;
                mesh.MaterialAsset = entity.MeshRenderer->MaterialAsset;
                mesh.CastsShadows = entity.MeshRenderer->CastsShadows;
                snapshot.Meshes.push_back(mesh);
            }

            if (entity.Light)
            {
                SceneRenderLight light;
                light.SourceEntity = entity.EntityHandle.Id;
                light.Transform = transform;
                light.Type = entity.Light->Type;
                light.Color = entity.Light->Color;
                light.PhotometricValue = entity.Light->PhotometricValue;
                light.PhotometricUnit = entity.Light->PhotometricUnit;
                light.Range = entity.Light->Range;
                light.InnerConeDegrees = entity.Light->InnerConeDegrees;
                light.OuterConeDegrees = entity.Light->OuterConeDegrees;
                light.CastsShadows = entity.Light->CastsShadows;
                snapshot.Lights.push_back(light);
            }

            if (entity.Camera)
            {
                SceneRenderCamera camera;
                camera.SourceEntity = entity.EntityHandle.Id;
                camera.Transform = transform;
                camera.Projection = entity.Camera->Projection;
                camera.BackgroundColor = entity.Camera->BackgroundColor;
                camera.Main = entity.EntityHandle == m_MainCameraEntity;
                snapshot.Cameras.push_back(camera);
            }
        }

        return snapshot;
    }

    Entity Scene::CreateEntity(std::string name)
    {
        // A counter at the largest id means the id space is exhausted: the
        // largest id itself is never issued, so the counter can neither wrap to an
        // invalid or reused id nor overflow.
        if (m_NextEntityId == kInvalidEntityId || m_NextEntityId == std::numeric_limits<EntityId>::max())
            return {};
        return CreateEntityWithId(m_NextEntityId, std::move(name));
    }

    bool Scene::DestroyEntity(Entity entity)
    {
        size_t index = 0;
        if (!TryGetEntityIndex(entity, index))
            return false;

        const bool destroyedMainCamera = m_Entities[index].EntityHandle == m_MainCameraEntity;
        m_EntityIndexById.erase(entity.Id);
        m_Entities.erase(m_Entities.begin() + static_cast<std::ptrdiff_t>(index));
        RebuildEntityIndexFrom(index);

        if (destroyedMainCamera)
        {
            m_MainCameraEntity = {};
            for (const SceneEntity& candidate : m_Entities)
            {
                if (candidate.Camera)
                {
                    SetMainCameraEntity(candidate.EntityHandle);
                    break;
                }
            }
        }

        return true;
    }

    bool Scene::RestoreEntity(const SceneEntity& entity, size_t index)
    {
        return InsertEntity(SceneEntity(entity), index);
    }

    bool Scene::RestoreEntity(const SceneEntity& entity)
    {
        return RestoreEntity(entity, m_Entities.size());
    }

    Entity Scene::CloneEntity(Entity source, std::string name)
    {
        const SceneEntity* original = FindEntityStorage(source);
        if (!original || m_NextEntityId == kInvalidEntityId
            || m_NextEntityId == std::numeric_limits<EntityId>::max())
        {
            return {};
        }

        SceneEntity copy = *original;
        copy.EntityHandle = Entity { m_NextEntityId };
        copy.Name = std::move(name);
        const Entity cloned = copy.EntityHandle;
        return InsertEntity(std::move(copy), m_Entities.size()) ? cloned : Entity {};
    }

    bool Scene::TryGetEntityIndex(Entity entity, size_t& outIndex) const
    {
        const SceneEntity* sceneEntity = FindEntityStorage(entity);
        if (!sceneEntity)
            return false;

        outIndex = static_cast<size_t>(sceneEntity - m_Entities.data());
        return true;
    }

    bool Scene::IsEntityValid(Entity entity) const
    {
        return FindEntityStorage(entity) != nullptr;
    }

    Entity Scene::FindEntityByName(std::string_view name) const
    {
        const auto it = std::find_if(m_Entities.begin(), m_Entities.end(), [name](const SceneEntity& candidate)
        {
            return candidate.Name == name;
        });

        return it == m_Entities.end() ? Entity {} : it->EntityHandle;
    }

    SceneEntity* Scene::TryGetEntity(Entity entity)
    {
        return FindEntityStorage(entity);
    }

    const SceneEntity* Scene::TryGetEntity(Entity entity) const
    {
        return FindEntityStorage(entity);
    }

    TransformComponent* Scene::TryGetTransform(Entity entity)
    {
        SceneEntity* sceneEntity = FindEntityStorage(entity);
        return sceneEntity ? &sceneEntity->Transform : nullptr;
    }

    const TransformComponent* Scene::TryGetTransform(Entity entity) const
    {
        const SceneEntity* sceneEntity = FindEntityStorage(entity);
        return sceneEntity ? &sceneEntity->Transform : nullptr;
    }

    bool Scene::SetEntityTransform(Entity entity, const Math::SectorLocalPosition& position,
        const Math::Vec3& rotationDegrees, const Math::Vec3& scale)
    {
        SceneEntity* sceneEntity = FindEntityStorage(entity);
        if (!sceneEntity || !Math::IsCanonical(position, m_WorldGridPolicy)
            || !IsFinite(rotationDegrees)
            || !HasStrictlyPositiveScale(scale)
            || (sceneEntity->Camera && !HasUnitScale(scale)))
        {
            return false;
        }

        TransformComponent normalized;
        normalized.RotationDegrees = rotationDegrees;
        normalized.Scale = scale;
        if (!normalized.SetPosition(position, m_WorldGridPolicy))
            return false;

        sceneEntity->Transform = normalized;
        return true;
    }

    bool Scene::SetEntityWorldPosition(Entity entity, const Math::DVec3& position)
    {
        SceneEntity* sceneEntity = FindEntityStorage(entity);
        return sceneEntity && sceneEntity->Transform.SetWorldPosition(position, m_WorldGridPolicy);
    }

    bool Scene::SetEntityWorldPositionAxis(Entity entity, u32 axis, double position)
    {
        SceneEntity* sceneEntity = FindEntityStorage(entity);
        return sceneEntity && sceneEntity->Transform.SetWorldPositionAxis(axis, position, m_WorldGridPolicy);
    }

    bool Scene::SetEntitySectorLocalPosition(Entity entity, const Math::SectorLocalPosition& position)
    {
        SceneEntity* sceneEntity = FindEntityStorage(entity);
        return sceneEntity && sceneEntity->Transform.SetPosition(position, m_WorldGridPolicy);
    }

    bool Scene::TryGetEntityApproximateWorldPosition(Entity entity, Math::DVec3& outPosition) const
    {
        const SceneEntity* sceneEntity = FindEntityStorage(entity);
        return sceneEntity
            && sceneEntity->Transform.TryGetApproximateWorldPosition(m_WorldGridPolicy, outPosition);
    }

    CameraComponent* Scene::AddCameraComponent(Entity entity, const CameraComponent& camera)
    {
        SceneEntity* sceneEntity = FindEntityStorage(entity);
        if (!sceneEntity || !HasUnitScale(sceneEntity->Transform.Scale) || !IsValidCameraComponent(camera))
            return nullptr;

        sceneEntity->Camera = camera;
        if (!m_MainCameraEntity && camera.Primary)
            SetMainCameraEntity(entity);
        else if (entity == m_MainCameraEntity)
            SyncMainCameraCacheFromEntity();

        return &(*sceneEntity->Camera);
    }

    CameraComponent* Scene::TryGetCameraComponent(Entity entity)
    {
        SceneEntity* sceneEntity = FindEntityStorage(entity);
        return sceneEntity && sceneEntity->Camera ? &(*sceneEntity->Camera) : nullptr;
    }

    const CameraComponent* Scene::TryGetCameraComponent(Entity entity) const
    {
        const SceneEntity* sceneEntity = FindEntityStorage(entity);
        return sceneEntity && sceneEntity->Camera ? &(*sceneEntity->Camera) : nullptr;
    }

    bool Scene::RemoveCameraComponent(Entity entity)
    {
        SceneEntity* sceneEntity = FindEntityStorage(entity);
        if (!sceneEntity || !sceneEntity->Camera)
            return false;

        sceneEntity->Camera.reset();
        if (entity == m_MainCameraEntity)
            m_MainCameraEntity = {};

        return true;
    }

    LightComponent* Scene::AddLightComponent(Entity entity, const LightComponent& light)
    {
        SceneEntity* sceneEntity = FindEntityStorage(entity);
        if (!sceneEntity || !IsValidLightComponent(light))
            return nullptr;

        sceneEntity->Light = light;
        return &(*sceneEntity->Light);
    }

    LightComponent* Scene::TryGetLightComponent(Entity entity)
    {
        SceneEntity* sceneEntity = FindEntityStorage(entity);
        return sceneEntity && sceneEntity->Light ? &(*sceneEntity->Light) : nullptr;
    }

    const LightComponent* Scene::TryGetLightComponent(Entity entity) const
    {
        const SceneEntity* sceneEntity = FindEntityStorage(entity);
        return sceneEntity && sceneEntity->Light ? &(*sceneEntity->Light) : nullptr;
    }

    bool Scene::SetLightComponent(Entity entity, const LightComponent& light)
    {
        SceneEntity* sceneEntity = FindEntityStorage(entity);
        if (!sceneEntity || !sceneEntity->Light || !IsValidLightComponent(light))
            return false;

        sceneEntity->Light = light;
        return true;
    }

    bool Scene::RemoveLightComponent(Entity entity)
    {
        SceneEntity* sceneEntity = FindEntityStorage(entity);
        if (!sceneEntity || !sceneEntity->Light)
            return false;

        sceneEntity->Light.reset();
        return true;
    }

    MeshRendererComponent* Scene::AddMeshRendererComponent(Entity entity, const MeshRendererComponent& meshRenderer)
    {
        SceneEntity* sceneEntity = FindEntityStorage(entity);
        if (!sceneEntity)
            return nullptr;

        sceneEntity->MeshRenderer = meshRenderer;
        return &(*sceneEntity->MeshRenderer);
    }

    MeshRendererComponent* Scene::TryGetMeshRendererComponent(Entity entity)
    {
        SceneEntity* sceneEntity = FindEntityStorage(entity);
        return sceneEntity && sceneEntity->MeshRenderer ? &(*sceneEntity->MeshRenderer) : nullptr;
    }

    const MeshRendererComponent* Scene::TryGetMeshRendererComponent(Entity entity) const
    {
        const SceneEntity* sceneEntity = FindEntityStorage(entity);
        return sceneEntity && sceneEntity->MeshRenderer ? &(*sceneEntity->MeshRenderer) : nullptr;
    }

    bool Scene::RemoveMeshRendererComponent(Entity entity)
    {
        SceneEntity* sceneEntity = FindEntityStorage(entity);
        if (!sceneEntity || !sceneEntity->MeshRenderer)
            return false;

        sceneEntity->MeshRenderer.reset();
        return true;
    }

    bool Scene::SetMainCameraEntity(Entity entity)
    {
        SceneEntity* sceneEntity = FindEntityStorage(entity);
        if (!sceneEntity || !sceneEntity->Camera)
            return false;

        m_MainCameraEntity = entity;
        SyncMainCameraCacheFromEntity();
        return true;
    }

    const TransformComponent& Scene::GetMainCameraTransform() const
    {
        const SceneEntity* sceneEntity = FindEntityStorage(m_MainCameraEntity);
        static const TransformComponent emptyTransform;
        return sceneEntity ? sceneEntity->Transform : emptyTransform;
    }

    bool Scene::SetMainCameraTransform(const TransformComponent& transform)
    {
        return SetEntityTransform(m_MainCameraEntity, transform.GetPosition(),
            transform.RotationDegrees, transform.Scale);
    }

    bool Scene::SetMainCamera(const CameraComponent& camera)
    {
        if (!IsValidCameraComponent(camera))
            return false;

        m_MainCamera = camera;
        if (SceneEntity* sceneEntity = FindEntityStorage(m_MainCameraEntity))
            sceneEntity->Camera = camera;
        return true;
    }

    bool Scene::SaveToFile(const std::filesystem::path& path) const
    {
        if (!Math::IsWorldGridPolicyValid(m_WorldGridPolicy))
        {
            Log::Error("Could not save scene with an invalid world-grid policy: ", path.string());
            return false;
        }
        // Refuse before touching the destination anything the loader would not
        // accept, so a bad in-memory value can never replace the last good file
        // with one that can no longer be loaded. The invariants are the same ones
        // load and RestoreEntity enforce.
        if (ContainsLineBreak(m_Name) || !IsValidCameraComponent(m_MainCamera))
        {
            Log::Error("Could not save scene with an unloadable name or main camera: ", path.string());
            return false;
        }
        {
            const SceneEntity* mainCamera = FindEntityStorage(m_MainCameraEntity);
            if (m_MainCameraEntity && (!mainCamera || !mainCamera->Camera))
            {
                Log::Error("Could not save scene whose main camera entity has no camera: ", path.string());
                return false;
            }
        }
        for (const SceneEntity& entity : m_Entities)
        {
            if (!Math::IsCanonical(entity.Transform.GetPosition(), m_WorldGridPolicy))
            {
                Log::Error("Could not save scene with a noncanonical transform: ", entity.Name);
                return false;
            }
            if (entity.Light && !IsValidLightComponent(*entity.Light))
            {
                Log::Error("Could not save scene with invalid photometric light data: ", entity.Name);
                return false;
            }
            if (!IsPersistableEntity(entity, m_WorldGridPolicy))
            {
                Log::Error("Could not save scene with an entity that would not load back "
                    "(non-finite or non-positive transform, invalid camera, or a line break in a name): ", entity.Name);
                return false;
            }
        }

        std::ostringstream output;
        output << std::setprecision(std::numeric_limits<double>::max_digits10);
        output << "SpiralScene " << kSceneFormatVersion << '\n';
        output << "Name " << std::quoted(m_Name) << '\n';
        output << '\n';
        output << "[WorldGrid]\n";
        output << "Version " << m_WorldGridPolicy.Version << '\n';
        output << "SectorExtent " << m_WorldGridPolicy.SectorExtent << '\n';
        output << "OriginHysteresis " << m_WorldGridPolicy.OriginHysteresis << '\n';
        output << "OriginMode " << ToString(m_WorldGridPolicy.OriginMode) << '\n';
        output << '\n';
        output << "[MainCamera]\n";
        output << "Primary " << (m_MainCamera.Primary ? "true" : "false") << '\n';
        output << "VerticalFovDegrees " << m_MainCamera.Projection.VerticalFovDegrees << '\n';
        output << "NearClip " << m_MainCamera.Projection.NearClip << '\n';
        output << "FarClip " << m_MainCamera.Projection.FarClip << '\n';
        WriteVec3(output, "BackgroundColor", m_MainCamera.BackgroundColor);
        output << '\n';
        output << "[Entities]\n";
        output << "NextEntityId " << m_NextEntityId << '\n';
        output << "MainCameraEntity " << m_MainCameraEntity.Id << '\n';
        for (const SceneEntity& entity : m_Entities)
        {
            const Math::SectorLocalPosition& position = entity.Transform.GetPosition();
            output << "Entity " << entity.EntityHandle.Id << ' ' << std::quoted(entity.Name) << '\n';
            output << "Transform " << entity.EntityHandle.Id
                << ' ' << position.Sector.X << ' ' << position.Sector.Y << ' ' << position.Sector.Z
                << ' ' << position.Local.X << ' ' << position.Local.Y << ' ' << position.Local.Z
                << ' ' << entity.Transform.RotationDegrees.X
                << ' ' << entity.Transform.RotationDegrees.Y
                << ' ' << entity.Transform.RotationDegrees.Z
                << ' ' << entity.Transform.Scale.X << ' ' << entity.Transform.Scale.Y << ' ' << entity.Transform.Scale.Z << '\n';

            if (entity.Camera)
            {
                output << "Camera " << entity.EntityHandle.Id
                    << ' ' << (entity.Camera->Primary ? "true" : "false")
                    << ' ' << entity.Camera->Projection.VerticalFovDegrees
                    << ' ' << entity.Camera->Projection.NearClip
                    << ' ' << entity.Camera->Projection.FarClip
                    << ' ' << entity.Camera->BackgroundColor.X
                    << ' ' << entity.Camera->BackgroundColor.Y
                    << ' ' << entity.Camera->BackgroundColor.Z << '\n';
            }

            if (entity.Light)
            {
                output << "Light " << entity.EntityHandle.Id
                    << ' ' << ToString(entity.Light->Type)
                    << ' ' << entity.Light->Color.X
                    << ' ' << entity.Light->Color.Y
                    << ' ' << entity.Light->Color.Z
                    << ' ' << entity.Light->PhotometricValue
                    << ' ' << ToString(entity.Light->PhotometricUnit)
                    << ' ' << entity.Light->Range
                    << ' ' << entity.Light->InnerConeDegrees
                    << ' ' << entity.Light->OuterConeDegrees
                    << ' ' << (entity.Light->CastsShadows ? "true" : "false") << '\n';
            }

            if (entity.MeshRenderer)
            {
                output << "MeshRenderer " << entity.EntityHandle.Id
                    << ' ' << entity.MeshRenderer->MeshAsset
                    << ' ' << entity.MeshRenderer->MaterialAsset
                    << ' ' << std::quoted(entity.MeshRenderer->MeshName)
                    << ' ' << (entity.MeshRenderer->Visible ? "true" : "false")
                    << ' ' << (entity.MeshRenderer->CastsShadows ? "true" : "false") << '\n';
            }
        }

        std::string writeError;
        if (!output || !WriteFileAtomically(path, output.str(), writeError))
        {
            Log::Error("Could not atomically save scene file: ", path.string(), " (", writeError, ")");
            return false;
        }
        return true;
    }

    bool Scene::LoadFromFile(const std::filesystem::path& path, Scene& outScene)
    {
        std::ifstream input(path);
        if (!input)
        {
            Log::Error("Could not open scene file for reading: ", path.string());
            return false;
        }

        std::string magic;
        int version = 0;
        if (!(input >> magic >> version) || magic != "SpiralScene" || version < 1 || version > kSceneFormatVersion)
        {
            Log::Error("Unsupported scene file format: ", path.string());
            return false;
        }

        std::string line;
        std::getline(input, line);

        Scene scene;
        TransformComponent cameraTransform;
        CameraComponent camera;
        bool parsedLegacyCameraTransform = false;
        bool parsedEntities = false;
        Entity parsedMainCameraEntity;
        bool parsedMainCameraKey = false;
        EntityId parsedNextEntityId = 1;
        std::string section;
        size_t lineNumber = 1;
        Math::WorldGridPolicy parsedWorldGridPolicy;
        bool parsedWorldGridVersion = false;
        bool parsedSectorExtent = false;
        bool parsedOriginHysteresis = false;
        bool parsedOriginMode = false;
        bool appliedWorldGridPolicy = version < 4;

        const auto fail = [&](std::string_view message)
        {
            Log::Error("Could not parse scene file '", path.string(), "' at line ", lineNumber, ": ", message);
            return false;
        };

        const auto applyWorldGridPolicy = [&]()
        {
            if (appliedWorldGridPolicy)
                return true;
            if (!parsedWorldGridVersion
                || !parsedSectorExtent
                || !parsedOriginHysteresis
                || !parsedOriginMode
                || !Math::IsWorldGridPolicyValid(parsedWorldGridPolicy))
            {
                return false;
            }

            Math::DVec3 defaultCameraPosition;
            if (!scene.TryGetEntityApproximateWorldPosition(scene.m_MainCameraEntity, defaultCameraPosition))
                return false;

            scene.m_WorldGridPolicy = parsedWorldGridPolicy;
            if (!scene.SetEntityWorldPosition(scene.m_MainCameraEntity, defaultCameraPosition))
                return false;

            appliedWorldGridPolicy = true;
            return true;
        };

        while (std::getline(input, line))
        {
            ++lineNumber;
            if (line.size() > kMaximumSceneLineBytes)
                return fail("line exceeds the maximum scene line length");
            if (line.empty())
                continue;

            if (line.front() == '[' && line.back() == ']')
            {
                if (section == "WorldGrid" && !applyWorldGridPolicy())
                    return fail("invalid or incomplete WorldGrid policy");
                const std::string nextSection = line.substr(1, line.size() - 2);
                if (version >= 4 && nextSection != "WorldGrid" && !appliedWorldGridPolicy)
                    return fail("WorldGrid policy must precede version 4 scene sections");
                section = nextSection;
                continue;
            }

            std::istringstream stream(line);
            std::string key;
            stream >> key;
            if (key.empty())
                continue;

            if (section.empty() && key == "Name")
            {
                if (!(stream >> std::quoted(scene.m_Name)))
                    return fail("invalid scene name");
            }
            else if (section == "WorldGrid")
            {
                if (version < 4)
                    return fail("WorldGrid is unsupported before scene format version 4");

                if (key == "Version")
                {
                    if (parsedWorldGridVersion || !(stream >> parsedWorldGridPolicy.Version))
                        return fail("invalid or duplicate WorldGrid.Version value");
                    parsedWorldGridVersion = true;
                }
                else if (key == "SectorExtent")
                {
                    if (parsedSectorExtent || !(stream >> parsedWorldGridPolicy.SectorExtent))
                        return fail("invalid or duplicate WorldGrid.SectorExtent value");
                    parsedSectorExtent = true;
                }
                else if (key == "OriginHysteresis")
                {
                    if (parsedOriginHysteresis || !(stream >> parsedWorldGridPolicy.OriginHysteresis))
                        return fail("invalid or duplicate WorldGrid.OriginHysteresis value");
                    parsedOriginHysteresis = true;
                }
                else if (key == "OriginMode")
                {
                    std::string value;
                    if (parsedOriginMode
                        || !(stream >> value)
                        || !ParseWorldOriginMode(value, parsedWorldGridPolicy.OriginMode))
                    {
                        return fail("invalid or duplicate WorldGrid.OriginMode value");
                    }
                    parsedOriginMode = true;
                }
                else
                    return fail("unknown WorldGrid field");
            }
            else if (section == "MainCamera")
            {
                if (key == "Primary")
                {
                    std::string value;
                    if (!(stream >> value) || !ParseBoolean(value, camera.Primary))
                        return fail("invalid MainCamera.Primary value");
                }
                else if (key == "VerticalFovDegrees")
                {
                    if (!(stream >> camera.Projection.VerticalFovDegrees))
                        return fail("invalid MainCamera.VerticalFovDegrees value");
                }
                else if (key == "NearClip")
                {
                    if (!(stream >> camera.Projection.NearClip))
                        return fail("invalid MainCamera.NearClip value");
                }
                else if (key == "FarClip")
                {
                    if (!(stream >> camera.Projection.FarClip))
                        return fail("invalid MainCamera.FarClip value");
                }
                else if (key == "BackgroundColor")
                {
                    if (version < 2 || !ReadVec3(stream, camera.BackgroundColor))
                        return fail("invalid MainCamera.BackgroundColor value");
                }
                else
                    return fail("unknown MainCamera field");
            }
            else if (section == "MainCamera.Transform")
            {
                if (version >= 4)
                    return fail("MainCamera.Transform is legacy duplicated state in scene format version 4 or newer");

                parsedLegacyCameraTransform = true;
                bool parsed = false;
                if (key == "Position")
                {
                    Math::DVec3 position;
                    parsed = ReadDVec3(stream, position)
                        && cameraTransform.SetWorldPosition(position, scene.m_WorldGridPolicy);
                }
                else if (key == "RotationDegrees")
                    parsed = ReadVec3(stream, cameraTransform.RotationDegrees);
                else if (key == "Scale")
                    parsed = ReadVec3(stream, cameraTransform.Scale);
                else
                    return fail("unknown MainCamera.Transform field");

                if (!parsed)
                    return fail("invalid MainCamera.Transform value");
            }
            else if (section == "Entities")
            {
                if (!parsedEntities)
                {
                    scene.m_Entities.clear();
                    scene.m_EntityIndexById.clear();
                    scene.m_MainCameraEntity = {};
                    parsedEntities = true;
                }

                if (key == "NextEntityId")
                {
                    if (!ReadUnsigned32(stream, parsedNextEntityId) || parsedNextEntityId == kInvalidEntityId)
                        return fail("invalid NextEntityId value");
                }
                else if (key == "MainCameraEntity")
                {
                    if (parsedMainCameraKey || !ReadUnsigned32(stream, parsedMainCameraEntity.Id))
                        return fail("invalid or duplicate MainCameraEntity value");
                    parsedMainCameraKey = true;
                }
                else if (key == "Entity")
                {
                    EntityId id = kInvalidEntityId;
                    std::string entityName;
                    if (!ReadUnsigned32(stream, id) || !(stream >> std::quoted(entityName))
                        || !scene.CreateEntityWithId(id, std::move(entityName)))
                        return fail("invalid or duplicate Entity record");
                }
                else if (key == "Transform")
                {
                    Entity entity;
                    if (!ReadUnsigned32(stream, entity.Id))
                        return fail("invalid Transform entity ID");

                    // Parse into a scratch transform and validate before storing:
                    // the same invariants SetEntityTransform enforces, including
                    // the unit scale of a camera that an earlier record attached.
                    SceneEntity* sceneEntity = scene.FindEntityStorage(entity);
                    bool parsedTransform = sceneEntity != nullptr;
                    TransformComponent parsed;
                    if (parsedTransform && version >= 4)
                    {
                        Math::SectorLocalPosition position;
                        parsedTransform = static_cast<bool>(stream
                                >> position.Sector.X >> position.Sector.Y >> position.Sector.Z
                                >> position.Local.X >> position.Local.Y >> position.Local.Z
                                >> parsed.RotationDegrees.X >> parsed.RotationDegrees.Y >> parsed.RotationDegrees.Z
                                >> parsed.Scale.X >> parsed.Scale.Y >> parsed.Scale.Z)
                            && Math::IsCanonical(position, scene.m_WorldGridPolicy)
                            && parsed.SetPosition(position, scene.m_WorldGridPolicy);
                    }
                    else if (parsedTransform)
                    {
                        Math::DVec3 position;
                        parsedTransform = static_cast<bool>(stream
                                >> position.X >> position.Y >> position.Z
                                >> parsed.RotationDegrees.X >> parsed.RotationDegrees.Y >> parsed.RotationDegrees.Z
                                >> parsed.Scale.X >> parsed.Scale.Y >> parsed.Scale.Z)
                            && parsed.SetWorldPosition(position, scene.m_WorldGridPolicy);
                    }

                    parsedTransform = parsedTransform
                        && IsFinite(parsed.RotationDegrees)
                        && HasStrictlyPositiveScale(parsed.Scale)
                        && (!sceneEntity->Camera || HasUnitScale(parsed.Scale));
                    if (!parsedTransform)
                        return fail("invalid Transform record or unknown entity");
                    sceneEntity->Transform = parsed;
                }
                else if (key == "Camera")
                {
                    Entity entity;
                    std::string primary;
                    CameraComponent entityCamera;
                    if (!ReadUnsigned32(stream, entity.Id) || !(stream >> primary
                            >> entityCamera.Projection.VerticalFovDegrees
                            >> entityCamera.Projection.NearClip
                            >> entityCamera.Projection.FarClip)
                        || (version >= 2 && !(stream
                            >> entityCamera.BackgroundColor.X
                            >> entityCamera.BackgroundColor.Y
                            >> entityCamera.BackgroundColor.Z))
                        || !ParseBoolean(primary, entityCamera.Primary)
                        || !scene.AddCameraComponent(entity, entityCamera))
                        return fail("invalid Camera record or unknown entity");
                }
                else if (key == "Light")
                {
                    Entity entity;
                    std::string type;
                    std::string photometricUnit;
                    std::string castsShadows;
                    LightComponent light;
                    double legacyIntensity = 0.0;
                    const bool commonPrefix = ReadUnsigned32(stream, entity.Id)
                        && static_cast<bool>(stream >> type >> light.Color.X >> light.Color.Y >> light.Color.Z)
                        && TryParseLightType(type, light.Type);
                    const bool photometricParsed = version >= 5
                        ? static_cast<bool>(stream >> light.PhotometricValue >> photometricUnit)
                            && TryParseLightPhotometricUnit(photometricUnit, light.PhotometricUnit)
                        : static_cast<bool>(stream >> legacyIntensity)
                            && TryMigrateLegacyLightIntensity(light.Type, legacyIntensity,
                                light.PhotometricValue, light.PhotometricUnit);
                    if (!commonPrefix || !photometricParsed
                        || !(stream >> light.Range >> light.InnerConeDegrees
                            >> light.OuterConeDegrees >> castsShadows)
                        || !ParseBoolean(castsShadows, light.CastsShadows)
                        || scene.TryGetLightComponent(entity)
                        || !scene.AddLightComponent(entity, light))
                        return fail("invalid Light record or unknown entity");
                }
                else if (key == "MeshRenderer")
                {
                    Entity entity;
                    std::string visible;
                    std::string castsShadows;
                    MeshRendererComponent meshRenderer;
                    if (!ReadUnsigned32(stream, entity.Id)
                        || !(stream >> meshRenderer.MeshAsset
                            >> meshRenderer.MaterialAsset
                            >> std::quoted(meshRenderer.MeshName)
                            >> visible
                            >> castsShadows)
                        || !ParseBoolean(visible, meshRenderer.Visible)
                        || !ParseBoolean(castsShadows, meshRenderer.CastsShadows)
                        || !scene.AddMeshRendererComponent(entity, meshRenderer))
                        return fail("invalid MeshRenderer record or unknown entity");
                }
                else
                    return fail("unknown Entities field");
            }
            else
                return fail("field appears in an unknown section");

            stream >> std::ws;
            if (!stream.eof())
                return fail("unexpected trailing data");
        }

        if (input.bad())
            return fail("I/O error while reading scene");

        if (!applyWorldGridPolicy())
            return fail("missing or invalid WorldGrid policy");

        if (parsedEntities)
        {
            scene.m_NextEntityId = std::max(scene.m_NextEntityId, parsedNextEntityId);
            if (parsedMainCameraKey && !parsedMainCameraEntity.IsValid())
            {
                // An explicit "no main camera" (the main camera's component was
                // removed) is the saved state: do not promote a camera, and drop
                // the election Camera records may have made while loading.
                scene.m_MainCameraEntity = {};
            }
            else if (!scene.SetMainCameraEntity(parsedMainCameraEntity))
            {
                for (const SceneEntity& sceneEntity : scene.m_Entities)
                {
                    if (sceneEntity.Camera)
                    {
                        scene.SetMainCameraEntity(sceneEntity.EntityHandle);
                        break;
                    }
                }
            }

            if (!scene.m_MainCameraEntity)
            {
                if (!scene.SetMainCamera(camera))
                    return fail("invalid MainCamera values");
                if (scene.m_MainCameraEntity)
                {
                    if (!scene.SetMainCameraTransform(cameraTransform))
                        return fail("invalid legacy MainCamera.Transform");
                }
                else if (parsedLegacyCameraTransform
                    && (!Math::IsCanonical(cameraTransform.GetPosition(), scene.m_WorldGridPolicy)
                        || !IsFinite(cameraTransform.RotationDegrees)
                        || !HasUnitScale(cameraTransform.Scale)))
                {
                    return fail("invalid legacy MainCamera.Transform");
                }
            }
        }
        else
        {
            if (!scene.SetMainCamera(camera))
                return fail("invalid MainCamera values");
            if (!scene.SetMainCameraTransform(cameraTransform))
                return fail("invalid legacy MainCamera.Transform");
        }

        // One validator owns the entity invariants, whatever order the records
        // arrived in (a Camera record before its Transform, for example).
        for (const SceneEntity& sceneEntity : scene.m_Entities)
        {
            if (!IsInsertableEntity(sceneEntity, scene.m_WorldGridPolicy))
                return fail("an entity violates the transform, camera, or light invariants");
        }

        outScene = std::move(scene);
        return true;
    }

    SceneEntity* Scene::FindEntityStorage(Entity entity)
    {
        return const_cast<SceneEntity*>(std::as_const(*this).FindEntityStorage(entity));
    }

    const SceneEntity* Scene::FindEntityStorage(Entity entity) const
    {
        if (!entity)
            return nullptr;

        const auto it = m_EntityIndexById.find(entity.Id);
        return it == m_EntityIndexById.end() ? nullptr : &m_Entities[it->second];
    }

    void Scene::RebuildEntityIndexFrom(size_t first)
    {
        for (size_t index = first; index < m_Entities.size(); ++index)
            m_EntityIndexById[m_Entities[index].EntityHandle.Id] = index;
    }

    void Scene::SyncMainCameraCacheFromEntity()
    {
        const SceneEntity* sceneEntity = FindEntityStorage(m_MainCameraEntity);
        if (!sceneEntity || !sceneEntity->Camera)
            return;

        m_MainCamera = *sceneEntity->Camera;
    }

    bool Scene::InsertEntity(SceneEntity&& entity, size_t index)
    {
        if (index > m_Entities.size() || !IsInsertableEntity(entity, m_WorldGridPolicy)
            || FindEntityStorage(entity.EntityHandle))
        {
            return false;
        }

        const EntityId id = entity.EntityHandle.Id;
        m_Entities.insert(m_Entities.begin() + static_cast<std::ptrdiff_t>(index), std::move(entity));
        RebuildEntityIndexFrom(index);
        RaiseNextEntityId(m_NextEntityId, id);
        return true;
    }

    Entity Scene::CreateEntityWithId(EntityId id, std::string name)
    {
        if (id == kInvalidEntityId)
            return {};

        Entity entity { id };
        if (FindEntityStorage(entity))
            return {};

        SceneEntity sceneEntity;
        sceneEntity.EntityHandle = entity;
        sceneEntity.Name = std::move(name);
        m_Entities.push_back(std::move(sceneEntity));
        m_EntityIndexById[id] = m_Entities.size() - 1;
        RaiseNextEntityId(m_NextEntityId, id);
        return entity;
    }
}
