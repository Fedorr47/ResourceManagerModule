// Descriptor runtime
std::unordered_map<std::string, rhi::TextureDescIndex> textureDesc_;
std::vector<PendingMaterialBinding> pendingBindings_;
std::optional<std::string> skyboxTextureId_;

// Editor/runtime state
mathUtils::Mat4 root_{ 1.0f };
std::vector<mathUtils::Mat4> world_;
std::vector<int> nodeToDraw_; // first draw for backward compatibility
std::vector<std::vector<int>> nodeToDraws_;
std::vector<int> drawToNode_;
std::vector<int> nodeToSkinnedDraw_;
std::vector<int> skinnedDrawToNode_;
std::unordered_map<std::string, std::shared_ptr<SkinnedAssetBundle>> baseSkinnedAssetCache_;
std::unordered_map<std::string, std::shared_ptr<SkinnedAssetBundle>> resolvedSkinnedAssetCache_;
std::vector<int> particleEmitterToSceneEmitter_;
std::unordered_map<std::string, MaterialHandle> materialHandles_;
bool transformsDirty_{ true };

// ECS runtime (hybrid phase)
LevelWorld ecs_{};
std::vector<EntityHandle> nodeToEntity_{};