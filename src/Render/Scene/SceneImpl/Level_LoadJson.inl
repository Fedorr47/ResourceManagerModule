[[nodiscard]] AnimationParameterType ParseAnimationControllerParameterType_(std::string_view type)
{
	if (type == "bool") return AnimationParameterType::Bool;
	if (type == "int") return AnimationParameterType::Int;
	if (type == "float") return AnimationParameterType::Float;
	if (type == "trigger") return AnimationParameterType::Trigger;
	throw std::runtime_error("Level JSON: animation controller parameter type must be bool|int|float|trigger");
}

[[nodiscard]] AnimationConditionOp ParseAnimationConditionOp_(std::string_view op)
{
	if (op == "true") return AnimationConditionOp::IfTrue;
	if (op == "false") return AnimationConditionOp::IfFalse;
	if (op == ">") return AnimationConditionOp::Greater;
	if (op == ">=") return AnimationConditionOp::GreaterEqual;
	if (op == "<") return AnimationConditionOp::Less;
	if (op == "<=") return AnimationConditionOp::LessEqual;
	if (op == "==" || op == "=") return AnimationConditionOp::Equal;
	if (op == "!=") return AnimationConditionOp::NotEqual;
	if (op == "triggered") return AnimationConditionOp::Triggered;
	throw std::runtime_error("Level JSON: animation controller condition op is invalid");
}

