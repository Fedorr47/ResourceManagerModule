module;

#include <vector>

export module core:gameplay_bootstrap;

import :gameplay;
import :level;
import :scene;

export namespace rendern
{
    [[nodiscard]] inline int FindGameplayBootstrapNodeIndex(const LevelAsset& levelAsset, const LevelInstance& levelInstance) noexcept
    {
        for (std::size_t i = 0; i < levelAsset.nodes.size(); ++i)
        {
            const LevelNode& node = levelAsset.nodes[i];
            if (!node.alive || !node.visible)
            {
                continue;
            }
            if (levelInstance.GetNodeSkinnedDrawIndex(static_cast<int>(i)) >= 0)
            {
                return static_cast<int>(i);
            }
        }

        for (std::size_t i = 0; i < levelAsset.nodes.size(); ++i)
        {
            const LevelNode& node = levelAsset.nodes[i];
            if (node.alive && node.visible)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    [[nodiscard]] inline EntityHandle SpawnGameplayNodeBoundEntity(
        GameplayWorld& world,
        std::vector<EntityHandle>& trackedEntities,
        LevelAsset& levelAsset,
        LevelInstance& levelInstance,
        const int nodeIndex,
        const bool playerControlled)
    {
        if (nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= levelAsset.nodes.size())
        {
            return kNullEntity;
        }

        const LevelNode& node = levelAsset.nodes[static_cast<std::size_t>(nodeIndex)];
        if (!node.alive)
        {
            return kNullEntity;
        }

        const EntityHandle entity = world.CreateEntity();
        world.AddTransform(entity,
            GameplayTransformComponent{
                .position = node.transform.position,
                .rotationDegrees = node.transform.rotationDegrees,
                .scale = node.transform.scale
            });

        world.AddNodeLink(entity,
            GameplayNodeLinkComponent{
                .nodeIndex = nodeIndex,
                .levelEntity = levelInstance.GetNodeEntity(nodeIndex)
            });

        world.AddInputIntent(entity);
        world.AddCharacterCommand(entity);
        world.AddCharacterMotor(entity);
        world.AddCharacterMovementState(entity,
            GameplayCharacterMovementStateComponent{
                .grounded = true,
                .jumping = false,
                .falling = false,
                .facingYawDegrees = node.transform.rotationDegrees.y,
                .desiredFacingYawDegrees = node.transform.rotationDegrees.y,
                .previousFacingYawDegrees = node.transform.rotationDegrees.y
            });
        world.AddLocomotion(entity);
        world.AddAction(entity);
        world.AddAnimationNotifyState(entity);

        const int skinnedDrawIndex = levelInstance.GetNodeSkinnedDrawIndex(nodeIndex);
        if (skinnedDrawIndex >= 0)
        {
            world.AddAnimationLink(entity,
                GameplayAnimationLinkComponent{
                    .skinnedDrawIndex = skinnedDrawIndex,
                    .controllerAssetId = node.animationController
                });
        }

        if (playerControlled)
        {
            world.AddPlayerControlled(entity);
            world.AddFollowCamera(entity);
        }

        trackedEntities.push_back(entity);
        return entity;
    }
}
