		// -----------------------------
		// Editor/runtime mutation API
		// (keeps LevelAsset indices stable via tombstones)
		// -----------------------------
		void SetRootTransform(const mathUtils::Mat4& root)
		{
			root_ = root;
			transformsDirty_ = true;
		}

		bool IsValidNodeIndex(const LevelAsset& asset, int nodeIndex) const noexcept
		{
			return nodeIndex >= 0 && static_cast<std::size_t>(nodeIndex) < asset.nodes.size();
		}

		bool IsNodeAlive(const LevelAsset& asset, int nodeIndex) const noexcept
		{
			if (!IsValidNodeIndex(asset, nodeIndex)) 
			{
				return false;
			}
			return asset.nodes[static_cast<std::size_t>(nodeIndex)].alive;
		}

		int GetNodeDrawIndex(int nodeIndex) const noexcept
		{
			if (nodeIndex < 0) 
			{
				return -1;
			}
			const std::size_t i = static_cast<std::size_t>(nodeIndex);
			if (i >= nodeToDraw_.size()) 
			{
				return -1;
			}
			return nodeToDraw_[i];
		}

		int GetNodeIndexFromDrawIndex(int drawIndex) const noexcept
		{
			if (drawIndex < 0)
			{
				return -1;
			}
			const std::size_t i = static_cast<std::size_t>(drawIndex);
			if (i >= drawToNode_.size())
			{
				return -1;
			}
			return drawToNode_[i];
		}

		const mathUtils::Mat4& GetNodeWorldMatrix(int nodeIndex) const noexcept
		{
			static const mathUtils::Mat4 identity{ 1.0f };
			if (nodeIndex < 0)
			{
				return identity;
			}
			const std::size_t i = static_cast<std::size_t>(nodeIndex);
			if (i >= world_.size())
			{
				return identity;
			}
			return world_[i];
		}

		mathUtils::Mat4 GetParentWorldMatrix(const LevelAsset& asset, int nodeIndex) const noexcept
		{
			if (!IsValidNodeIndex(asset, nodeIndex))
			{
				return root_;
			}

			const LevelNode& node = asset.nodes[static_cast<std::size_t>(nodeIndex)];
			if (node.parent < 0)
			{
				return root_;
			}

			const std::size_t parentIndex = static_cast<std::size_t>(node.parent);
			if (parentIndex >= world_.size())
			{
				return root_;
			}

			return world_[parentIndex];
		}

		mathUtils::Vec3 GetNodeWorldPosition(int nodeIndex) const noexcept
		{
			return GetNodeWorldMatrix(nodeIndex)[3].xyz();
		}

		// Create a new node and (optionally) spawn a DrawItem.
		// Returns the new node index.
		int AddNode(LevelAsset& asset,
			Scene& scene,
			AssetManager& assets,
			std::string_view meshId,
			std::string_view materialId,
			int parentNodeIndex,
			const Transform& localTransform,
			std::string_view name = {})
		{
			LevelNode node;
			node.name = std::string(name);
			node.parent = parentNodeIndex;
			node.visible = true;
			node.alive = true;
			node.transform = localTransform;
			node.mesh = std::string(meshId);
			node.material = std::string(materialId);

			asset.nodes.push_back(std::move(node));

			if (nodeToDraw_.size() < asset.nodes.size())
			{ 
				nodeToDraw_.resize(asset.nodes.size(), -1);
			}
			if (world_.size() < asset.nodes.size())
			{	
				world_.resize(asset.nodes.size(), mathUtils::Mat4(1.0f));
			}

			const int newIndex = static_cast<int>(asset.nodes.size() - 1);

			EnsureDrawForNode_(asset, scene, assets, newIndex);
			transformsDirty_ = true;
			return newIndex;
		}

		// Delete selected node and all its children. (tombstone - keeps indices stable)
		void DeleteSubtree(LevelAsset& asset, Scene& scene, int rootNodeIndex)
		{
			if (!IsNodeAlive(asset, rootNodeIndex))
			{
				return;
			}

			const std::vector<int> toDelete = CollectSubtree_(asset, rootNodeIndex);
			for (int idx : toDelete)
			{
				if (!IsNodeAlive(asset, idx))
				{
					continue;
				}
				LevelNode& n = asset.nodes[static_cast<std::size_t>(idx)];
				n.alive = false;
				n.visible = false;
				DestroyDrawForNode_(scene, idx);
			}

			transformsDirty_ = true;
		}

		void SetNodeVisible(LevelAsset& asset, Scene& scene, AssetManager& assets, int nodeIndex, bool visible)
		{
			if (!IsNodeAlive(asset, nodeIndex))
				return;

			LevelNode& n = asset.nodes[static_cast<std::size_t>(nodeIndex)];
			n.visible = visible;

			EnsureDrawForNode_(asset, scene, assets, nodeIndex);
		}

		void SetNodeMesh(LevelAsset& asset, Scene& scene, AssetManager& assets, int nodeIndex, std::string_view meshId)
		{
			if (!IsNodeAlive(asset, nodeIndex))
			{
				return;
			}

			LevelNode& n = asset.nodes[static_cast<std::size_t>(nodeIndex)];
			n.mesh = std::string(meshId);

			EnsureDrawForNode_(asset, scene, assets, nodeIndex);
		}

		void SetNodeMaterial(LevelAsset& asset, Scene& scene, int nodeIndex, std::string_view materialId)
		{
			if (!IsNodeAlive(asset, nodeIndex))
				return;

			LevelNode& n = asset.nodes[static_cast<std::size_t>(nodeIndex)];
			n.material = std::string(materialId);

			const int di = GetNodeDrawIndex(nodeIndex);
			if (di >= 0 && static_cast<std::size_t>(di) < scene.drawItems.size())
			{
				scene.drawItems[static_cast<std::size_t>(di)].material = GetMaterialHandle_(materialId);
			}
		}

		void MarkTransformsDirty() noexcept
		{
			transformsDirty_ = true;
		}

		// Recompute world transforms (with hierarchy) and push to Scene draw items.
		void SyncTransformsIfDirty(const LevelAsset& asset, Scene& scene)
		{
			if (!transformsDirty_)
				return;

			RecomputeWorld_(asset);

			// Push to Scene
			const std::size_t ncount = asset.nodes.size();
			if (nodeToDraw_.size() < ncount)
				nodeToDraw_.resize(ncount, -1);

			for (std::size_t i = 0; i < ncount; ++i)
			{
				const LevelNode& n = asset.nodes[i];
				if (!n.alive)
				{
					continue;
				}

				const int di = nodeToDraw_[i];
				if (di < 0 || static_cast<std::size_t>(di) >= scene.drawItems.size())
				{
					continue;
				}

				DrawItem& item = scene.drawItems[static_cast<std::size_t>(di)];
				item.transform.useMatrix = true;
				item.transform.matrix = world_[i];
			}

			transformsDirty_ = false;
		}
