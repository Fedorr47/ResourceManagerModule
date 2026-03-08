// ---- Combine and upload once ----
auto AlignUpU32 = [](std::uint32_t v, std::uint32_t a) -> std::uint32_t
	{
		return (v + (a - 1u)) / a * a;
	};
const std::uint32_t shadowBase = 0;
const std::uint32_t mainBase = static_cast<std::uint32_t>(shadowInstances.size());
const std::uint32_t captureMainBase = static_cast<std::uint32_t>(shadowInstances.size() + mainInstances.size());
const std::uint32_t transparentBase = captureMainBase + static_cast<std::uint32_t>(captureMainInstancesNoCull.size());
const std::uint32_t planarMirrorBase = transparentBase + static_cast<std::uint32_t>(transparentInstances.size());

const std::uint32_t transparentEnd =
planarMirrorBase + static_cast<std::uint32_t>(planarMirrorInstances.size());
const std::uint32_t layeredShadowBase = AlignUpU32(transparentEnd, 6u);
const std::uint32_t layeredReflectionBase =
AlignUpU32(layeredShadowBase + static_cast<std::uint32_t>(shadowInstancesLayered.size()), 6u);

for (auto& sbatch : shadowBatches)
{
	sbatch.instanceOffset += shadowBase;
}
for (auto& mbatch : mainBatches)
{
	mbatch.instanceOffset += mainBase;
}
for (auto& cbatch : captureMainBatchesNoCull)
{
	cbatch.instanceOffset += captureMainBase;
}
for (auto& lbatch : shadowBatchesLayered)
{
	lbatch.instanceOffset += layeredShadowBase;
}
for (auto& rbatch : reflectionBatchesLayered)
{
	rbatch.instanceOffset += layeredReflectionBase;
}
for (auto& mirrorDraw : planarMirrorDraws)
{
	mirrorDraw.instanceOffset = planarMirrorBase + mirrorDraw.instanceOffset;
}

transparentDrawsScratch_.clear();
transparentDrawsScratch_.reserve(transparentTmp.size());
auto& transparentDraws = transparentDrawsScratch_;
for (const auto& transparentInst : transparentTmp)
{
	TransparentDraw transparentDraw{};
	transparentDraw.mesh = transparentInst.mesh;
	transparentDraw.material = transparentInst.material;
	transparentDraw.materialHandle = transparentInst.materialHandle;
	transparentDraw.instanceOffset = transparentBase + transparentInst.localInstanceOffset;
	transparentDraw.dist2 = transparentInst.dist2;
	transparentDraws.push_back(transparentDraw);
}

std::sort(transparentDraws.begin(), transparentDraws.end(),
	[](const TransparentDraw& first, const TransparentDraw& second)
	{
		return first.dist2 > second.dist2; // far -> near
	});

combinedInstancesScratch_.clear();
auto& combinedInstances = combinedInstancesScratch_;
const std::uint32_t finalCount =
layeredReflectionBase + static_cast<std::uint32_t>(reflectionInstancesLayered.size());

combinedInstances.clear();
combinedInstances.reserve(finalCount);

// 1) normal groups
combinedInstances.insert(combinedInstances.end(), shadowInstances.begin(), shadowInstances.end());
combinedInstances.insert(combinedInstances.end(), mainInstances.begin(), mainInstances.end());
combinedInstances.insert(combinedInstances.end(), captureMainInstancesNoCull.begin(), captureMainInstancesNoCull.end());
combinedInstances.insert(combinedInstances.end(), transparentInstances.begin(), transparentInstances.end());
combinedInstances.insert(combinedInstances.end(), planarMirrorInstances.begin(), planarMirrorInstances.end());

// 2) pad up to layeredShadowBase (between transparent/planar-mirror and layered shadow)
if (combinedInstances.size() < layeredShadowBase)
	combinedInstances.resize(layeredShadowBase);

// 3) layered shadow
combinedInstances.insert(combinedInstances.end(),
	shadowInstancesLayered.begin(), shadowInstancesLayered.end());

// 4) pad up to layeredReflectionBase (between layered shadow and layered reflection)
if (combinedInstances.size() < layeredReflectionBase)
	combinedInstances.resize(layeredReflectionBase);

// 5) layered reflection
combinedInstances.insert(combinedInstances.end(),
	reflectionInstancesLayered.begin(), reflectionInstancesLayered.end());

assert(shadowBase == 0u);
assert(mainBase == shadowInstances.size());
assert(captureMainBase == shadowInstances.size() + mainInstances.size());
assert(transparentBase == captureMainBase + captureMainInstancesNoCull.size());
assert(planarMirrorBase == transparentBase + transparentInstances.size());
assert(layeredShadowBase >= planarMirrorBase + planarMirrorInstances.size());
assert(layeredReflectionBase >= layeredShadowBase + shadowInstancesLayered.size());
assert(combinedInstances.size() == finalCount);

const std::uint32_t instStride = static_cast<std::uint32_t>(sizeof(InstanceData));

if (!combinedInstances.empty())
{
	const std::size_t bytes = combinedInstances.size() * sizeof(InstanceData);
	if (bytes > instanceBufferSizeBytes_)
	{
		throw std::runtime_error("DX12Renderer: instance buffer overflow (increase instanceBufferSizeBytes_)");
	}
	device_.UpdateBuffer(instanceBuffer_, std::as_bytes(std::span{ combinedInstances }));
}

if (settings_.debugPrintDrawCalls)
{
	static std::uint32_t frame = 0;
	if ((++frame % 60u) == 0u)
	{
		std::cout << "[DX12] MainPass draw calls: " << mainBatches.size()
			<< " (instances main: " << mainInstances.size()
			<< ", shadow: " << shadowInstances.size() << ")"
			<< " | DepthPrepass: " << (settings_.enableDepthPrepass ? "ON" : "OFF")
			<< " (draw calls: " << shadowBatches.size() << ")\n";
	}
}