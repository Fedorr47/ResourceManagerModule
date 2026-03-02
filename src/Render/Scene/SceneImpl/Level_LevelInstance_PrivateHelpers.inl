		rhi::TextureHandle TryGetTextureHandle_(ResourceManager& rm, std::string_view id) const noexcept
		{
			if (auto texRes = rm.Get<TextureResource>(id))
			{
				const auto& gpu = texRes->GetResource();
				if (gpu.id != 0)
				{
					return rhi::TextureHandle{ static_cast<std::uint32_t>(gpu.id) };
				}
			}
			return {};
		}

		rhi::TextureDescIndex GetOrCreateTextureDesc_(
			ResourceManager& rm,
			BindlessTable& bindless,
			std::string_view textureId)
		{
			const std::string key{ textureId };

			if (auto it = textureDesc_.find(key); it != textureDesc_.end())
			{
				return it->second;
			}

			const rhi::TextureHandle handle = TryGetTextureHandle_(rm, textureId);
			if (!handle)
			{
				return 0;
			}

			const rhi::TextureDescIndex index = bindless.RegisterTexture(handle);
			textureDesc_.emplace(key, index);
			return index;
		}

		MaterialHandle GetMaterialHandle_(std::string_view materialId) const noexcept
		{
			if (materialId.empty())
			{	
				return {};
			}

			auto it = materialHandles_.find(std::string(materialId));
			if (it == materialHandles_.end())
			{
				return {};
			}
			return it->second;
		}

		MeshHandle GetOrLoadMeshHandle_(const LevelAsset& asset, AssetManager& assets, const std::string& meshId) const
		{
			auto it = asset.meshes.find(meshId);
			if (it == asset.meshes.end())
			{
				throw std::runtime_error("Level: node references unknown meshId: " + meshId);
			}

			MeshProperties p{};
			p.filePath = it->second.path;
			p.debugName = it->second.debugName;
			return assets.LoadMeshAsync(meshId, std::move(p));
		}

		void EnsureDrawForNode_(const LevelAsset& asset, Scene& scene, AssetManager& assets, int nodeIndex)
		{
			if (nodeIndex < 0) 
			{
				return;
			}

			const std::size_t i = static_cast<std::size_t>(nodeIndex);
			if (i >= asset.nodes.size())
			{
				return;
			}

			const LevelNode& node = asset.nodes[i];
			if (!node.alive || !node.visible || node.mesh.empty())
			{
				DestroyDrawForNode_(scene, nodeIndex);
				return;
			}

			if (nodeToDraw_.size() < asset.nodes.size())
			{	
				nodeToDraw_.resize(asset.nodes.size(), -1);
			}
			if (world_.size() < asset.nodes.size())
			{	
				world_.resize(asset.nodes.size(), mathUtils::Mat4(1.0f));
			}

			const int existing = nodeToDraw_[i];
			if (existing >= 0 && static_cast<std::size_t>(existing) < scene.drawItems.size())
			{
				DrawItem& item = scene.drawItems[static_cast<std::size_t>(existing)];
				item.mesh = GetOrLoadMeshHandle_(asset, assets, node.mesh);
				item.material = GetMaterialHandle_(node.material);
				return;
			}

			// Spawn new draw item
			DrawItem item{};
			item.mesh = GetOrLoadMeshHandle_(asset, assets, node.mesh);
			item.material = GetMaterialHandle_(node.material);
			item.transform.useMatrix = true;
			item.transform.matrix = world_[i]; // will be updated on next SyncTransformsIfDirty()

			const int drawIndex = static_cast<int>(scene.drawItems.size());
			scene.AddDraw(item);

			// Maintain mapping vectors aligned with scene.drawItems.
			if (drawToNode_.size() < scene.drawItems.size())
			{	
				drawToNode_.resize(scene.drawItems.size(), -1);
			}
			drawToNode_[static_cast<std::size_t>(drawIndex)] = nodeIndex;
			nodeToDraw_[i] = drawIndex;
		}

		void DestroyDrawForNode_(Scene& scene, int nodeIndex)
		{
			if (nodeIndex < 0) 
			{
				return;
			}

			const std::size_t nodeIdx = static_cast<std::size_t>(nodeIndex);
			if (nodeIdx >= nodeToDraw_.size())
			{
				return;
			}

			const int di = nodeToDraw_[nodeIdx];
			if (di < 0)
			{
				return;
			}

			const std::size_t drawIndex = static_cast<std::size_t>(di);
			if (drawIndex >= scene.drawItems.size())
			{
				nodeToDraw_[nodeIdx] = -1;
				return;
			}

			const std::size_t last = scene.drawItems.size() - 1;
			if (drawIndex != last)
			{
				std::swap(scene.drawItems[drawIndex], scene.drawItems[last]);

				if (last < drawToNode_.size())
				{
					const int movedNode = drawToNode_[last];
					if (drawIndex < drawToNode_.size())
						drawToNode_[drawIndex] = movedNode;
					if (movedNode >= 0 && static_cast<std::size_t>(movedNode) < nodeToDraw_.size())
						nodeToDraw_[static_cast<std::size_t>(movedNode)] = static_cast<int>(drawIndex);
				}
			}

			scene.drawItems.pop_back();
			if (!drawToNode_.empty())
			{	
				drawToNode_.pop_back();
			}

			nodeToDraw_[nodeIdx] = -1;
		}

		std::vector<int> CollectSubtree_(const LevelAsset& asset, int rootNodeIndex) const
		{
			std::vector<int> out;
			if (rootNodeIndex < 0)
				return out;

			const std::size_t nodeCount = asset.nodes.size();
			if (static_cast<std::size_t>(rootNodeIndex) >= nodeCount)
			{	
				return out;
			}

			// children adjacency (alive only)
			std::vector<std::vector<int>> children;
			children.resize(nodeCount);

			for (std::size_t i = 0; i < nodeCount; ++i)
			{
				const LevelNode& node = asset.nodes[i];
				if (!node.alive)
					continue;
				if (node.parent < 0)
					continue;

				const std::size_t p = static_cast<std::size_t>(node.parent);
				if (p >= nodeCount)
					continue;
				if (!asset.nodes[p].alive)
					continue;

				children[p].push_back(static_cast<int>(i));
			}

			std::vector<int> stack;
			stack.push_back(rootNodeIndex);

			while (!stack.empty())
			{
				const int cur = stack.back();
				stack.pop_back();

				if (cur < 0 || static_cast<std::size_t>(cur) >= nodeCount)
					continue;
				if (!asset.nodes[static_cast<std::size_t>(cur)].alive)
					continue;

				out.push_back(cur);

				for (int ch : children[static_cast<std::size_t>(cur)])
				{
					stack.push_back(ch);
				}
			}

			return out;
		}

		void RecomputeWorld_(const LevelAsset& asset)
		{
			const std::size_t n = asset.nodes.size();
			world_.resize(n, mathUtils::Mat4(1.0f));

			std::vector<std::uint8_t> state;
			state.resize(n, 0); // 0=unvisited, 1=visiting, 2=done

			auto compute = [&](auto&& self, std::size_t i) -> const mathUtils::Mat4&
			{
				if (state[i] == 2)
					return world_[i];

				if (state[i] == 1)
				{
					// cycle - treat as root
					world_[i] = root_ * asset.nodes[i].transform.ToMatrix();
					state[i] = 2;
					return world_[i];
				}

				state[i] = 1;

				const LevelNode& node = asset.nodes[i];
				if (!node.alive)
				{
					world_[i] = mathUtils::Mat4(1.0f);
					state[i] = 2;
					return world_[i];
				}

				mathUtils::Mat4 parentWorld = root_;
				if (node.parent >= 0)
				{
					const std::size_t p = static_cast<std::size_t>(node.parent);
					if (p < n && asset.nodes[p].alive)
					{
						parentWorld = self(self, p);
					}
				}

				world_[i] = parentWorld * node.transform.ToMatrix();
				state[i] = 2;
				return world_[i];
			};

			for (std::size_t i = 0; i < n; ++i)
			{
				compute(compute, i);
			}
		}
