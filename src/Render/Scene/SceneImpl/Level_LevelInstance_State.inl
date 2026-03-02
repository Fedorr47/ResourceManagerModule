		// Descriptor runtime
		std::unordered_map<std::string, rhi::TextureDescIndex> textureDesc_;
		std::vector<PendingMaterialBinding> pendingBindings_;
		std::optional<std::string> skyboxTextureId_;

		// Editor/runtime state
		mathUtils::Mat4 root_{ 1.0f };
		std::vector<mathUtils::Mat4> world_;
		std::vector<int> nodeToDraw_;
		std::vector<int> drawToNode_;
		std::unordered_map<std::string, MaterialHandle> materialHandles_;
		bool transformsDirty_{ true };
