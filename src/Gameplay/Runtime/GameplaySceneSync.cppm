module;

#include <vector>

export module core:gameplay_scene_sync;

import :gameplay;
import :gameplay_runtime_common;
import :math_utils;
import :level;
import :scene;

export namespace rendern
{
    inline void SyncGameplayTransformsToRuntime(
        GameplayWorld& world,
        const std::vector<EntityHandle>& entities,
        const GameplayUpdateContext& ctx)
    {
        if (ctx.mode != GameplayRuntimeMode::Game ||
            ctx.levelAsset == nullptr || ctx.levelInstance == nullptr || ctx.scene == nullptr)
        {
            return;
        }

        bool anyTransformChanged = false;
        for (const EntityHandle entity : entities)
        {
            const GameplayTransformComponent* transform = world.TryGetTransform(entity);
            const GameplayNodeLinkComponent* nodeLink = world.TryGetNodeLink(entity);
            if (transform == nullptr || nodeLink == nullptr)
            {
                continue;
            }
            if (nodeLink->nodeIndex < 0 || static_cast<std::size_t>(nodeLink->nodeIndex) >= ctx.levelAsset->nodes.size())
            {
                continue;
            }

            LevelNode& node = ctx.levelAsset->nodes[static_cast<std::size_t>(nodeLink->nodeIndex)];
            if (!node.alive)
            {
                continue;
            }

            const Transform desiredTransform{
                .position = transform->position,
                .rotationDegrees = transform->rotationDegrees,
                .scale = transform->scale
            };

            if (mathUtils::NearlyEqualVec3_(node.transform.position, desiredTransform.position) &&
                mathUtils::NearlyEqualVec3_(node.transform.rotationDegrees, desiredTransform.rotationDegrees) &&
                mathUtils::NearlyEqualVec3_(node.transform.scale, desiredTransform.scale))
            {
                continue;
            }

            node.transform.position = desiredTransform.position;
            node.transform.rotationDegrees = desiredTransform.rotationDegrees;
            node.transform.scale = desiredTransform.scale;
            anyTransformChanged = true;
        }

        if (anyTransformChanged)
        {
            ctx.levelInstance->MarkTransformsDirty();
            ctx.levelInstance->SyncTransformsIfDirty(*ctx.levelAsset, *ctx.scene);
        }
    }
}
