	template <typename TGizmoState>
	static void HideGizmoState(TGizmoState& gizmo) noexcept
	{
		gizmo.visible = false;
		gizmo.hoveredAxis = rendern::GizmoAxis::None;
		gizmo.activeAxis = rendern::GizmoAxis::None;
	}

	static bool TryGetPrimaryAliveNode(const rendern::LevelAsset& asset,
		const rendern::LevelInstance& levelInst,
		const rendern::Scene& scene,
		int& outPrimaryNode) noexcept
	{
		const int primaryNode = scene.editorSelectedNode;
		if (!levelInst.IsNodeAlive(asset, primaryNode))
		{
			return false;
		}

		outPrimaryNode = primaryNode;
		return true;
	}

	static bool TryComputeSelectionPivotWorld(const rendern::LevelAsset& asset,
		const rendern::LevelInstance& levelInst,
		const rendern::Scene& scene,
		mathUtils::Vec3& outPivotWorld,
		int& outPrimaryNode) noexcept
	{
		if (!TryGetPrimaryAliveNode(asset, levelInst, scene, outPrimaryNode))
		{
			return false;
		}

		mathUtils::Vec3 sum{ 0.0f, 0.0f, 0.0f };
		int count = 0;
		for (const int nodeIndex : scene.editorSelectedNodes)
		{
			if (!levelInst.IsNodeAlive(asset, nodeIndex))
			{
				continue;
			}
			sum = sum + levelInst.GetNodeWorldPosition(nodeIndex);
			++count;
		}

		if (count == 0)
		{
			outPivotWorld = levelInst.GetNodeWorldPosition(outPrimaryNode);
			return true;
		}

		outPivotWorld = sum * (1.0f / static_cast<float>(count));
		return true;
	}

	static bool CollectAliveSelectedNodes(const rendern::LevelAsset& asset,
		const rendern::LevelInstance& levelInst,
		rendern::Scene& scene,
		std::vector<int>& outNodes)
	{
		outNodes.clear();

		int primaryNode = -1;
		if (!TryGetPrimaryAliveNode(asset, levelInst, scene, primaryNode))
		{
			return false;
		}

		if (scene.editorSelectedNodes.empty())
		{
			scene.editorSelectedNodes.push_back(primaryNode);
		}

		outNodes.reserve(scene.editorSelectedNodes.size());
		for (const int nodeIndex : scene.editorSelectedNodes)
		{
			if (!levelInst.IsNodeAlive(asset, nodeIndex))
			{
				continue;
			}
			outNodes.push_back(nodeIndex);
		}

		if (outNodes.empty())
		{
			outNodes.push_back(primaryNode);
		}

		return true;
	}
