void SaveLevelAssetToJson(std::string_view levelRelativeOrAbsPath, const LevelAsset& level)
{
	namespace fs = std::filesystem;

	// --- alive-only node remap (indices in JSON are dense) ---
	std::vector<int> oldToNew(level.nodes.size(), -1);
	std::vector<int> newToOld;
	newToOld.reserve(level.nodes.size());
	for (std::size_t i = 0; i < level.nodes.size(); ++i)
	{
		if (!level.nodes[i].alive)
			continue;
		oldToNew[i] = static_cast<int>(newToOld.size());
		newToOld.push_back(static_cast<int>(i));
	}

	const fs::path absPath = corefs::ResolveAsset(fs::path(std::string(levelRelativeOrAbsPath)));
	fs::create_directories(absPath.parent_path());

	std::ofstream file(absPath, std::ios::binary | std::ios::trunc);
	if (!file)
	{
		throw std::runtime_error("Level JSON: failed to open for write: " + absPath.string());
	}

	std::ostringstream ss;
	ss.setf(std::ios::fixed);
	ss << std::setprecision(6);

	ss << "{\n";
	ss << "  \"name\": ";
	WriteJsonEscaped(ss, level.name);
	ss << ",\n";

	// meshes
	ss << "  \"meshes\": {";
	{
		auto keys = SortedStringKeys(level.meshes);
		for (std::size_t i = 0; i < keys.size(); ++i)
		{
			const auto& id = keys[i];
			const LevelMeshDef& md = level.meshes.at(id);
			if (i == 0) ss << "\n"; else ss << ",\n";
			ss << "    ";
			WriteJsonEscaped(ss, id);
			ss << ": {\"path\": ";
			WriteJsonEscaped(ss, md.path);
			if (!md.debugName.empty())
			{
				ss << ", \"debugName\": ";
				WriteJsonEscaped(ss, md.debugName);
			}
			ss << "}";
		}
		if (!keys.empty()) ss << "\n  ";
	}
	ss << "},\n";

	// textures
	ss << "  \"textures\": {";
	{
		auto keys = SortedStringKeys(level.textures);
		for (std::size_t i = 0; i < keys.size(); ++i)
		{
			const auto& id = keys[i];
			const LevelTextureDef& td = level.textures.at(id);
			if (i == 0) ss << "\n"; else ss << ",\n";
			ss << "    ";
			WriteJsonEscaped(ss, id);
			ss << ": {";
			if (td.kind == LevelTextureKind::Tex2D)
			{
				ss << "\"kind\": \"tex2d\", \"path\": ";
				WriteJsonEscaped(ss, td.props.filePath);
				ss << ", \"srgb\": ";
				WriteJsonBool(ss, td.props.srgb);
				ss << ", \"mips\": ";
				WriteJsonBool(ss, td.props.generateMips);
				ss << ", \"flipY\": ";
				WriteJsonBool(ss, td.props.flipY);
			}
			else
			{
				ss << "\"kind\": \"cube\"";
				if (td.cubeSource == LevelCubeSource::Cross)
				{
					ss << ", \"source\": \"cross\", \"cross\": ";
					WriteJsonEscaped(ss, td.props.filePath);
				}
				else if (td.cubeSource == LevelCubeSource::AutoFaces)
				{
					ss << ", \"source\": \"auto\", \"baseOrDir\": ";
					WriteJsonEscaped(ss, td.baseOrDir);
					if (!td.preferBase.empty())
					{
						ss << ", \"preferBase\": ";
						WriteJsonEscaped(ss, td.preferBase);
					}
				}
				else // Faces
				{
					ss << ", \"source\": \"faces\", \"faces\": {";
					ss << "\"px\": ";
					WriteJsonEscaped(ss, td.facePaths[0]);
					ss << ", \"nx\": ";
					WriteJsonEscaped(ss, td.facePaths[1]);
					ss << ", \"py\": ";
					WriteJsonEscaped(ss, td.facePaths[2]);
					ss << ", \"ny\": ";
					WriteJsonEscaped(ss, td.facePaths[3]);
					ss << ", \"pz\": ";
					WriteJsonEscaped(ss, td.facePaths[4]);
					ss << ", \"nz\": ";
					WriteJsonEscaped(ss, td.facePaths[5]);
					ss << "}";
				}
				ss << ", \"srgb\": ";
				WriteJsonBool(ss, td.props.srgb);
				ss << ", \"mips\": ";
				WriteJsonBool(ss, td.props.generateMips);
				ss << ", \"flipY\": ";
				WriteJsonBool(ss, td.props.flipY);
			}
			ss << "}";
		}
		if (!keys.empty()) ss << "\n  ";
	}
	ss << "},\n";

	// materials
	ss << "  \"materials\": {";
	{
		auto keys = SortedStringKeys(level.materials);
		for (std::size_t i = 0; i < keys.size(); ++i)
		{
			const auto& id = keys[i];
			const LevelMaterialDef& md = level.materials.at(id);
			const MaterialParams& p = md.material.params;

			if (i == 0) ss << "\n"; else ss << ",\n";
			ss << "    ";
			WriteJsonEscaped(ss, id);
			ss << ": {";
			ss << "\"baseColor\": ";
			WriteJsonVec4(ss, p.baseColor);
			ss << ", \"shininess\": ";
			WriteJsonFloat(ss, p.shininess);
			ss << ", \"specStrength\": ";
			WriteJsonFloat(ss, p.specStrength);
			ss << ", \"shadowBias\": ";
			WriteJsonFloat(ss, p.shadowBias);
			ss << ", \"metallic\": ";
			WriteJsonFloat(ss, p.metallic);
			ss << ", \"roughness\": ";
			WriteJsonFloat(ss, p.roughness);
			ss << ", \"ao\": ";
			WriteJsonFloat(ss, p.ao);
			ss << ", \"emissiveStrength\": ";
			WriteJsonFloat(ss, p.emissiveStrength);
			if (md.material.envSource == EnvSource::ReflectionCapture)
			{
				ss << ", \"envSource\": \"reflectionCapture\"";
			}

			// flags as array (parser expects array)
			ss << ", \"flags\": [";
			bool firstFlag = true;
			auto emitFlag = [&](std::string_view f)
				{
					if (!firstFlag) ss << ", ";
					WriteJsonEscaped(ss, f);
					firstFlag = false;
				};
			if (HasFlag(md.material.permFlags, MaterialPerm::UseTex)) emitFlag("useTex");
			if (HasFlag(md.material.permFlags, MaterialPerm::UseShadow)) emitFlag("useShadow");
			if (HasFlag(md.material.permFlags, MaterialPerm::Skinning)) emitFlag("skinning");
			if (HasFlag(md.material.permFlags, MaterialPerm::Transparent)) emitFlag("transparent");
			if (HasFlag(md.material.permFlags, MaterialPerm::PlanarMirror)) emitFlag("planarMirror");
			ss << "]";

			// texture bindings
			ss << ", \"textures\": {";
			{
				auto tkeys = SortedStringKeys(md.textureBindings);
				for (std::size_t ti = 0; ti < tkeys.size(); ++ti)
				{
					const auto& slot = tkeys[ti];
					const std::string& texId = md.textureBindings.at(slot);
					if (ti == 0) ss << "\n"; else ss << ",\n";
					ss << "      ";
					WriteJsonEscaped(ss, slot);
					ss << ": ";
					WriteJsonEscaped(ss, texId);
				}
				if (!tkeys.empty()) ss << "\n    ";
			}
			ss << "}";

			ss << "}";
		}
		if (!keys.empty()) ss << "\n  ";
	}
	ss << "},\n";

	// camera (always write if present)
	if (level.camera)
	{
		const Camera& cam = *level.camera;
		ss << "  \"camera\": {\"position\": ";
		WriteJsonVec3(ss, cam.position);
		ss << ", \"target\": ";
		WriteJsonVec3(ss, cam.target);
		ss << ", \"up\": ";
		WriteJsonVec3(ss, cam.up);
		ss << ", \"fovYDeg\": ";
		WriteJsonFloat(ss, cam.fovYDeg);
		ss << ", \"nearZ\": ";
		WriteJsonFloat(ss, cam.nearZ);
		ss << ", \"farZ\": ";
		WriteJsonFloat(ss, cam.farZ);
		ss << "},\n";
	}

	// lights
	ss << "  \"lights\": [";
	for (std::size_t i = 0; i < level.lights.size(); ++i)
	{
		const Light& l = level.lights[i];
		if (i == 0) ss << "\n"; else ss << ",\n";
		ss << "    {";
		std::string_view type = "directional";
		if (l.type == LightType::Point) type = "point";
		else if (l.type == LightType::Spot) type = "spot";
		ss << "\"type\": ";
		WriteJsonEscaped(ss, type);
		ss << ", \"position\": ";
		WriteJsonVec3(ss, l.position);
		ss << ", \"direction\": ";
		WriteJsonVec3(ss, l.direction);
		ss << ", \"color\": ";
		WriteJsonVec3(ss, l.color);
		ss << ", \"intensity\": ";
		WriteJsonFloat(ss, l.intensity);
		ss << ", \"range\": ";
		WriteJsonFloat(ss, l.range);
		ss << ", \"innerHalfAngleDeg\": ";
		WriteJsonFloat(ss, l.innerHalfAngleDeg);
		ss << ", \"outerHalfAngleDeg\": ";
		WriteJsonFloat(ss, l.outerHalfAngleDeg);
		ss << ", \"attConstant\": ";
		WriteJsonFloat(ss, l.attConstant);
		ss << ", \"attLinear\": ";
		WriteJsonFloat(ss, l.attLinear);
		ss << ", \"attQuadratic\": ";
		WriteJsonFloat(ss, l.attQuadratic);
		ss << "}";
	}
	if (!level.lights.empty()) ss << "\n  ";
	ss << "],\n";

	// skybox: write as string or null (keep loader happy and stable)
	ss << "  \"skybox\": ";
	if (level.skyboxTexture && !level.skyboxTexture->empty())
	{
		WriteJsonEscaped(ss, *level.skyboxTexture);
	}
	else
	{
		ss << "null";
	}
	ss << ",\n";

	// nodes (alive only)
	ss << "  \"nodes\": [";
	for (std::size_t ni = 0; ni < newToOld.size(); ++ni)
	{
		const LevelNode& n = level.nodes[static_cast<std::size_t>(newToOld[ni])];
		if (ni == 0) ss << "\n"; else ss << ",\n";
		ss << "    {";
		ss << "\"name\": ";
		WriteJsonEscaped(ss, n.name);

		int parent = -1;
		if (n.parent >= 0)
		{
			const std::size_t op = static_cast<std::size_t>(n.parent);
			if (op < oldToNew.size() && oldToNew[op] >= 0)
				parent = oldToNew[op];
		}
		ss << ", \"parent\": " << parent;
		ss << ", \"visible\": ";
		WriteJsonBool(ss, n.visible);

		if (!n.mesh.empty())
		{
			ss << ", \"mesh\": ";
			WriteJsonEscaped(ss, n.mesh);
		}
		if (!n.material.empty())
		{
			ss << ", \"material\": ";
			WriteJsonEscaped(ss, n.material);
		}

		ss << ", \"transform\": {";
		if (n.transform.useMatrix)
		{
			ss << "\"matrix\": ";
			WriteJsonMat4ColMajor16(ss, n.transform.matrix);
		}
		else
		{
			ss << "\"position\": ";
			WriteJsonVec3(ss, n.transform.position);
			ss << ", \"rotationDegrees\": ";
			WriteJsonVec3(ss, n.transform.rotationDegrees);
			ss << ", \"scale\": ";
			WriteJsonVec3(ss, n.transform.scale);
		}
		ss << "}";

		ss << "}";
	}
	if (!newToOld.empty()) ss << "\n  ";
	ss << "]\n";

	ss << "}\n";

	const std::string outText = ss.str();
	file.write(outText.data(), static_cast<std::streamsize>(outText.size()));
	file.flush();
	if (!file)
	{
		throw std::runtime_error("Level JSON: failed to write: " + absPath.string());
	}
}