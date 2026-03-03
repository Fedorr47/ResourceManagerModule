if (instanceBuffer_)
{
	device_.DestroyBuffer(instanceBuffer_);
}
if (highlightInstanceBuffer_)
{
	device_.DestroyBuffer(highlightInstanceBuffer_);
}
if (lightsBuffer_)
{
	device_.DestroyBuffer(lightsBuffer_);
}
if (shadowDataBuffer_)
{
	device_.DestroyBuffer(shadowDataBuffer_);
}

if (reflectionCubeDescIndex_ != 0)
{
	device_.FreeTextureDescriptor(reflectionCubeDescIndex_);
	reflectionCubeDescIndex_ = 0;
}
if (reflectionCube_)
{
	device_.DestroyTexture(reflectionCube_);
	reflectionCube_ = {};
}
if (reflectionDepthCube_)
{
	device_.DestroyTexture(reflectionDepthCube_);
	reflectionDepthCube_ = {};
}
reflectionCubeExtent_ = {};
DestroyMesh(device_, skyboxMesh_);
debugDrawRenderer_.Shutdown();
debugTextRenderer_.Shutdown();
psoCache_.ClearCache();
shaderLibrary_.ClearCache();
