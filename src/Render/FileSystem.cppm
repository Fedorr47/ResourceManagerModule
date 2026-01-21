module;

#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <stdexcept>

export module core:file_system;

export namespace FILE_UTILS
{
	namespace fs = std::filesystem;

	struct TextFile
	{
		std::string text;
		std::vector<fs::path> dpendencies;
	};

	struct BinaryFile
	{
		std::vector<std::byte> data;
	};

	std::string ReadAllText(const fs::path& path)
	{
		std::ifstream file(path, std::ios::binary | std::ios::in);
		if (!file)
		{
			throw std::runtime_error("Filed to open text file:" + path.string());
		}

		std::ostringstream stringStream;
		stringStream << file.rdbuf();
		return stringStream.str();
	}

	TextFile LoadTextFile(const fs::path& path)
	{
		TextFile outputFile;
		outputFile.text = ReadAllText(path);
		outputFile.dpendencies = { fs::weakly_canonical(path) };
		return outputFile;
	}

	BinaryFile ReadBinaryFile(const fs::path& path)
	{
		BinaryFile outputFile;

		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			throw std::runtime_error("Filed to open binary file:" + path.string());
		}

		file.seekg(0, std::ios::end);
		const std::size_t size = static_cast<std::size_t>(file.tellg());
		file.seekg(0, std::ios::beg);

		if (size)
		{
			outputFile.data.resize(size);
			file.read(reinterpret_cast<char*>(outputFile.data.data()), static_cast<std::streamsize>(size));
		}

		return outputFile;
	}
}

export namespace corefs
{
	using namespace FILE_UTILS;

	fs::path FindAssetRoot()
	{
		static fs::path cached;
		if (!cached.empty())
		{
			return cached;
		}

		fs::path curretnPath = fs::current_path();
		for (int i = 0; i < 10; ++i)
		{
			fs::path candidate = curretnPath / "assets";
			if (fs::exists(candidate) && fs::is_directory(candidate))
			{
				cached = candidate;
				return cached;
			}
			if (!curretnPath.has_parent_path())
			{
				break;
			}
			curretnPath = curretnPath.parent_path();
		}

		cached = fs::path("assets");
		return cached;
	}

	fs::path ResolveAsset(const fs::path& relative)
	{
		if (relative.is_absolute())
		{
			return relative;
		}
		return FindAssetRoot() / relative;
	}
}