// -----------------------------
// Loader API
// -----------------------------
LevelAsset LoadLevelAssetFromJson(std::string_view levelRelativePath)
{
	const std::filesystem::path absPath = corefs::ResolveAsset(std::filesystem::path(std::string(levelRelativePath)));
	const std::string text = FILE_UTILS::ReadAllText(absPath);

	JsonParser parser(text);
	JsonValue root = parser.Parse();
	if (!root.IsObject())
	{
		throw std::runtime_error("Level JSON: root must be object");
	}
	const JsonObject& jsonObject = root.AsObject();

	LevelAsset out;
	out.name = GetStringOpt(jsonObject, "name", "Level");
	out.sourcePath = std::string(levelRelativePath);

	// --- meshes ---
	if (auto* meshesV = TryGet(jsonObject, "meshes"))
	{
		const JsonObject& meshesO = meshesV->AsObject();
		for (const auto& [id, defV] : meshesO)
		{
			const JsonObject& md = defV.AsObject();
			LevelMeshDef def;
			def.path = GetStringOpt(md, "path");
			def.debugName = GetStringOpt(md, "debugName");
			def.flipUVs = GetBoolOpt(md, "flipUVs", true);
			def.bakeNodeTransforms = GetBoolOpt(md, "bakeNodeTransforms", true);
			if (auto* submeshV = TryGet(md, "submeshIndex"))
			{
				if (!submeshV->IsNumber())
				{
					throw std::runtime_error("Level JSON: meshes." + id + ".submeshIndex must be number");
				}
				def.submeshIndex = static_cast<std::uint32_t>(submeshV->AsNumber());
			}
			if (def.path.empty())
			{
				throw std::runtime_error("Level JSON: meshes." + id + ".path is required");
			}
			out.meshes.emplace(id, std::move(def));
		}
	}

	// --- models ---
	if (auto* modelsV = TryGet(jsonObject, "models"))
	{
		const JsonObject& modelsO = modelsV->AsObject();
		for (const auto& [id, defV] : modelsO)
		{
			const JsonObject& md = defV.AsObject();
			LevelModelDef def;
			def.path = GetStringOpt(md, "path");
			def.debugName = GetStringOpt(md, "debugName");
			def.flipUVs = GetBoolOpt(md, "flipUVs", true);
			if (def.path.empty())
			{
				throw std::runtime_error("Level JSON: models." + id + ".path is required");
			}
			out.models.emplace(id, std::move(def));
		}
	}

	// --- textures ---
	if (auto* texV = TryGet(jsonObject, "textures"))
	{
		const JsonObject& texO = texV->AsObject();
		for (const auto& [id, defV] : texO)
		{
			const JsonObject& td = defV.AsObject();
			LevelTextureDef def;

			const std::string kind = GetStringOpt(td, "kind", "tex2d");
			if (kind == "tex2d")
			{
				def.kind = LevelTextureKind::Tex2D;
				def.props.dimension = TextureDimension::Tex2D;
				def.props.filePath = GetStringOpt(td, "path");
				if (def.props.filePath.empty())
				{
					throw std::runtime_error("Level JSON: textures." + id + ".path is required for tex2d");
				}
			}
			else if (kind == "cube")
			{
				def.kind = LevelTextureKind::Cube;
				def.props.dimension = TextureDimension::Cube;

				const std::string source = GetStringOpt(td, "source", "cross");
				if (source == "cross")
				{
					def.cubeSource = LevelCubeSource::Cross;
					def.props.cubeFromCross = true;
					def.props.filePath = GetStringOpt(td, "cross");
					if (def.props.filePath.empty())
					{
						throw std::runtime_error("Level JSON: textures." + id + ".cross is required for cube/cross");
					}
				}
				else if (source == "auto")
				{
					def.cubeSource = LevelCubeSource::AutoFaces;
					def.baseOrDir = GetStringOpt(td, "baseOrDir");
					def.preferBase = GetStringOpt(td, "preferBase");
					if (def.baseOrDir.empty())
					{
						throw std::runtime_error("Level JSON: textures." + id + ".baseOrDir is required for cube/auto");
					}
				}
				else if (source == "faces")
				{
					def.cubeSource = LevelCubeSource::Faces;
					const JsonObject& facesO = GetReq(td, "faces").AsObject();
					def.facePaths[0] = GetStringOpt(facesO, "px");
					def.facePaths[1] = GetStringOpt(facesO, "nx");
					def.facePaths[2] = GetStringOpt(facesO, "py");
					def.facePaths[3] = GetStringOpt(facesO, "ny");
					def.facePaths[4] = GetStringOpt(facesO, "pz");
					def.facePaths[5] = GetStringOpt(facesO, "nz");

					for (const auto& p : def.facePaths)
					{
						if (p.empty())
						{
							throw std::runtime_error("Level JSON: textures." + id + ".faces must define px/nx/py/ny/pz/nz");
						}
					}
				}
				else
				{
					throw std::runtime_error("Level JSON: textures." + id + ".source must be cross|auto|faces");
				}
			}
			else
			{
				throw std::runtime_error("Level JSON: textures." + id + ".kind must be tex2d|cube");
			}

			// Common props
			def.props.srgb = GetBoolOpt(td, "srgb", true);
			def.props.generateMips = GetBoolOpt(td, "mips", true);
			def.props.flipY = GetBoolOpt(td, "flipY", false);
			def.props.isNormalMap = GetBoolOpt(td, "normalMap", GetBoolOpt(td, "isNormalMap", false));

			out.textures.emplace(id, std::move(def));
		}
	}

	// --- animations ---
	if (auto* animationsV = TryGet(jsonObject, "animations"))
	{
		const JsonObject& animationsO = animationsV->AsObject();
		for (const auto& [id, defV] : animationsO)
		{
			const JsonObject& md = defV.AsObject();
			LevelAnimationDef def;
			def.path = GetStringOpt(md, "path");
			def.debugName = GetStringOpt(md, "debugName");
			def.flipUVs = GetBoolOpt(md, "flipUVs", true);
			if (def.path.empty())
			{
				throw std::runtime_error("Level JSON: animations." + id + ".path is required");
			}
			out.animations.emplace(id, std::move(def));
		}
	}

	// --- animationControllers ---
	if (auto* controllersV = TryGet(jsonObject, "animationControllers"))
	{
		const JsonObject& controllersO = controllersV->AsObject();
		for (const auto& [id, defV] : controllersO)
		{
			const JsonObject& cd = defV.AsObject();
			AnimationControllerAsset def;
			def.id = id;
			def.defaultState = GetStringOpt(cd, "defaultState");

			if (auto* paramsV = TryGet(cd, "parameters"))
			{
				const JsonObject& paramsO = paramsV->AsObject();
				for (const auto& [paramName, paramV] : paramsO)
				{
					const JsonObject& pd = paramV.AsObject();
					AnimationParameterDesc paramDesc;
					paramDesc.name = paramName;
					paramDesc.defaultValue.type = ParseAnimationControllerParameterType_(GetStringOpt(pd, "type", "bool"));
					if (auto* defaultV = TryGet(pd, "default"))
					{
						switch (paramDesc.defaultValue.type)
						{
						case AnimationParameterType::Bool:
						case AnimationParameterType::Trigger:
							if (!defaultV->IsBool()) throw std::runtime_error("Level JSON: animation controller bool default must be bool");
							paramDesc.defaultValue.boolValue = defaultV->AsBool();
							paramDesc.defaultValue.triggerValue = defaultV->AsBool();
							break;
						case AnimationParameterType::Int:
							if (!defaultV->IsNumber()) throw std::runtime_error("Level JSON: animation controller int default must be number");
							paramDesc.defaultValue.intValue = static_cast<int>(defaultV->AsNumber());
							break;
						case AnimationParameterType::Float:
							if (!defaultV->IsNumber()) throw std::runtime_error("Level JSON: animation controller float default must be number");
							paramDesc.defaultValue.floatValue = static_cast<float>(defaultV->AsNumber());
							break;
						}
					}
					def.parameters.push_back(std::move(paramDesc));
				}
			}

			if (auto* statesV = TryGet(cd, "states"))
			{
				const JsonObject& statesO = statesV->AsObject();
				for (const auto& [stateName, stateV] : statesO)
				{
					const JsonObject& sd = stateV.AsObject();
					AnimationStateDesc stateDesc;
					stateDesc.name = stateName;
					stateDesc.clipName = GetStringOpt(sd, "clip");
					stateDesc.clipSourceAssetId = GetStringOpt(sd, "clipSourceAssetId");
					stateDesc.looping = GetBoolOpt(sd, "loop", true);
					stateDesc.playRate = GetFloatOpt(sd, "playRate", 1.0f);
					if (auto* blendV = TryGet(sd, "blend1D"))
					{
						const JsonObject& bd = blendV->AsObject();
						stateDesc.blendParameter = GetStringOpt(bd, "parameter");
						if (stateDesc.blendParameter.empty())
						{
							throw std::runtime_error("Level JSON: animationControllers." + id + ".states." + stateName + ".blend1D.parameter is required");
						}
						if (const AnimationParameterDesc* paramDesc = FindAnimationParameterDesc(def, stateDesc.blendParameter))
						{
							if (paramDesc->defaultValue.type == AnimationParameterType::Trigger)
							{
								throw std::runtime_error("Level JSON: animationControllers." + id + ".states." + stateName + ".blend1D.parameter must not be trigger");
							}
						}
						else
						{
							throw std::runtime_error("Level JSON: animationControllers." + id + ".states." + stateName + ".blend1D.parameter references unknown parameter");
						}
						auto* pointsV = TryGet(bd, "points");
						if (pointsV == nullptr || !pointsV->IsArray())
						{
							throw std::runtime_error("Level JSON: animationControllers." + id + ".states." + stateName + ".blend1D.points must be array");
						}
						for (const JsonValue& pointV : pointsV->AsArray())
						{
							const JsonObject& pd = pointV.AsObject();
							AnimationBlend1DPoint point;
							point.clipName = GetStringOpt(pd, "clip");
							if (point.clipName.empty())
							{
								throw std::runtime_error("Level JSON: animationControllers." + id + ".states." + stateName + ".blend1D.points[].clip is required");
							}
							point.value = GetFloatOpt(pd, "value", 0.0f);
							stateDesc.blend1D.push_back(std::move(point));
						}
						if (stateDesc.blend1D.empty())
						{
							throw std::runtime_error("Level JSON: animationControllers." + id + ".states." + stateName + ".blend1D.points must not be empty");
						}
						std::sort(stateDesc.blend1D.begin(), stateDesc.blend1D.end(), [](const AnimationBlend1DPoint& a, const AnimationBlend1DPoint& b)
							{
								return a.value < b.value;
							});
						if (stateDesc.clipName.empty())
						{
							stateDesc.clipName = stateDesc.blend1D.front().clipName;
						}
					}
					if (stateDesc.clipName.empty() && stateDesc.clipSourceAssetId.empty() && stateDesc.blend1D.empty())
					{
						throw std::runtime_error("Level JSON: animationControllers." + id + ".states." + stateName + " must define clip, clipSourceAssetId, or blend1D");
					}
					def.states.push_back(std::move(stateDesc));
				}
			}
			if (def.states.empty())
			{
				throw std::runtime_error("Level JSON: animationControllers." + id + ".states must not be empty");
			}

			if (auto* transitionsV = TryGet(cd, "transitions"))
			{
				const JsonArray& transitionsA = transitionsV->AsArray();
				for (const JsonValue& transitionV : transitionsA)
				{
					const JsonObject& td = transitionV.AsObject();
					AnimationTransitionDesc transitionDesc;
					transitionDesc.fromState = GetStringOpt(td, "from");
					transitionDesc.toState = GetStringOpt(td, "to");
					if (transitionDesc.toState.empty())
					{
						throw std::runtime_error("Level JSON: animation controller transition .to is required");
					}
					transitionDesc.hasExitTime = TryGet(td, "exitTime") != nullptr;
					transitionDesc.exitTimeNormalized = GetFloatOpt(td, "exitTime", 1.0f);
					transitionDesc.blendDurationSeconds = GetFloatOpt(td, "blendDuration", 0.15f);

					if (auto* conditionsV = TryGet(td, "conditions"))
					{
						const JsonArray& conditionsA = conditionsV->AsArray();
						for (const JsonValue& conditionV : conditionsA)
						{
							const JsonObject& condO = conditionV.AsObject();
							AnimationConditionDesc conditionDesc;
							conditionDesc.parameter = GetStringOpt(condO, "parameter");
							conditionDesc.op = ParseAnimationConditionOp_(GetStringOpt(condO, "op", "true"));
							if (const AnimationParameterDesc* paramDesc = FindAnimationParameterDesc(def, conditionDesc.parameter))
							{
								conditionDesc.value.type = paramDesc->defaultValue.type;
							}
							if (auto* valueV = TryGet(condO, "value"))
							{
								switch (conditionDesc.value.type)
								{
								case AnimationParameterType::Bool:
								case AnimationParameterType::Trigger:
									if (!valueV->IsBool()) throw std::runtime_error("Level JSON: animation controller condition bool value must be bool");
									conditionDesc.value.boolValue = valueV->AsBool();
									conditionDesc.value.triggerValue = valueV->AsBool();
									break;
								case AnimationParameterType::Int:
									if (!valueV->IsNumber()) throw std::runtime_error("Level JSON: animation controller condition int value must be number");
									conditionDesc.value.intValue = static_cast<int>(valueV->AsNumber());
									break;
								case AnimationParameterType::Float:
									if (!valueV->IsNumber()) throw std::runtime_error("Level JSON: animation controller condition float value must be number");
									conditionDesc.value.floatValue = static_cast<float>(valueV->AsNumber());
									break;
								}
							}
							transitionDesc.conditions.push_back(std::move(conditionDesc));
						}
					}
					def.transitions.push_back(std::move(transitionDesc));
				}
			}

			out.animationControllers.emplace(id, std::move(def));
		}
	}

	// --- skinnedMeshes ---
	if (auto* skinnedV = TryGet(jsonObject, "skinnedMeshes"))
	{
		const JsonObject& skinnedO = skinnedV->AsObject();
		for (const auto& [id, defV] : skinnedO)
		{
			const JsonObject& md = defV.AsObject();
			LevelSkinnedMeshDef def;
			def.path = GetStringOpt(md, "path");
			def.debugName = GetStringOpt(md, "debugName");
			def.flipUVs = GetBoolOpt(md, "flipUVs", true);
			if (auto* smi = TryGet(md, "submeshIndex"))
			{
				if (!smi->IsNumber())
				{
					throw std::runtime_error("Level JSON: skinnedMeshes." + id + ".submeshIndex must be number");
				}
				def.submeshIndex = static_cast<std::uint32_t>(smi->AsNumber());
			}
			if (def.path.empty())
			{
				throw std::runtime_error("Level JSON: skinnedMeshes." + id + ".path is required");
			}
			out.skinnedMeshes.emplace(id, std::move(def));
		}
	}


	// --- materials ---
	if (auto* matsV = TryGet(jsonObject, "materials"))
	{
		const JsonObject& matsO = matsV->AsObject();
		for (const auto& [id, defV] : matsO)
		{
			const JsonObject& md = defV.AsObject();
			LevelMaterialDef def;

			if (auto* bc = TryGet(md, "baseColor"))
			{
				auto a = ReadFloatArray(*bc, 4, "baseColor");
				def.material.params.baseColor = { a[0], a[1], a[2], a[3] };
			}

			def.material.params.shininess = GetFloatOpt(md, "shininess", def.material.params.shininess);
			def.material.params.specStrength = GetFloatOpt(md, "specStrength", def.material.params.specStrength);
			def.material.params.shadowBias = GetFloatOpt(md, "shadowBias", def.material.params.shadowBias);

			def.material.params.metallic = GetFloatOpt(md, "metallic", def.material.params.metallic);
			def.material.params.roughness = GetFloatOpt(md, "roughness", def.material.params.roughness);
			def.material.params.ao = GetFloatOpt(md, "ao", def.material.params.ao);
			def.material.params.emissiveStrength = GetFloatOpt(md, "emissiveStrength", def.material.params.emissiveStrength);

			if (auto* flagsV = TryGet(md, "flags"))
			{
				def.material.permFlags = ParsePermFlags(*flagsV);
			}

			if (auto* envV = TryGet(md, "envSource"))
			{
				def.material.envSource = ParseEnvSourceOrThrow(*envV, id);
			}
			else if (auto* envV2 = TryGet(md, "env"))
			{
				def.material.envSource = ParseEnvSourceOrThrow(*envV2, id);
			}

			if (auto* texBindV = TryGet(md, "textures"))
			{
				const JsonObject& tbo = texBindV->AsObject();
				for (const auto& [slot, tv] : tbo)
				{
					if (!tv.IsString())
					{
						throw std::runtime_error("Level JSON: materials." + id + ".textures values must be strings");
					}
					def.textureBindings.emplace(slot, tv.AsString());
				}
			}

			out.materials.emplace(id, std::move(def));
		}
	}

	// --- camera ---
	if (auto* camV = TryGet(jsonObject, "camera"))
	{
		const JsonObject& cd = camV->AsObject();
		Camera cam;
		if (auto* p = TryGet(cd, "position"))
		{
			auto a = ReadFloatArray(*p, 3, "camera.position");
			cam.position = { a[0], a[1], a[2] };
		}
		if (auto* t = TryGet(cd, "target"))
		{
			auto a = ReadFloatArray(*t, 3, "camera.target");
			cam.target = { a[0], a[1], a[2] };
		}
		if (auto* up = TryGet(cd, "up"))
		{
			auto a = ReadFloatArray(*up, 3, "camera.up");
			cam.up = { a[0], a[1], a[2] };
		}
		cam.fovYDeg = GetFloatOpt(cd, "fovYDeg", cam.fovYDeg);
		cam.nearZ = GetFloatOpt(cd, "nearZ", cam.nearZ);
		cam.farZ = GetFloatOpt(cd, "farZ", cam.farZ);
		out.camera = cam;
	}

	// --- lights ---
	if (auto* lightsV = TryGet(jsonObject, "lights"))
	{
		for (const auto& lv : lightsV->AsArray())
		{
			const JsonObject& ld = lv.AsObject();
			Light l;
			const std::string type = GetStringOpt(ld, "type", "directional");
			if (type == "directional") l.type = LightType::Directional;
			else if (type == "point") l.type = LightType::Point;
			else if (type == "spot") l.type = LightType::Spot;
			else throw std::runtime_error("Level JSON: unknown light type: " + type);

			if (auto* p = TryGet(ld, "position"))
			{
				auto a = ReadFloatArray(*p, 3, "light.position");
				l.position = { a[0], a[1], a[2] };
			}
			if (auto* d = TryGet(ld, "direction"))
			{
				auto a = ReadFloatArray(*d, 3, "light.direction");
				l.direction = mathUtils::Normalize({ a[0], a[1], a[2] });
			}
			if (auto* c = TryGet(ld, "color"))
			{
				auto a = ReadFloatArray(*c, 3, "light.color");
				l.color = { a[0], a[1], a[2] };
			}
			l.intensity = GetFloatOpt(ld, "intensity", l.intensity);
			l.range = GetFloatOpt(ld, "range", l.range);
			l.innerHalfAngleDeg = GetFloatOpt(ld, "innerHalfAngleDeg", l.innerHalfAngleDeg);
			l.outerHalfAngleDeg = GetFloatOpt(ld, "outerHalfAngleDeg", l.outerHalfAngleDeg);
			l.attConstant = GetFloatOpt(ld, "attConstant", l.attConstant);
			l.attLinear = GetFloatOpt(ld, "attLinear", l.attLinear);
			l.attQuadratic = GetFloatOpt(ld, "attQuadratic", l.attQuadratic);

			out.lights.push_back(l);
		}
	}

	// --- particle emitters ---
	if (auto* emittersV = TryGet(jsonObject, "particleEmitters"))
	{
		for (const auto& ev : emittersV->AsArray())
		{
			const JsonObject& ed = ev.AsObject();
			ParticleEmitter emitter;
			emitter.name = GetStringOpt(ed, "name");
			emitter.textureId = GetStringOpt(ed, "textureId");
			emitter.enabled = GetBoolOpt(ed, "enabled", emitter.enabled);
			emitter.looping = GetBoolOpt(ed, "looping", emitter.looping);
			if (auto* p = TryGet(ed, "position"))
			{
				auto a = ReadFloatArray(*p, 3, "particleEmitters.position");
				emitter.position = { a[0], a[1], a[2] };
			}
			if (auto* p = TryGet(ed, "positionJitter"))
			{
				auto a = ReadFloatArray(*p, 3, "particleEmitters.positionJitter");
				emitter.positionJitter = { a[0], a[1], a[2] };
			}
			if (auto* p = TryGet(ed, "velocityMin"))
			{
				auto a = ReadFloatArray(*p, 3, "particleEmitters.velocityMin");
				emitter.velocityMin = { a[0], a[1], a[2] };
			}
			if (auto* p = TryGet(ed, "velocityMax"))
			{
				auto a = ReadFloatArray(*p, 3, "particleEmitters.velocityMax");
				emitter.velocityMax = { a[0], a[1], a[2] };
			}
			if (auto* c = TryGet(ed, "color"))
			{
				auto a = ReadFloatArray(*c, 4, "particleEmitters.color");
				emitter.color = { a[0], a[1], a[2], a[3] };
				emitter.colorBegin = emitter.color;
				emitter.colorEnd = emitter.color;
			}
			if (auto* c = TryGet(ed, "colorBegin"))
			{
				auto a = ReadFloatArray(*c, 4, "particleEmitters.colorBegin");
				emitter.colorBegin = { a[0], a[1], a[2], a[3] };
			}
			if (auto* c = TryGet(ed, "colorEnd"))
			{
				auto a = ReadFloatArray(*c, 4, "particleEmitters.colorEnd");
				emitter.colorEnd = { a[0], a[1], a[2], a[3] };
			}
			if (auto* s = TryGet(ed, "size"))
			{
				auto a = ReadFloatArray(*s, 2, "particleEmitters.size");
				emitter.sizeMin = a[0];
				emitter.sizeMax = a[1];
			}
			emitter.sizeMin = GetFloatOpt(ed, "sizeMin", emitter.sizeMin);
			emitter.sizeMax = GetFloatOpt(ed, "sizeMax", emitter.sizeMax);
			emitter.sizeBegin = GetFloatOpt(ed, "sizeBegin", emitter.sizeBegin);
			emitter.sizeEnd = GetFloatOpt(ed, "sizeEnd", emitter.sizeEnd);
			if (auto* s = TryGet(ed, "lifetime"))
			{
				auto a = ReadFloatArray(*s, 2, "particleEmitters.lifetime");
				emitter.lifetimeMin = a[0];
				emitter.lifetimeMax = a[1];
			}
			emitter.lifetimeMin = GetFloatOpt(ed, "lifetimeMin", emitter.lifetimeMin);
			emitter.lifetimeMax = GetFloatOpt(ed, "lifetimeMax", emitter.lifetimeMax);
			emitter.spawnRate = GetFloatOpt(ed, "spawnRate", emitter.spawnRate);
			emitter.burstCount = static_cast<std::uint32_t>(std::max(0.0f, GetFloatOpt(ed, "burstCount", static_cast<float>(emitter.burstCount))));
			emitter.duration = GetFloatOpt(ed, "duration", emitter.duration);
			emitter.startDelay = GetFloatOpt(ed, "startDelay", emitter.startDelay);
			emitter.maxParticles = static_cast<std::uint32_t>(std::max(0.0f, GetFloatOpt(ed, "maxParticles", static_cast<float>(emitter.maxParticles))));
			out.particleEmitters.push_back(std::move(emitter));
		}
	}

	// --- skybox ---
	if (auto* sb = TryGet(jsonObject, "skybox"))
	{
		// Accept either:
		//   - string: "SkyboxTexId"
		//   - object: { "textureId": "SkyboxTexId" }  (or "texture")
		//   - null
		if (sb->IsNull())
		{
			// ok
		}
		else if (sb->IsString())
		{
			out.skyboxTexture = sb->AsString();
		}
		else if (sb->IsObject())
		{
			const JsonObject& sbo = sb->AsObject();
			const JsonValue* t = TryGet(sbo, "textureId");
			if (!t) t = TryGet(sbo, "texture");
			if (!t) t = TryGet(sbo, "id");
			if (!t)
			{
				throw std::runtime_error("Level JSON: skybox object must contain 'textureId' (or 'texture')");
			}
			if (t->IsNull())
			{
				// ok
			}
			else if (t->IsString())
			{
				out.skyboxTexture = t->AsString();
			}
			else
			{
				throw std::runtime_error("Level JSON: skybox.textureId must be string or null");
			}
		}
		else
		{
			throw std::runtime_error("Level JSON: skybox must be string, object, or null");
		}
	}

	// --- nodes ---
	if (auto* nodesV = TryGet(jsonObject, "nodes"))
	{
		for (const auto& nv : nodesV->AsArray())
		{
			const JsonObject& nd = nv.AsObject();
			LevelNode n;
			n.name = GetStringOpt(nd, "name");
			n.parent = static_cast<int>(GetFloatOpt(nd, "parent", -1.0f));
			n.visible = GetBoolOpt(nd, "visible", true);
			n.alive = GetBoolOpt(nd, "alive", true);
			if (auto* delV = TryGet(nd, "deleted"))
			{
				if (!delV->IsBool())
				{
					throw std::runtime_error("Level JSON: node.deleted must be bool");
				}
				n.alive = !delV->AsBool();
			}
			n.mesh = GetStringOpt(nd, "mesh");
			n.model = GetStringOpt(nd, "model");
			n.skinnedMesh = GetStringOpt(nd, "skinnedMesh");
			n.material = GetStringOpt(nd, "material");
			n.animation = GetStringOpt(nd, "animation");
			n.animationController = GetStringOpt(nd, "animationController");
			n.animationClip = GetStringOpt(nd, "animationClip");
			n.animationAutoplay = GetBoolOpt(nd, "animationAutoplay", true);
			n.animationLoop = GetBoolOpt(nd, "animationLoop", true);
			n.animationPlayRate = GetFloatOpt(nd, "animationPlayRate", 1.0f);
			if (auto* overridesV = TryGet(nd, "materialOverrides"))
			{
				if (!overridesV->IsObject())
				{
					throw std::runtime_error("Level JSON: node.materialOverrides must be object");
				}
				for (const auto& [submeshKey, materialValue] : overridesV->AsObject())
				{
					if (!materialValue.IsString())
					{
						throw std::runtime_error("Level JSON: node.materialOverrides values must be strings");
					}

					char* end = nullptr;
					const unsigned long parsed = std::strtoul(submeshKey.c_str(), &end, 10);
					if (end == submeshKey.c_str() || *end != '\0')
					{
						throw std::runtime_error("Level JSON: node.materialOverrides keys must be unsigned integers");
					}

					n.materialOverrides.emplace(static_cast<std::uint32_t>(parsed), materialValue.AsString());
				}
			}

			if (auto* trV = TryGet(nd, "transform"))
			{
				const JsonObject& td = trV->AsObject();
				Transform t;
				if (auto* matV = TryGet(td, "matrix"))
				{
					t.useMatrix = true;
					t.matrix = ReadMat4_ColumnMajor16(*matV, "transform.matrix");
				}
				else
				{
					if (auto* p = TryGet(td, "position"))
					{
						auto a = ReadFloatArray(*p, 3, "transform.position");
						t.position = { a[0], a[1], a[2] };
					}
					if (auto* r = TryGet(td, "rotationDegrees"))
					{
						auto a = ReadFloatArray(*r, 3, "transform.rotationDegrees");
						t.rotationDegrees = { a[0], a[1], a[2] };
					}
					if (auto* s = TryGet(td, "scale"))
					{
						auto a = ReadFloatArray(*s, 3, "transform.scale");
						t.scale = { a[0], a[1], a[2] };
					}
				}
				n.transform = t;
			}

			out.nodes.push_back(std::move(n));
		}
	}

	return out;
}