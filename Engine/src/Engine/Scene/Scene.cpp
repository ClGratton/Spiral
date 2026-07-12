#include "Engine/Scene/Scene.h"

#include "Engine/Core/Log.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace Engine
{
    namespace
    {
        constexpr int kSceneFormatVersion = 2;

        void WriteVec3(std::ostream& stream, std::string_view name, const Math::Vec3& value)
        {
            stream << name << ' ' << value.X << ' ' << value.Y << ' ' << value.Z << '\n';
        }

        bool ReadVec3(std::istringstream& stream, Math::Vec3& outValue)
        {
            return static_cast<bool>(stream >> outValue.X >> outValue.Y >> outValue.Z);
        }

        const char* ToString(LightType type)
        {
            switch (type)
            {
                case LightType::Directional: return "Directional";
                case LightType::Point: return "Point";
                case LightType::Spot: return "Spot";
            }

            return "Directional";
        }

        bool ParseLightType(std::string_view value, LightType& outType)
        {
            if (value == "Directional")
            {
                outType = LightType::Directional;
                return true;
            }
            if (value == "Point")
            {
                outType = LightType::Point;
                return true;
            }
            if (value == "Spot")
            {
                outType = LightType::Spot;
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

        SceneEntity* FindEntityInList(std::vector<SceneEntity>& entities, Entity entity)
        {
            const auto it = std::find_if(entities.begin(), entities.end(), [entity](const SceneEntity& candidate)
            {
                return candidate.EntityHandle == entity;
            });

            return it == entities.end() ? nullptr : &(*it);
        }
    }

    Scene::Scene(std::string name)
        : m_Name(std::move(name))
    {
        m_MainCameraTransform.Position = { 0.0f, 0.0f, -3.35f };

        m_MainCameraEntity = CreateEntity("Main Camera");
        SetMainCameraTransform(m_MainCameraTransform);
        SetMainCamera(m_MainCamera);
    }

    void Scene::OnUpdate(Timestep timestep)
    {
        (void)timestep;
    }

    Entity Scene::CreateEntity(std::string name)
    {
        return CreateEntityWithId(m_NextEntityId++, std::move(name));
    }

    bool Scene::DestroyEntity(Entity entity)
    {
        const auto it = std::find_if(m_Entities.begin(), m_Entities.end(), [entity](const SceneEntity& candidate)
        {
            return candidate.EntityHandle == entity;
        });

        if (it == m_Entities.end())
            return false;

        const bool destroyedMainCamera = it->EntityHandle == m_MainCameraEntity;
        m_Entities.erase(it);

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

    CameraComponent* Scene::AddCameraComponent(Entity entity, const CameraComponent& camera)
    {
        SceneEntity* sceneEntity = FindEntityStorage(entity);
        if (!sceneEntity)
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
        if (!sceneEntity)
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

    void Scene::SetMainCameraTransform(const TransformComponent& transform)
    {
        m_MainCameraTransform = transform;
        if (SceneEntity* sceneEntity = FindEntityStorage(m_MainCameraEntity))
            sceneEntity->Transform = transform;
    }

    void Scene::SetMainCamera(const CameraComponent& camera)
    {
        m_MainCamera = camera;
        if (SceneEntity* sceneEntity = FindEntityStorage(m_MainCameraEntity))
            sceneEntity->Camera = camera;
    }

    bool Scene::SaveToFile(const std::filesystem::path& path) const
    {
        std::error_code error;
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, error);

        if (error)
        {
            Log::Error("Could not create scene directory: ", parent.string(), " (", error.message(), ")");
            return false;
        }

        std::ofstream output(path, std::ios::out | std::ios::trunc);
        if (!output)
        {
            Log::Error("Could not open scene file for writing: ", path.string());
            return false;
        }

        output << std::setprecision(9);
        output << "SpiralScene " << kSceneFormatVersion << '\n';
        output << "Name " << std::quoted(m_Name) << '\n';
        output << '\n';
        output << "[MainCamera]\n";
        output << "Primary " << (m_MainCamera.Primary ? "true" : "false") << '\n';
        output << "VerticalFovDegrees " << m_MainCamera.Projection.VerticalFovDegrees << '\n';
        output << "NearClip " << m_MainCamera.Projection.NearClip << '\n';
        output << "FarClip " << m_MainCamera.Projection.FarClip << '\n';
        WriteVec3(output, "BackgroundColor", m_MainCamera.BackgroundColor);
        output << '\n';
        output << "[MainCamera.Transform]\n";
        WriteVec3(output, "Position", m_MainCameraTransform.Position);
        WriteVec3(output, "RotationDegrees", m_MainCameraTransform.RotationDegrees);
        WriteVec3(output, "Scale", m_MainCameraTransform.Scale);
        output << '\n';
        output << "[Entities]\n";
        output << "NextEntityId " << m_NextEntityId << '\n';
        output << "MainCameraEntity " << m_MainCameraEntity.Id << '\n';
        for (const SceneEntity& entity : m_Entities)
        {
            output << "Entity " << entity.EntityHandle.Id << ' ' << std::quoted(entity.Name) << '\n';
            output << "Transform " << entity.EntityHandle.Id
                << ' ' << entity.Transform.Position.X << ' ' << entity.Transform.Position.Y << ' ' << entity.Transform.Position.Z
                << ' ' << entity.Transform.RotationDegrees.X << ' ' << entity.Transform.RotationDegrees.Y << ' ' << entity.Transform.RotationDegrees.Z
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
                    << ' ' << entity.Light->Intensity
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
        bool parsedEntities = false;
        Entity parsedMainCameraEntity;
        EntityId parsedNextEntityId = 1;
        std::string section;
        size_t lineNumber = 1;

        const auto fail = [&](std::string_view message)
        {
            Log::Error("Could not parse scene file '", path.string(), "' at line ", lineNumber, ": ", message);
            return false;
        };

        while (std::getline(input, line))
        {
            ++lineNumber;
            if (line.empty())
                continue;

            if (line.front() == '[' && line.back() == ']')
            {
                section = line.substr(1, line.size() - 2);
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
                bool parsed = false;
                if (key == "Position")
                    parsed = ReadVec3(stream, cameraTransform.Position);
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
                    scene.m_MainCameraEntity = {};
                    parsedEntities = true;
                }

                if (key == "NextEntityId")
                {
                    if (!(stream >> parsedNextEntityId) || parsedNextEntityId == kInvalidEntityId)
                        return fail("invalid NextEntityId value");
                }
                else if (key == "MainCameraEntity")
                {
                    if (!(stream >> parsedMainCameraEntity.Id))
                        return fail("invalid MainCameraEntity value");
                }
                else if (key == "Entity")
                {
                    EntityId id = kInvalidEntityId;
                    std::string entityName;
                    if (!(stream >> id >> std::quoted(entityName)) || !scene.CreateEntityWithId(id, std::move(entityName)))
                        return fail("invalid or duplicate Entity record");
                }
                else if (key == "Transform")
                {
                    Entity entity;
                    if (!(stream >> entity.Id))
                        return fail("invalid Transform entity ID");

                    SceneEntity* sceneEntity = scene.FindEntityStorage(entity);
                    if (!sceneEntity
                        || !(stream
                            >> sceneEntity->Transform.Position.X >> sceneEntity->Transform.Position.Y >> sceneEntity->Transform.Position.Z
                            >> sceneEntity->Transform.RotationDegrees.X >> sceneEntity->Transform.RotationDegrees.Y >> sceneEntity->Transform.RotationDegrees.Z
                            >> sceneEntity->Transform.Scale.X >> sceneEntity->Transform.Scale.Y >> sceneEntity->Transform.Scale.Z))
                        return fail("invalid Transform record or unknown entity");
                }
                else if (key == "Camera")
                {
                    Entity entity;
                    std::string primary;
                    CameraComponent entityCamera;
                    if (!(stream >> entity.Id >> primary
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
                    std::string castsShadows;
                    LightComponent light;
                    if (!(stream >> entity.Id >> type
                            >> light.Color.X
                            >> light.Color.Y
                            >> light.Color.Z
                            >> light.Intensity
                            >> light.Range
                            >> light.InnerConeDegrees
                            >> light.OuterConeDegrees
                            >> castsShadows)
                        || !ParseLightType(type, light.Type)
                        || !ParseBoolean(castsShadows, light.CastsShadows)
                        || !scene.AddLightComponent(entity, light))
                        return fail("invalid Light record or unknown entity");
                }
                else if (key == "MeshRenderer")
                {
                    Entity entity;
                    std::string visible;
                    std::string castsShadows;
                    MeshRendererComponent meshRenderer;
                    if (!(stream >> entity.Id
                            >> meshRenderer.MeshAsset
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

        if (parsedEntities && !scene.m_Entities.empty())
        {
            scene.m_NextEntityId = std::max(scene.m_NextEntityId, parsedNextEntityId);
            if (!scene.SetMainCameraEntity(parsedMainCameraEntity))
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
                scene.SetMainCamera(camera);
                scene.SetMainCameraTransform(cameraTransform);
            }
        }
        else
        {
            scene.SetMainCamera(camera);
            scene.SetMainCameraTransform(cameraTransform);
        }

        outScene = std::move(scene);
        return true;
    }

    SceneEntity* Scene::FindEntityStorage(Entity entity)
    {
        return entity ? FindEntityInList(m_Entities, entity) : nullptr;
    }

    const SceneEntity* Scene::FindEntityStorage(Entity entity) const
    {
        const auto it = std::find_if(m_Entities.begin(), m_Entities.end(), [entity](const SceneEntity& candidate)
        {
            return candidate.EntityHandle == entity;
        });

        return it == m_Entities.end() ? nullptr : &(*it);
    }

    void Scene::SyncMainCameraCacheFromEntity()
    {
        const SceneEntity* sceneEntity = FindEntityStorage(m_MainCameraEntity);
        if (!sceneEntity || !sceneEntity->Camera)
            return;

        m_MainCameraTransform = sceneEntity->Transform;
        m_MainCamera = *sceneEntity->Camera;
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
        m_NextEntityId = std::max(m_NextEntityId, id + 1);
        return entity;
    }
}
