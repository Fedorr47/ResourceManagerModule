module;

#include <cstdint>
#include <vector>
#include <string>

export module core:shader_system;

export namespace render
{
	enum class ShaderBlobType : std::uint8_t
	{
		Text,	// GLSL/HLSL source code
		Spriv,	// SPIR-V bytecode
		Dxil	// DXIL bytecode
	};

	struct ShaderBlob
	{
		ShaderBlobType type{ ShaderBlobType::Text };
		std::vector<std::byte> data;
		std::string debugName;

		static ShaderBlob FromText(std::string_view debugName, std::string_view source)
		{
			ShaderBlob blob;
			blob.type = ShaderBlobType::Text;
			blob.debugName = debugName;

			blob.data.resize(source.size());
			if (!source.empty())
				std::memcpy(blob.data.data(), source.data(), source.size());

			return blob;
		}
	};

	struct ShaderCompileRequest
	{
		std::string debugName{};
		std::string_view sourceText{};
		std::string entryPoint{ "main" };
		std::vector<std::string> defines{};
	};


	struct ShaderCompileResult
	{
		bool success{ false };
		ShaderBlob blob;
		std::string errorMessage;
	};

	class IShaderCompiler
	{
	public:
		virtual ~IShaderCompiler() = default;
		virtual ShaderCompileResult CompileVertexShader(const ShaderCompileRequest& request) = 0;
		virtual ShaderCompileResult CompilePixelShader(const ShaderCompileRequest& request) = 0;
	};

	class GLSLPasshroughCompiler final : public IShaderCompiler
	{
	public:
		ShaderCompileResult CompileVertexShader(const ShaderCompileRequest& request) override
		{
			ShaderCompileResult result;
			result.success = true;
			result.blob = ShaderBlob::FromText(request.debugName, request.sourceText);
			return result;
		}

		ShaderCompileResult CompilePixelShader(const ShaderCompileRequest& request) override
		{
			ShaderCompileResult result;
			result.success = true;
			result.blob = ShaderBlob::FromText(request.debugName, request.sourceText);
			return result;
		}
	};
}