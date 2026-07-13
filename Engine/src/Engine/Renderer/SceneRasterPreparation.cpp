#include "Engine/Renderer/SceneRasterPreparation.h"

namespace Engine
{
    SceneRasterFrame PrepareSceneRasterFrame(const SceneRenderSnapshot& snapshot, size_t viewIndex)
    {
        SceneRasterFrame frame;
        frame.SnapshotFrameIndex = snapshot.FrameIndex;
        if (viewIndex >= snapshot.Views.size() || !snapshot.Views[viewIndex].Camera.Valid)
            return frame;

        const CameraView& view = snapshot.Views[viewIndex].Camera;
        frame.TranslationOrigin = view.TranslationOrigin;
        frame.HasValidView = true;
        frame.Instances.reserve(snapshot.Meshes.size());

        for (const SceneRenderMesh& mesh : snapshot.Meshes)
        {
            SceneRasterInstance instance;
            instance.SourceEntity = mesh.SourceEntity;
            instance.MeshAsset = mesh.MeshAsset;
            instance.MaterialAsset = mesh.MaterialAsset;
            instance.WorldPosition = mesh.Transform.WorldPosition;
            instance.TranslationOrigin = view.TranslationOrigin;
            instance.CameraRelativePosition = Math::CameraRelative(
                mesh.Transform.WorldPosition,
                view.TranslationOrigin);

            const Math::Mat4 scale = Math::Scale(mesh.Transform.Scale);
            const Math::Mat4 rotation = Math::RotationYawPitchRoll(
                Math::DegreesToRadians(mesh.Transform.RotationDegrees.Y),
                Math::DegreesToRadians(mesh.Transform.RotationDegrees.X),
                Math::DegreesToRadians(mesh.Transform.RotationDegrees.Z));
            const Math::Mat4 translation = Math::Translation(instance.CameraRelativePosition);
            instance.CameraRelativeModel = Math::Multiply(Math::Multiply(scale, rotation), translation);
            instance.ModelViewProjection = Math::Multiply(instance.CameraRelativeModel, view.ViewProjection);
            frame.Instances.push_back(instance);
        }

        return frame;
    }
}
