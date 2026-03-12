// -----------------------------
// LevelAsset (pure data)
// -----------------------------
struct LevelMeshDef
{
	std::string path;
	std::string debugName;
	bool flipUVs{ true };
	std::optional<std::uint32_t> submeshIndex{};
	bool bakeNodeTransforms{ true };
};

struct LevelModelDef
{
	std::string path;
	std::string debugName;
	bool flipUVs{ true };
};

struct LevelSkinnedMeshDef
{
	std::string path;
	std::string debugName;
	bool flipUVs{ true };
	std::optional<std::uint32_t> submeshIndex{};
};

struct LevelAnimationDef
{
	std::string path;
	std::string debugName;
	bool flipUVs{ true };
};

enum class LevelTextureKind : std::uint8_t
{
	Tex2D,
	Cube
};

enum class LevelCubeSource : std::uint8_t
{
	Cross,
	AutoFaces,
	Faces
};

struct LevelTextureDef
{
	LevelTextureKind kind{ LevelTextureKind::Tex2D };
	TextureProperties props{};

	// Cube options
	LevelCubeSource cubeSource{ LevelCubeSource::Cross };
	std::string baseOrDir;
	std::string preferBase; // for directory scan
	std::array<std::string, 6> facePaths{};
};

struct LevelMaterialDef
{
	Material material{};
	std::unordered_map<std::string, std::string> textureBindings; // slotName -> textureId
};

struct LevelNode
{
	std::string name;
	int parent{ -1 };

	bool visible{ true };
	bool alive{ true }; // editor/runtime tombstone (keeps indices stable)

	Transform transform{};

	std::string mesh;        // meshId
	std::string model;       // modelId (multi-draw runtime)
	std::string skinnedMesh; // skinnedMeshId
	std::string material;    // default materialId
	std::string animation;   // animation asset id
	std::string animationController; // controller asset id
	std::string animationClip;
	bool animationAutoplay{ true };
	bool animationLoop{ true };
	float animationPlayRate{ 1.0f };
	std::unordered_map<std::uint32_t, std::string> materialOverrides; // submeshIndex -> materialId
};

struct LevelAsset
{
	std::string name;
	// Remember where this level was loaded from (used by editor Save).
	// Can be asset-relative (e.g. "levels/demo.level.json") or absolute.
	std::string sourcePath;

	std::unordered_map<std::string, LevelMeshDef> meshes;
	std::unordered_map<std::string, LevelModelDef> models;
	std::unordered_map<std::string, LevelSkinnedMeshDef> skinnedMeshes;
	std::unordered_map<std::string, LevelAnimationDef> animations;
	std::unordered_map<std::string, AnimationControllerAsset> animationControllers;
	std::unordered_map<std::string, LevelTextureDef> textures;
	std::unordered_map<std::string, LevelMaterialDef> materials;

	std::optional<Camera> camera;
	std::vector<Light> lights;
	std::vector<ParticleEmitter> particleEmitters;
	std::optional<std::string> skyboxTexture; // textureId

	std::vector<LevelNode> nodes;
};

// -----------------------------
// LevelInstance (runtime glue)
// -----------------------------
enum class MaterialTextureSlot : std::uint8_t
{
	Albedo,
	Normal,
	Metalness,
	Roughness,
	AO,
	Emissive,
	Specular,
	Gloss
};

struct PendingMaterialBinding
{
	MaterialHandle material;
	MaterialTextureSlot slot{ MaterialTextureSlot::Albedo };
	std::string textureId;
};