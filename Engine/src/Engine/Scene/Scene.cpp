#include "Engine/Scene/Scene.h"

#include "Engine/Core/Log.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace Engine
{
    namespace
    {
        constexpr int kSceneFormatVersion = 1;

        void WriteVec3(std::ostream& stream, std::string_view name, const Math::Vec3& value)
        {
            stream << name << ' ' << value.X << ' ' << value.Y << ' ' << value.Z << '\n';
        }

        bool ReadVec3(std::istringstream& stream, Math::Vec3& outValue)
        {
            return static_cast<bool>(stream >> outValue.X >> outValue.Y >> outValue.Z);
        }
    }

    Scene::Scene(std::string name)
        : m_Name(std::move(name))
    {
        m_MainCameraTransform.Position = { 0.0f, 0.0f, -3.35f };
    }

    void Scene::OnUpdate(Timestep timestep)
    {
        (void)timestep;
    }

    void Scene::SetMainCameraTransform(const TransformComponent& transform)
    {
        m_MainCameraTransform = transform;
    }

    void Scene::SetMainCamera(const CameraComponent& camera)
    {
        m_MainCamera = camera;
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
        output << '\n';
        output << "[MainCamera.Transform]\n";
        WriteVec3(output, "Position", m_MainCameraTransform.Position);
        WriteVec3(output, "RotationDegrees", m_MainCameraTransform.RotationDegrees);
        WriteVec3(output, "Scale", m_MainCameraTransform.Scale);

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
        input >> magic >> version;
        if (magic != "SpiralScene" || version != kSceneFormatVersion)
        {
            Log::Error("Unsupported scene file format: ", path.string());
            return false;
        }

        std::string line;
        std::getline(input, line);

        Scene scene;
        TransformComponent cameraTransform;
        CameraComponent camera;
        std::string section;

        while (std::getline(input, line))
        {
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
                stream >> std::quoted(scene.m_Name);
            }
            else if (section == "MainCamera")
            {
                if (key == "Primary")
                {
                    std::string value;
                    stream >> value;
                    camera.Primary = value == "true";
                }
                else if (key == "VerticalFovDegrees")
                {
                    stream >> camera.Projection.VerticalFovDegrees;
                }
                else if (key == "NearClip")
                {
                    stream >> camera.Projection.NearClip;
                }
                else if (key == "FarClip")
                {
                    stream >> camera.Projection.FarClip;
                }
            }
            else if (section == "MainCamera.Transform")
            {
                if (key == "Position")
                    ReadVec3(stream, cameraTransform.Position);
                else if (key == "RotationDegrees")
                    ReadVec3(stream, cameraTransform.RotationDegrees);
                else if (key == "Scale")
                    ReadVec3(stream, cameraTransform.Scale);
            }
        }

        scene.SetMainCamera(camera);
        scene.SetMainCameraTransform(cameraTransform);
        outScene = std::move(scene);
        return true;
    }
}
