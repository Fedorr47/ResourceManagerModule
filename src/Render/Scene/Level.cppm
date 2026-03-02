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

export module core:level;

import :scene;
import :asset_manager;
import :resource_manager;
import :render_bindless;
import :file_system;
import :math_utils;

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