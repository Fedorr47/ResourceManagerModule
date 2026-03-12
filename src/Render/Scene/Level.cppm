module;

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <variant>
#include <optional>
#include <array>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <limits>
#include <cmath>
#include <cassert>

export module core:level;
import :scene; 
import :level_ecs; 
import :asset_manager; 
import :resource_manager; 
import :render_bindless; 
import :file_system; 
import :math_utils;
import :assimp_scene_loader;
import :assimp_loader;
import :animator;
import :animation_controller;
import :animation_clip;

// ------------------------------------------------------------
// LevelAsset / LevelInstance
#include "SceneImpl/Level_JsonSupport.inl"

export namespace rendern
{
#include "SceneImpl/Level_AssetTypes.inl"

	class LevelInstance
	{
	public:
		LevelInstance() = default;

#include "SceneImpl/Level_LevelInstance_RuntimeBindings.inl"
#include "SceneImpl/Level_LevelInstance_Editing.inl"

	private:
		friend LevelInstance InstantiateLevel(Scene& scene, AssetManager& assets, BindlessTable& bindless, const LevelAsset& asset, const mathUtils::Mat4& root);

#include "SceneImpl/Level_LevelInstance_PrivateHelpers.inl"
#include "SceneImpl/Level_LevelInstance_State.inl"
	};

#include "SceneImpl/Level_LoadJson.inl"
#include "SceneImpl/Level_InstantiateRuntime.inl"
#include "SceneImpl/Level_SaveJson.inl"
}