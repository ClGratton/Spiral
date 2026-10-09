#include "ScenePrerequisiteTests.h"

#include "TestSupport/GeneratedTest.h"

#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace SpiralTests
{
    namespace
    {
        using namespace Engine;

        struct Checker
        {
            bool Passed = true;

            void operator()(bool condition, std::string_view message)
            {
                if (!condition)
                {
                    std::cerr << "Scene prerequisite test failed: " << message << '\n';
                    Passed = false;
                }
            }
        };

        class TempDir
        {
        public:
            TempDir()
            {
                static std::atomic<unsigned> counter { 0 };
                std::error_code error;
                m_Path = std::filesystem::temp_directory_path(error)
                    / ("spiral-scene-prerequisites-" + std::to_string(counter.fetch_add(1)) + "-"
                        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
                std::filesystem::create_directories(m_Path, error);
            }

            ~TempDir()
            {
                std::error_code error;
                std::filesystem::remove_all(m_Path, error);
            }

            TempDir(const TempDir&) = delete;
            TempDir& operator=(const TempDir&) = delete;

            std::filesystem::path File(std::string_view name) const { return m_Path / std::string(name); }

        private:
            std::filesystem::path m_Path;
        };

        // Serialized form is the primary oracle: it is produced by the unmodified writer and
        // covers ids, order, every component field, the next-id counter and the main camera.
        std::optional<std::string> SaveBytes(const Scene& scene, const TempDir& dir)
        {
            const std::filesystem::path path = dir.File("scene.spiralscene");
            if (!scene.SaveToFile(path))
                return std::nullopt;
            std::ifstream input(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }

        std::optional<EntityId> NextIdFromBytes(const std::string& bytes)
        {
            const std::string key = "\nNextEntityId ";
            const size_t position = bytes.find(key);
            if (position == std::string::npos)
                return std::nullopt;
            return static_cast<EntityId>(std::stoull(bytes.substr(position + key.size())));
        }

        std::string WithNextId(std::string bytes, EntityId next)
        {
            const std::string key = "\nNextEntityId ";
            const size_t start = bytes.find(key) + key.size();
            const size_t end = bytes.find('\n', start);
            bytes.replace(start, end - start, std::to_string(next));
            return bytes;
        }

        bool SameBits(float a, float b)
        {
            return std::memcmp(&a, &b, sizeof(float)) == 0;
        }

        bool SameBits(double a, double b)
        {
            return std::memcmp(&a, &b, sizeof(double)) == 0;
        }

        bool SameVec(const Math::Vec3& a, const Math::Vec3& b)
        {
            return SameBits(a.X, b.X) && SameBits(a.Y, b.Y) && SameBits(a.Z, b.Z);
        }

        bool SameEntity(const SceneEntity& a, const SceneEntity& b)
        {
            const Math::SectorLocalPosition& pa = a.Transform.GetPosition();
            const Math::SectorLocalPosition& pb = b.Transform.GetPosition();
            if (a.EntityHandle != b.EntityHandle || a.Name != b.Name
                || !(pa.Sector == pb.Sector)
                || !SameBits(pa.Local.X, pb.Local.X) || !SameBits(pa.Local.Y, pb.Local.Y) || !SameBits(pa.Local.Z, pb.Local.Z)
                || !SameVec(a.Transform.RotationDegrees, b.Transform.RotationDegrees)
                || !SameVec(a.Transform.Scale, b.Transform.Scale)
                || a.Camera.has_value() != b.Camera.has_value()
                || a.Light.has_value() != b.Light.has_value()
                || a.MeshRenderer.has_value() != b.MeshRenderer.has_value())
            {
                return false;
            }
            if (a.Camera
                && (a.Camera->Primary != b.Camera->Primary
                    || !SameBits(a.Camera->Projection.VerticalFovDegrees, b.Camera->Projection.VerticalFovDegrees)
                    || !SameBits(a.Camera->Projection.NearClip, b.Camera->Projection.NearClip)
                    || !SameBits(a.Camera->Projection.FarClip, b.Camera->Projection.FarClip)
                    || !SameVec(a.Camera->BackgroundColor, b.Camera->BackgroundColor)))
            {
                return false;
            }
            if (a.Light
                && (a.Light->Type != b.Light->Type || a.Light->PhotometricUnit != b.Light->PhotometricUnit
                    || !SameVec(a.Light->Color, b.Light->Color)
                    || !SameBits(a.Light->PhotometricValue, b.Light->PhotometricValue)
                    || !SameBits(a.Light->Range, b.Light->Range)
                    || !SameBits(a.Light->InnerConeDegrees, b.Light->InnerConeDegrees)
                    || !SameBits(a.Light->OuterConeDegrees, b.Light->OuterConeDegrees)
                    || a.Light->CastsShadows != b.Light->CastsShadows))
            {
                return false;
            }
            return !a.MeshRenderer
                || (a.MeshRenderer->MeshAsset == b.MeshRenderer->MeshAsset
                    && a.MeshRenderer->MaterialAsset == b.MeshRenderer->MaterialAsset
                    && a.MeshRenderer->MeshName == b.MeshRenderer->MeshName
                    && a.MeshRenderer->Visible == b.MeshRenderer->Visible
                    && a.MeshRenderer->CastsShadows == b.MeshRenderer->CastsShadows);
        }

        bool SameEntities(const Scene& a, const Scene& b)
        {
            const std::vector<SceneEntity>& left = a.GetEntities();
            const std::vector<SceneEntity>& right = b.GetEntities();
            if (left.size() != right.size() || a.GetMainCameraEntity() != b.GetMainCameraEntity())
                return false;
            for (size_t index = 0; index < left.size(); ++index)
                if (!SameEntity(left[index], right[index]))
                    return false;
            return true;
        }

        bool IdsAreUnique(const Scene& scene)
        {
            std::set<EntityId> ids;
            for (const SceneEntity& entity : scene.GetEntities())
                if (!entity.EntityHandle.IsValid() || !ids.insert(entity.EntityHandle.Id).second)
                    return false;
            return true;
        }

        std::vector<EntityId> Ids(const Scene& scene)
        {
            std::vector<EntityId> ids;
            for (const SceneEntity& entity : scene.GetEntities())
                ids.push_back(entity.EntityHandle.Id);
            return ids;
        }

        bool RunProperty(std::string_view name, const Spiral::Tests::Property& property, size_t iterations)
        {
            Spiral::Tests::CampaignOptions options;
            options.Iterations = iterations;
            if (const char* seed = std::getenv("SPIRAL_SCENE_PREREQ_SEED"))
                options.Seed = std::strtoull(seed, nullptr, 10);
            Spiral::Tests::ChoiceTrace replay;
            if (const char* trace = std::getenv("SPIRAL_SCENE_PREREQ_REPLAY");
                trace && Spiral::Tests::ParseTrace(trace, replay))
                options.Replay = replay;

            Spiral::Tests::Counterexample failure;
            if (Spiral::Tests::RunCampaign(options, property, failure))
                return true;

            const std::string minimized = Spiral::Tests::SerializeTrace(failure.MinimizedTrace);
            const std::string rerun = "SPIRAL_SCENE_PREREQ_SEED=" + std::to_string(failure.Seed)
                + " SPIRAL_SCENE_PREREQ_REPLAY=\"" + minimized + "\" EngineTests --test <registered name of "
                + std::string(name) + ">";
            const std::filesystem::path artifact = std::filesystem::temp_directory_path()
                / "spiral-scene-prerequisites-counterexample.json";
            std::string artifactError;
            const bool written = Spiral::Tests::WriteCounterexample(artifact, name, failure, rerun, artifactError);
            std::cerr << "Scene prerequisite property failed [" << name << "]: " << failure.Message
                << " seed=" << failure.Seed << " iteration=" << failure.Iteration
                << " originalTrace=" << Spiral::Tests::SerializeTrace(failure.OriginalTrace)
                << " minimizedTrace=" << minimized << "\n  rerun: " << rerun << '\n';
            if (written)
                std::cerr << "  counterexample: " << artifact.string() << '\n';
            else
                std::cerr << "  counterexample write failed: " << artifactError << '\n';
            return false;
        }

        // ---- generation --------------------------------------------------------------

        double UnitDouble(Spiral::Tests::ChoiceStream& choices)
        {
            return static_cast<double>(choices.Next() >> 11) / 9007199254740992.0;
        }

        float RangeFloat(Spiral::Tests::ChoiceStream& choices, float minimum, float maximum)
        {
            return minimum + static_cast<float>(UnitDouble(choices)) * (maximum - minimum);
        }

        std::string RandomName(Spiral::Tests::ChoiceStream& choices)
        {
            static constexpr std::string_view alphabet = "abcXYZ 019_-\"\\'\xC3\xA9";
            std::string name;
            const size_t length = choices.NextSize(0, 14);
            for (size_t index = 0; index < length; ++index)
                name += alphabet[choices.NextSize(0, alphabet.size() - 1)];
            return name;
        }

        Math::SectorLocalPosition RandomPosition(Spiral::Tests::ChoiceStream& choices, double extent)
        {
            const double half = extent * 0.5;
            const auto local = [&]
            {
                switch (choices.NextSize(0, 5))
                {
                    case 0: return -half;
                    case 1: return 0.0;
                    case 2: return -0.0;
                    case 3: return std::nextafter(half, 0.0);
                    default: return -half + UnitDouble(choices) * extent;
                }
            };
            const std::vector<std::int64_t> sectorBoundaries { -1000000, -1, 0, 1, 1000000 };
            Math::SectorLocalPosition position;
            position.Sector = { choices.NextI64(-1000000, 1000000, sectorBoundaries),
                choices.NextI64(-1000000, 1000000, sectorBoundaries),
                choices.NextI64(-1000000, 1000000, sectorBoundaries) };
            position.Local = { local(), local(), local() };
            return position;
        }

        std::optional<LightComponent> RandomLight(Spiral::Tests::ChoiceStream& choices)
        {
            LightComponent light;
            light.Type = static_cast<LightType>(choices.NextSize(0, 2));
            light.PhotometricUnit = GetLightPhotometricUnit(light.Type);
            light.PhotometricValue = UnitDouble(choices) * GetMaximumLightPhotometricValue(light.Type);
            light.Color = { RangeFloat(choices, 0.0f, 4.0f), RangeFloat(choices, 0.0f, 4.0f), RangeFloat(choices, 0.0f, 4.0f) };
            light.Range = light.Type == LightType::Directional ? 10.0f : RangeFloat(choices, 0.0f, 500.0f);
            light.InnerConeDegrees = RangeFloat(choices, 0.0f, 90.0f);
            light.OuterConeDegrees = light.InnerConeDegrees + RangeFloat(choices, 0.0f, 90.0f);
            light.CastsShadows = choices.NextBool();
            if (!IsValidLightComponent(light))
                return std::nullopt;
            return light;
        }

        // A scene with id gaps (random deletions), mixed components, boundary-heavy transforms and
        // a random main camera. Everything goes through the public mutators.
        Scene BuildScene(Spiral::Tests::ChoiceStream& choices)
        {
            Math::WorldGridPolicy policy;
            policy.SectorExtent = choices.NextBool() ? 4096.0 : 1000.0;
            Scene scene("Generated \"scene\"", policy);

            const size_t count = choices.NextSize(0, 10);
            for (size_t index = 0; index < count; ++index)
            {
                const Entity entity = scene.CreateEntity(RandomName(choices));
                const bool camera = choices.NextSize(0, 3) == 0;
                const Math::Vec3 scale = camera ? Math::Vec3 { 1.0f, 1.0f, 1.0f }
                    : Math::Vec3 { RangeFloat(choices, 0.01f, 8.0f), RangeFloat(choices, 0.01f, 8.0f), RangeFloat(choices, 0.01f, 8.0f) };
                scene.SetEntityTransform(entity, RandomPosition(choices, policy.SectorExtent),
                    { RangeFloat(choices, -720.0f, 720.0f), RangeFloat(choices, -720.0f, 720.0f), RangeFloat(choices, -720.0f, 720.0f) },
                    scale);
                if (camera)
                {
                    CameraComponent component;
                    component.Projection.VerticalFovDegrees = RangeFloat(choices, 20.0f, 120.0f);
                    component.Projection.NearClip = RangeFloat(choices, 0.01f, 1.0f);
                    component.Projection.FarClip = RangeFloat(choices, 10.0f, 5000.0f);
                    component.BackgroundColor = { RangeFloat(choices, 0.0f, 1.0f), RangeFloat(choices, 0.0f, 1.0f), RangeFloat(choices, 0.0f, 1.0f) };
                    component.Primary = choices.NextBool();
                    scene.AddCameraComponent(entity, component);
                }
                if (choices.NextSize(0, 2) == 0)
                    if (const std::optional<LightComponent> light = RandomLight(choices))
                        scene.AddLightComponent(entity, *light);
                if (choices.NextBool())
                {
                    MeshRendererComponent mesh;
                    mesh.MeshAsset = choices.Next();
                    mesh.MaterialAsset = choices.Next();
                    mesh.MeshName = RandomName(choices);
                    mesh.Visible = choices.NextBool();
                    mesh.CastsShadows = choices.NextBool();
                    scene.AddMeshRendererComponent(entity, mesh);
                }
            }

            // Leave id gaps and a next-id counter above every live id.
            const size_t deletions = choices.NextSize(0, 3);
            for (size_t index = 0; index < deletions && !scene.GetEntities().empty(); ++index)
                scene.DestroyEntity(scene.GetEntities()[choices.NextSize(0, scene.GetEntities().size() - 1)].EntityHandle);

            std::vector<Entity> cameras;
            for (const SceneEntity& entity : scene.GetEntities())
                if (entity.Camera)
                    cameras.push_back(entity.EntityHandle);
            if (!cameras.empty() && choices.NextBool())
                scene.SetMainCameraEntity(cameras[choices.NextSize(0, cameras.size() - 1)]);
            return scene;
        }
    }

    bool TestSceneRestoreEntityUndoesDeletesByteExactly()
    {
        const Spiral::Tests::Property property = [](Spiral::Tests::ChoiceStream& choices, std::string& message)
        {
            TempDir dir;
            Scene scene = BuildScene(choices);
            const std::optional<std::string> initial = SaveBytes(scene, dir);
            if (!initial)
            {
                message = "generated scene did not save";
                return false;
            }
            const std::optional<EntityId> initialNext = NextIdFromBytes(*initial);

            struct Deleted
            {
                SceneEntity Snapshot;
                size_t Index = 0;
                bool WasMain = false;
                std::string BytesBefore;
            };
            std::vector<Deleted> deleted;
            const size_t deletions = choices.NextSize(1, 8);
            for (size_t step = 0; step < deletions && !scene.GetEntities().empty(); ++step)
            {
                const size_t index = choices.NextSize(0, scene.GetEntities().size() - 1);
                Deleted record;
                record.Snapshot = scene.GetEntities()[index];
                record.Index = index;
                record.WasMain = record.Snapshot.EntityHandle == scene.GetMainCameraEntity();
                const std::optional<std::string> before = SaveBytes(scene, dir);
                if (!before)
                {
                    message = "scene did not save before deletion";
                    return false;
                }
                record.BytesBefore = *before;
                size_t reportedIndex = 0;
                if (!scene.TryGetEntityIndex(record.Snapshot.EntityHandle, reportedIndex) || reportedIndex != index)
                {
                    message = "TryGetEntityIndex disagrees with GetEntities order";
                    return false;
                }
                if (!scene.DestroyEntity(record.Snapshot.EntityHandle) || scene.IsEntityValid(record.Snapshot.EntityHandle))
                {
                    message = "DestroyEntity failed";
                    return false;
                }
                size_t absentIndex = 123;
                if (scene.TryGetEntityIndex(record.Snapshot.EntityHandle, absentIndex) || absentIndex != 123)
                {
                    message = "TryGetEntityIndex reported a destroyed entity or wrote its output";
                    return false;
                }
                deleted.push_back(std::move(record));
            }

            const std::optional<std::string> afterDeletes = SaveBytes(scene, dir);
            if (!afterDeletes)
            {
                message = "scene did not save after deletions";
                return false;
            }

            for (size_t step = deleted.size(); step-- > 0;)
            {
                const Deleted& record = deleted[step];
                if (!scene.RestoreEntity(record.Snapshot, record.Index))
                {
                    message = "RestoreEntity rejected an entity that DestroyEntity removed";
                    return false;
                }
                if (record.WasMain && !scene.SetMainCameraEntity(record.Snapshot.EntityHandle))
                {
                    message = "SetMainCameraEntity rejected a restored main camera";
                    return false;
                }
                const std::optional<std::string> restored = SaveBytes(scene, dir);
                if (!restored || *restored != record.BytesBefore || !IdsAreUnique(scene))
                {
                    message = "restoring deletion " + std::to_string(step) + " did not reproduce the saved scene byte for byte";
                    return false;
                }
            }

            // The id counter survived: the next fresh entity gets the id the original scene would have given.
            Scene probe = scene;
            if (initialNext && probe.CreateEntity("probe").Id != *initialNext)
            {
                message = "next entity id changed across delete and restore";
                return false;
            }

            // Redo: deleting the same ids again reproduces the post-deletion scene exactly.
            for (const Deleted& record : deleted)
                if (!scene.DestroyEntity(record.Snapshot.EntityHandle))
                {
                    message = "redo deletion failed";
                    return false;
                }
            const std::optional<std::string> redone = SaveBytes(scene, dir);
            if (!redone || *redone != *afterDeletes)
            {
                message = "redoing the deletions did not reproduce the post-deletion scene";
                return false;
            }
            return true;
        };

        return RunProperty("Scene RestoreEntity undoes deletes byte exactly", property, 150);
    }

    bool TestSceneRestoreEntityRejectsInvalidRequestsFailureAtomically()
    {
        const Spiral::Tests::Property property = [](Spiral::Tests::ChoiceStream& choices, std::string& message)
        {
            TempDir dir;
            Scene scene = BuildScene(choices);
            if (scene.GetEntities().empty())
                scene.CreateEntity("seed");
            const std::optional<std::string> initial = SaveBytes(scene, dir);
            if (!initial)
            {
                message = "generated scene did not save";
                return false;
            }
            const EntityId next = NextIdFromBytes(*initial).value_or(0);
            const Math::WorldGridPolicy policy = scene.GetWorldGridPolicy();
            const double half = policy.SectorExtent * 0.5;
            const float nan = std::numeric_limits<float>::quiet_NaN();
            const float inf = std::numeric_limits<float>::infinity();

            SceneEntity candidate = scene.GetEntities()[choices.NextSize(0, scene.GetEntities().size() - 1)];
            candidate.EntityHandle = Entity { next + 100 };
            size_t index = choices.NextSize(0, scene.GetEntities().size());

            {
                Scene control = scene;
                if (!control.RestoreEntity(candidate, index) || !control.IsEntityValid(candidate.EntityHandle))
                {
                    message = "positive control candidate was rejected";
                    return false;
                }
            }

            const size_t mutation = choices.NextSize(0, 15);
            switch (mutation)
            {
                case 0: candidate.EntityHandle = Entity {}; break;
                case 1:
                    candidate.EntityHandle = scene.GetEntities()[choices.NextSize(0, scene.GetEntities().size() - 1)].EntityHandle;
                    break;
                case 2: index = scene.GetEntities().size() + 1; break;
                case 3: index = std::numeric_limits<size_t>::max(); break;
                case 4:
                {
                    // Canonical for a policy with twice the extent, outside this Scene's sector.
                    Math::WorldGridPolicy wider = policy;
                    wider.SectorExtent = policy.SectorExtent * 2.0;
                    Scene donor("donor", wider);
                    const Entity donated = donor.CreateEntity("donor");
                    if (!donor.SetEntitySectorLocalPosition(donated, { { 0, 0, 0 }, { half * 1.5, 0.0, 0.0 } }))
                    {
                        message = "donor position was rejected";
                        return false;
                    }
                    candidate.Transform = donor.TryGetEntity(donated)->Transform;
                    break;
                }
                case 5: candidate.Transform.RotationDegrees.X = nan; break;
                case 6: candidate.Transform.RotationDegrees.Y = inf; break;
                case 7: candidate.Transform.Scale.X = 0.0f; break;
                case 8: candidate.Transform.Scale.Y = -1.0f; break;
                case 9: candidate.Transform.Scale.Z = nan; break;
                case 10: candidate.Transform.Scale.X = inf; break;
                case 11:
                    candidate.Camera = CameraComponent {};
                    candidate.Transform.Scale = { 2.0f, 1.0f, 1.0f };
                    break;
                case 12:
                    candidate.Light = LightComponent {};
                    candidate.Light->PhotometricValue = std::numeric_limits<double>::quiet_NaN();
                    break;
                case 13:
                    candidate.Light = LightComponent {};
                    candidate.Light->PhotometricUnit = LightPhotometricUnit::Lumens;
                    break;
                case 14:
                    candidate.Light = LightComponent {};
                    candidate.Light->Color.Y = -0.5f;
                    break;
                default:
                    candidate.Light = LightComponent {};
                    candidate.Light->Type = LightType::Spot;
                    candidate.Light->PhotometricUnit = LightPhotometricUnit::Lumens;
                    candidate.Light->PhotometricValue = 1000.0;
                    candidate.Light->InnerConeDegrees = 50.0f;
                    candidate.Light->OuterConeDegrees = 40.0f;
                    break;
            }

            const std::vector<EntityId> idsBefore = Ids(scene);
            const Entity mainBefore = scene.GetMainCameraEntity();
            if (scene.RestoreEntity(candidate, index))
            {
                message = "mutation " + std::to_string(mutation) + " was accepted";
                return false;
            }
            const std::optional<std::string> after = SaveBytes(scene, dir);
            if (!after || *after != *initial || Ids(scene) != idsBefore || scene.GetMainCameraEntity() != mainBefore)
            {
                message = "rejected mutation " + std::to_string(mutation) + " changed the scene";
                return false;
            }
            if (mutation != 1 && mutation != 0 && scene.IsEntityValid(candidate.EntityHandle))
            {
                message = "rejected entity is observable";
                return false;
            }
            if (!scene.RestoreEntity(SceneEntity { Entity { next + 7 }, "after rejection", {}, {}, {}, {} }))
            {
                message = "scene refused a valid restore after a rejected one";
                return false;
            }
            return true;
        };

        return RunProperty("Scene RestoreEntity rejects invalid requests failure-atomically", property, 200);
    }

    bool TestSceneCloneEntityCopiesComponentsUnderFreshIdsAndUndoesExactly()
    {
        const Spiral::Tests::Property property = [](Spiral::Tests::ChoiceStream& choices, std::string& message)
        {
            TempDir dir;
            Scene scene = BuildScene(choices);
            if (scene.GetEntities().empty())
                scene.CreateEntity("seed");
            const std::optional<std::string> initial = SaveBytes(scene, dir);
            if (!initial)
            {
                message = "generated scene did not save";
                return false;
            }
            const EntityId next = NextIdFromBytes(*initial).value_or(0);
            const Entity main = scene.GetMainCameraEntity();
            const SceneEntity source = scene.GetEntities()[choices.NextSize(0, scene.GetEntities().size() - 1)];
            const std::string name = RandomName(choices);

            if (scene.CloneEntity(Entity {}, name).IsValid() || scene.CloneEntity(Entity { next + 50 }, name).IsValid())
            {
                message = "cloning an unknown entity succeeded";
                return false;
            }
            const std::optional<std::string> afterFailure = SaveBytes(scene, dir);
            if (!afterFailure || *afterFailure != *initial)
            {
                message = "failed clone changed the scene";
                return false;
            }

            const Entity clone = scene.CloneEntity(source.EntityHandle, name);
            if (clone.Id != next)
            {
                message = "clone did not receive the previous next-id counter value";
                return false;
            }
            SceneEntity expected = source;
            expected.EntityHandle = clone;
            expected.Name = name;
            size_t cloneIndex = 0;
            const SceneEntity* stored = scene.TryGetEntity(clone);
            if (!stored || !SameEntity(expected, *stored) || !scene.TryGetEntityIndex(clone, cloneIndex)
                || cloneIndex + 1 != scene.GetEntities().size())
            {
                message = "clone differs from the source in components, or is not appended";
                return false;
            }
            const SceneEntity* original = scene.TryGetEntity(source.EntityHandle);
            if (!original || !SameEntity(source, *original) || scene.GetMainCameraEntity() != main || !IdsAreUnique(scene))
            {
                message = "cloning disturbed the source, the main camera or id uniqueness";
                return false;
            }

            const std::optional<std::string> cloned = SaveBytes(scene, dir);
            Scene loaded;
            if (!cloned || !Scene::LoadFromFile(dir.File("scene.spiralscene"), loaded) || !SameEntities(scene, loaded))
            {
                message = "cloned scene did not survive a save/load round trip";
                return false;
            }
            const std::optional<std::string> resaved = SaveBytes(loaded, dir);
            if (!resaved || *resaved != *cloned || NextIdFromBytes(*cloned) != std::optional<EntityId>(next + 1))
            {
                message = "reloaded cloned scene does not reserialize identically";
                return false;
            }

            // Undo of a duplicate is a destroy; ids are never reused, so only the counter differs.
            SceneEntity snapshot = *scene.TryGetEntity(clone);
            if (!scene.DestroyEntity(clone))
            {
                message = "destroying the clone failed";
                return false;
            }
            const std::optional<std::string> undone = SaveBytes(scene, dir);
            if (!undone || *undone != WithNextId(*initial, next + 1))
            {
                message = "undoing the duplicate did not reproduce the original scene";
                return false;
            }
            if (!scene.RestoreEntity(snapshot, scene.GetEntities().size()))
            {
                message = "redoing the duplicate was rejected";
                return false;
            }
            const std::optional<std::string> redone = SaveBytes(scene, dir);
            if (!redone || *redone != *cloned)
            {
                message = "redoing the duplicate did not reproduce the cloned scene";
                return false;
            }
            return true;
        };

        return RunProperty("Scene CloneEntity copies components under fresh ids and undoes exactly", property, 150);
    }

    bool TestSceneRestoreEntityIdCounterOrderingCameraAndPasteRules()
    {
        Checker check;
        TempDir dir;

        const auto plain = [](EntityId id, std::string name)
        {
            SceneEntity entity;
            entity.EntityHandle = Entity { id };
            entity.Name = std::move(name);
            return entity;
        };

        // Id counter: raised above restored ids, never lowered, saturating at the id limit.
        {
            Scene scene;
            check(scene.RestoreEntity(plain(5000, "far")), "restore with a high id");
            check(scene.CreateEntity("next").Id == 5001, "counter moved above the restored id");
            check(scene.RestoreEntity(plain(3, "low")), "restore with an id below the counter");
            check(scene.CreateEntity("next").Id == 5002, "counter was not lowered by a low id");

            constexpr EntityId kMaxId = std::numeric_limits<EntityId>::max();
            check(scene.RestoreEntity(plain(kMaxId, "limit")), "restore with the largest id");
            std::set<EntityId> created;
            for (int index = 0; index < 4; ++index)
                created.insert(scene.CreateEntity("after limit").Id);
            check(created.size() == 4 && !created.contains(kMaxId) && IdsAreUnique(scene),
                "creating after the largest id keeps ids unique");
            check(SaveBytes(scene, dir).has_value(), "scene holding the largest id saves");
            Scene loaded;
            check(Scene::LoadFromFile(dir.File("scene.spiralscene"), loaded) && SameEntities(scene, loaded) && IdsAreUnique(loaded),
                "scene holding the largest id round trips");

            Scene exhausted;
            const Math::SectorLocalPosition origin;
            check(exhausted.RestoreEntity(plain(kMaxId - 1, "last"))
                    && !exhausted.CloneEntity(Entity { kMaxId - 1 }, "no id left").IsValid(),
                "clone refuses when no id is left");
            check(exhausted.GetEntities().size() == 2, "refused clone left the scene unchanged");
            (void)origin;
        }

        // Ordering: the index selects the position, which is also the saved order.
        {
            Scene scene;
            const Entity two = scene.CreateEntity("two");
            const Entity three = scene.CreateEntity("three");
            const Entity four = scene.CreateEntity("four");
            (void)two;
            (void)four;
            const SceneEntity threeSnapshot = *scene.TryGetEntity(three);
            size_t threeIndex = 0;
            check(scene.TryGetEntityIndex(three, threeIndex) && threeIndex == 2, "index of the third entity");
            check(scene.DestroyEntity(three) && (Ids(scene) == std::vector<EntityId> { 1, 2, 4 }), "destroyed the middle entity");
            check(scene.RestoreEntity(threeSnapshot, threeIndex) && (Ids(scene) == std::vector<EntityId> { 1, 2, 3, 4 }),
                "restored at its original index");
            check(scene.RestoreEntity(plain(9, "front"), 0) && (Ids(scene) == std::vector<EntityId> { 9, 1, 2, 3, 4 }),
                "restore at index zero");
            check(scene.RestoreEntity(plain(8, "appended")) && (Ids(scene) == std::vector<EntityId> { 9, 1, 2, 3, 4, 8 }),
                "overload without an index appends");
            check(scene.RestoreEntity(plain(7, "end"), scene.GetEntities().size()) && Ids(scene).back() == 7, "restore at the end index");
            check(!scene.RestoreEntity(plain(6, "past"), scene.GetEntities().size() + 1), "index past the end is rejected");
            check(SaveBytes(scene, dir).has_value(), "reordered scene saves");
            Scene loaded;
            check(Scene::LoadFromFile(dir.File("scene.spiralscene"), loaded) && Ids(loaded) == Ids(scene) && SameEntities(scene, loaded),
                "saved order is the vector order");
        }

        // Main camera: restore and clone never change the designation; the caller reapplies it.
        {
            Scene scene;
            const Entity original = scene.GetMainCameraEntity();
            CameraComponent alternate;
            alternate.Projection.VerticalFovDegrees = 33.0f;
            alternate.Primary = true;
            const Entity other = scene.CreateEntity("other camera");
            scene.AddCameraComponent(other, alternate);
            check(scene.GetMainCameraEntity() == original, "an added camera does not displace an existing main camera");

            const SceneEntity mainSnapshot = *scene.TryGetEntity(original);
            const CameraComponent mainCamera = scene.GetMainCamera();
            size_t mainIndex = 0;
            check(scene.TryGetEntityIndex(original, mainIndex) && scene.DestroyEntity(original), "destroyed the main camera");
            check(scene.GetMainCameraEntity() == other && scene.GetMainCamera().Projection.VerticalFovDegrees == 33.0f,
                "DestroyEntity promoted the other camera");
            check(scene.RestoreEntity(mainSnapshot, mainIndex), "restored the old main camera");
            check(scene.GetMainCameraEntity() == other, "restore did not displace the promoted camera");
            check(scene.SetMainCameraEntity(original)
                    && scene.GetMainCameraEntity() == original
                    && scene.GetMainCamera().Projection.VerticalFovDegrees == mainCamera.Projection.VerticalFovDegrees
                    && scene.GetMainCamera().Primary == mainCamera.Primary,
                "reapplying the main camera restores its cached camera values");

            const Entity clone = scene.CloneEntity(original, "main copy");
            check(clone.IsValid() && scene.GetMainCameraEntity() == original, "cloning the main camera keeps the original as main");
            const CameraComponent* cloneCamera = scene.TryGetCameraComponent(clone);
            check(cloneCamera && cloneCamera->Primary == mainCamera.Primary
                    && scene.TryGetTransform(clone)->Scale.X == 1.0f
                    && scene.TryGetTransform(clone)->Scale.Y == 1.0f
                    && scene.TryGetTransform(clone)->Scale.Z == 1.0f,
                "cloned camera keeps its component and unit scale");

            // A Scene without a main camera stays without one.
            Scene headless;
            const Entity formerMain = headless.GetMainCameraEntity();
            const SceneEntity formerSnapshot = *headless.TryGetEntity(formerMain);
            headless.RemoveCameraComponent(formerMain);
            check(!headless.GetMainCameraEntity().IsValid(), "main camera removed");
            SceneEntity extra = plain(40, "camera");
            extra.Camera = CameraComponent {};
            check(headless.RestoreEntity(extra) && !headless.GetMainCameraEntity().IsValid(),
                "restoring a primary camera does not elect a main camera");
            check(headless.CloneEntity(Entity { 40 }, "copy").IsValid() && !headless.GetMainCameraEntity().IsValid(),
                "cloning a primary camera does not elect a main camera");
            (void)formerSnapshot;
        }

        // Paste between scenes: id collisions and policy mismatches are rejected, valid pastes round trip.
        {
            Scene source;
            const Entity lamp = source.CreateEntity("lamp");
            LightComponent light;
            light.Type = LightType::Point;
            light.PhotometricUnit = LightPhotometricUnit::Lumens;
            light.PhotometricValue = 800.0;
            check(source.AddLightComponent(lamp, light) != nullptr, "light added");
            check(source.SetEntitySectorLocalPosition(lamp, { { 3, -2, 1 }, { 1500.0, -2000.0, 0.25 } }), "lamp positioned");
            SceneEntity copied = *source.TryGetEntity(lamp);

            Scene target;
            target.CreateEntity("occupant");
            check(copied.EntityHandle.Id == 2 && target.IsEntityValid(copied.EntityHandle), "fixture ids collide");
            const std::optional<std::string> before = SaveBytes(target, dir);
            check(!target.RestoreEntity(copied) && SaveBytes(target, dir) == before, "paste with a colliding id is rejected atomically");

            copied.EntityHandle = Entity { 77 };
            check(target.RestoreEntity(copied), "paste with a fresh id");
            check(SaveBytes(target, dir).has_value(), "pasted scene saves");
            Scene loaded;
            check(Scene::LoadFromFile(dir.File("scene.spiralscene"), loaded) && SameEntities(target, loaded),
                "pasted scene round trips");

            Math::WorldGridPolicy narrow;
            narrow.SectorExtent = 1000.0;
            Scene narrowScene("narrow", narrow);
            const std::optional<std::string> narrowBefore = SaveBytes(narrowScene, dir);
            check(!narrowScene.RestoreEntity(copied) && SaveBytes(narrowScene, dir) == narrowBefore,
                "paste whose local position is outside the target sector extent is rejected atomically");
        }

        // A source invalidated through a raw component pointer cannot be cloned; nothing changes.
        {
            Scene scene;
            const Entity lamp = scene.CreateEntity("lamp");
            scene.AddLightComponent(lamp);
            scene.TryGetLightComponent(lamp)->PhotometricValue = std::numeric_limits<double>::quiet_NaN();
            const Scene snapshot = scene;
            check(!scene.CloneEntity(lamp, "bad copy").IsValid(), "clone of an invalid source is rejected");
            check(SameEntities(scene, snapshot) && scene.CreateEntity("probe").Id == lamp.Id + 1,
                "failed clone changed no entity and consumed no id");
        }

        // Non-ASCII, quote and backslash names survive clone and reload.
        {
            Scene scene;
            const Entity clone = scene.CloneEntity(scene.GetMainCameraEntity(), "q\"uote \\ \xC3\xA9");
            check(clone.IsValid(), "clone with a punctuated name");
            check(SaveBytes(scene, dir).has_value(), "punctuated scene saves");
            Scene loaded;
            check(Scene::LoadFromFile(dir.File("scene.spiralscene"), loaded) && SameEntities(scene, loaded),
                "punctuated names round trip");
        }

        return check.Passed;
    }
}
