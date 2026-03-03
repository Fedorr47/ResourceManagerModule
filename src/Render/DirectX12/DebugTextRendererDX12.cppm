module;

#include <cstdint>
#include <cstring>

export module core:debug_text_renderer_dx12;

import std;

import :rhi;
import :render_core;
import :file_system;
import :debug_text;

export namespace rendern::debugText
{
	// Simple “pixel font” vertex: position in pixels (top-left origin), plus color.
	struct DebugTextVertex
	{
		float x{};
		float y{};
		std::uint32_t rgba{ 0xffffffffu };
	};

	// Built-in 5x7 bitmap font (ASCII subset).
	// Each glyph is 7 rows, 5 bits per row (LSB is leftmost bit 0..4).
	struct Glyph5x7
	{
		std::array<std::uint8_t, 7> row{};
	};

	constexpr Glyph5x7 kGlyph_Space{ { 0,0,0,0,0,0,0 } };
	constexpr Glyph5x7 kGlyph_Question{
		{ 0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100 }
	};
	 constexpr Glyph5x7 kGlyph_Dot{
		{ 0,0,0,0,0,0b00100,0b00100 }
	};
	 constexpr Glyph5x7 kGlyph_Dash{
		{ 0,0,0,0b11111,0,0,0 }
	};
	 constexpr Glyph5x7 kGlyph_Colon{
		{ 0,0b00100,0b00100,0,0b00100,0b00100,0 }
	};
	 constexpr Glyph5x7 kGlyph_Slash{
		{ 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0, 0 }
	};
	 constexpr Glyph5x7 kGlyph_Plus{
		{ 0,0b00100,0b00100,0b11111,0b00100,0b00100,0 }
	};
	 constexpr Glyph5x7 kGlyph_LParen{
		{ 0b00010,0b00100,0b01000,0b01000,0b01000,0b00100,0b00010 }
	};
	 constexpr Glyph5x7 kGlyph_RParen{
		{ 0b01000,0b00100,0b00010,0b00010,0b00010,0b00100,0b01000 }
	};
	 constexpr Glyph5x7 kGlyph_LBrack{
		{ 0b01110,0b01000,0b01000,0b01000,0b01000,0b01000,0b01110 }
	};
	 constexpr Glyph5x7 kGlyph_RBrack{
		{ 0b01110,0b00010,0b00010,0b00010,0b00010,0b00010,0b01110 }
	};
	 constexpr Glyph5x7 kGlyph_Percent{
		{ 0b11001,0b11010,0b00100,0b01000,0b10110,0b00110,0 }
	};

	// Digits 0..9
	 constexpr Glyph5x7 kGlyph_0{ { 0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110 } };
	 constexpr Glyph5x7 kGlyph_1{ { 0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110 } };
	 constexpr Glyph5x7 kGlyph_2{ { 0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111 } };
	 constexpr Glyph5x7 kGlyph_3{ { 0b11110,0b00001,0b00001,0b01110,0b00001,0b00001,0b11110 } };
	 constexpr Glyph5x7 kGlyph_4{ { 0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010 } };
	 constexpr Glyph5x7 kGlyph_5{ { 0b11111,0b10000,0b10000,0b11110,0b00001,0b00001,0b11110 } };
	 constexpr Glyph5x7 kGlyph_6{ { 0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110 } };
	 constexpr Glyph5x7 kGlyph_7{ { 0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000 } };
	 constexpr Glyph5x7 kGlyph_8{ { 0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110 } };
	 constexpr Glyph5x7 kGlyph_9{ { 0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100 } };

	// Letters A..Z
	 constexpr Glyph5x7 kGlyph_A{ { 0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001 } };
	 constexpr Glyph5x7 kGlyph_B{ { 0b11110,0b10001,0b10001,0b11110,0b10001,0b10001,0b11110 } };
	 constexpr Glyph5x7 kGlyph_C{ { 0b01110,0b10001,0b10000,0b10000,0b10000,0b10001,0b01110 } };
	 constexpr Glyph5x7 kGlyph_D{ { 0b11110,0b10001,0b10001,0b10001,0b10001,0b10001,0b11110 } };
	 constexpr Glyph5x7 kGlyph_E{ { 0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b11111 } };
	 constexpr Glyph5x7 kGlyph_F{ { 0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b10000 } };
	 constexpr Glyph5x7 kGlyph_G{ { 0b01110,0b10001,0b10000,0b10111,0b10001,0b10001,0b01110 } };
	 constexpr Glyph5x7 kGlyph_H{ { 0b10001,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001 } };
	 constexpr Glyph5x7 kGlyph_I{ { 0b01110,0b00100,0b00100,0b00100,0b00100,0b00100,0b01110 } };
	 constexpr Glyph5x7 kGlyph_J{ { 0b00111,0b00010,0b00010,0b00010,0b00010,0b10010,0b01100 } };
	 constexpr Glyph5x7 kGlyph_K{ { 0b10001,0b10010,0b10100,0b11000,0b10100,0b10010,0b10001 } };
	 constexpr Glyph5x7 kGlyph_L{ { 0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111 } };
	 constexpr Glyph5x7 kGlyph_M{ { 0b10001,0b11011,0b10101,0b10101,0b10001,0b10001,0b10001 } };
	 constexpr Glyph5x7 kGlyph_N{ { 0b10001,0b11001,0b10101,0b10011,0b10001,0b10001,0b10001 } };
	 constexpr Glyph5x7 kGlyph_O{ { 0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110 } };
	 constexpr Glyph5x7 kGlyph_P{ { 0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000 } };
	 constexpr Glyph5x7 kGlyph_Q{ { 0b01110,0b10001,0b10001,0b10001,0b10101,0b10010,0b01101 } };
	 constexpr Glyph5x7 kGlyph_R{ { 0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001 } };
	 constexpr Glyph5x7 kGlyph_S{ { 0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110 } };
	 constexpr Glyph5x7 kGlyph_T{ { 0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100 } };
	 constexpr Glyph5x7 kGlyph_U{ { 0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110 } };
	 constexpr Glyph5x7 kGlyph_V{ { 0b10001,0b10001,0b10001,0b10001,0b10001,0b01010,0b00100 } };
	 constexpr Glyph5x7 kGlyph_W{ { 0b10001,0b10001,0b10001,0b10101,0b10101,0b10101,0b01010 } };
	 constexpr Glyph5x7 kGlyph_X{ { 0b10001,0b10001,0b01010,0b00100,0b01010,0b10001,0b10001 } };
	 constexpr Glyph5x7 kGlyph_Y{ { 0b10001,0b10001,0b01010,0b00100,0b00100,0b00100,0b00100 } };
	 constexpr Glyph5x7 kGlyph_Z{ { 0b11111,0b00001,0b00010,0b00100,0b01000,0b10000,0b11111 } };

	 const Glyph5x7& GetGlyph(char c) noexcept
	{
		// Normalize to upper-case for latin letters.
		if (c >= 'a' && c <= 'z')
		{
			c = static_cast<char>(c - 'a' + 'A');
		}

		switch (c)
		{
		case ' ': return kGlyph_Space;
		case '?': return kGlyph_Question;
		case '.': return kGlyph_Dot;
		case '-': return kGlyph_Dash;
		case ':': return kGlyph_Colon;
		case '/': return kGlyph_Slash;
		case '+': return kGlyph_Plus;
		case '(': return kGlyph_LParen;
		case ')': return kGlyph_RParen;
		case '[': return kGlyph_LBrack;
		case ']': return kGlyph_RBrack;
		case '%': return kGlyph_Percent;

		case '0': return kGlyph_0;
		case '1': return kGlyph_1;
		case '2': return kGlyph_2;
		case '3': return kGlyph_3;
		case '4': return kGlyph_4;
		case '5': return kGlyph_5;
		case '6': return kGlyph_6;
		case '7': return kGlyph_7;
		case '8': return kGlyph_8;
		case '9': return kGlyph_9;

		case 'A': return kGlyph_A;
		case 'B': return kGlyph_B;
		case 'C': return kGlyph_C;
		case 'D': return kGlyph_D;
		case 'E': return kGlyph_E;
		case 'F': return kGlyph_F;
		case 'G': return kGlyph_G;
		case 'H': return kGlyph_H;
		case 'I': return kGlyph_I;
		case 'J': return kGlyph_J;
		case 'K': return kGlyph_K;
		case 'L': return kGlyph_L;
		case 'M': return kGlyph_M;
		case 'N': return kGlyph_N;
		case 'O': return kGlyph_O;
		case 'P': return kGlyph_P;
		case 'Q': return kGlyph_Q;
		case 'R': return kGlyph_R;
		case 'S': return kGlyph_S;
		case 'T': return kGlyph_T;
		case 'U': return kGlyph_U;
		case 'V': return kGlyph_V;
		case 'W': return kGlyph_W;
		case 'X': return kGlyph_X;
		case 'Y': return kGlyph_Y;
		case 'Z': return kGlyph_Z;

		default:
			return kGlyph_Question;
		}
	}

	 inline void EmitQuad(std::vector<DebugTextVertex>& out,
		float x0, float y0, float x1, float y1,
		std::uint32_t rgba)
	{
		// Two triangles:
		// (x0,y0) (x1,y0) (x1,y1)
		// (x0,y0) (x1,y1) (x0,y1)
		out.push_back(DebugTextVertex{ x0, y0, rgba });
		out.push_back(DebugTextVertex{ x1, y0, rgba });
		out.push_back(DebugTextVertex{ x1, y1, rgba });

		out.push_back(DebugTextVertex{ x0, y0, rgba });
		out.push_back(DebugTextVertex{ x1, y1, rgba });
		out.push_back(DebugTextVertex{ x0, y1, rgba });
	}

	class DebugTextRendererDX12
	{
	public:
		DebugTextRendererDX12(rhi::IRHIDevice& device, ShaderLibrary& shaderLibrary, PSOCache& psoCache)
			: device_(device)
			, shaderLibrary_(shaderLibrary)
			, psoCache_(psoCache)
		{
		}

		void Upload(const DebugTextList& list)
		{
			EnsureResources();

			lastVertexCount_ = 0;
			uploadScratch_.clear();

			if (list.items.empty())
			{
				return;
			}

			// Rough reserve: average ~60 vertices per character is usually enough for short labels.
			std::size_t totalChars = 0;
			for (const auto& it : list.items)
			{
				totalChars += it.text.size();
			}
			uploadScratch_.reserve(std::max<std::size_t>(256, totalChars * 60));

			for (const auto& it : list.items)
			{
				const float cell = std::max(1.0f, it.scale);
				float penX = it.xPx;
				float penY = it.yPx;

				for (char ch : it.text)
				{
					if (ch == '\n')
					{
						penX = it.xPx;
						penY += cell * 8.0f;
						continue;
					}

					const Glyph5x7& g = GetGlyph(ch);

					// Render 5x7 pixels; 1 pixel spacing between glyphs.
					for (int row = 0; row < 7; ++row)
					{
						const std::uint8_t bits = g.row[static_cast<std::size_t>(row)];
						if (bits == 0)
						{
							continue;
						}

						for (int col = 0; col < 5; ++col)
						{
							const std::uint8_t mask = static_cast<std::uint8_t>(1u << col);
							if ((bits & mask) == 0)
							{
								continue;
							}

							const float x0 = penX + static_cast<float>(col) * cell;
							const float y0 = penY + static_cast<float>(row) * cell;
							const float x1 = x0 + cell;
							const float y1 = y0 + cell;
							EmitQuad(uploadScratch_, x0, y0, x1, y1, it.rgba);
						}
					}

					penX += cell * 6.0f; // 5 cols + 1 spacing
				}
			}

			lastVertexCount_ = static_cast<std::uint32_t>(uploadScratch_.size());
			if (lastVertexCount_ == 0)
			{
				return;
			}

			EnsureVertexBufferCapacity(lastVertexCount_);
			const std::span<const DebugTextVertex> verts{ uploadScratch_.data(), uploadScratch_.size() };
			device_.UpdateBuffer(vertexBuffer_, std::as_bytes(verts), 0);
		}

		void Draw(rhi::CommandList& cmd, std::uint32_t viewportWidth, std::uint32_t viewportHeight)
		{
			if (lastVertexCount_ == 0)
			{
				return;
			}

			EnsureResources();

			cmd.BindPipeline(psoText_);
			cmd.BindInputLayout(inputLayout_);
			cmd.BindVertexBuffer(0, vertexBuffer_, static_cast<std::uint32_t>(sizeof(DebugTextVertex)), 0);
			cmd.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);

			struct alignas(16) Constants
			{
				float uInvViewportSize[2]{};
				float _pad[2]{};
			};

			Constants c{};
			c.uInvViewportSize[0] = (viewportWidth > 0) ? (1.0f / static_cast<float>(viewportWidth)) : 0.0f;
			c.uInvViewportSize[1] = (viewportHeight > 0) ? (1.0f / static_cast<float>(viewportHeight)) : 0.0f;
			cmd.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));

			rhi::GraphicsState s{};
			s.depth.testEnable = false;
			s.depth.writeEnable = false;
			s.depth.depthCompareOp = rhi::CompareOp::Always;
			s.rasterizer.cullMode = rhi::CullMode::None;
			s.blend.enable = true;

			cmd.SetState(s);
			cmd.Draw(lastVertexCount_, 0);
		}

		void Shutdown()
		{
			if (vertexBuffer_)
			{
				device_.DestroyBuffer(vertexBuffer_);
				vertexBuffer_ = {};
			}
			vbCapacityVertices_ = 0;
			lastVertexCount_ = 0;
			uploadScratch_.clear();

			if (inputLayout_)
			{
				device_.DestroyInputLayout(inputLayout_);
				inputLayout_ = {};
			}

			psoText_ = {};
			initialized_ = false;
		}

	private:
		void EnsureResources()
		{
			if (initialized_)
			{
				return;
			}

			rhi::InputLayoutDesc il{};
			il.debugName = "DebugTextInputLayout";
			il.strideBytes = static_cast<std::uint32_t>(sizeof(DebugTextVertex));
			il.attributes = {
				rhi::VertexAttributeDesc{
					.semantic = rhi::VertexSemantic::Position,
					.semanticIndex = 0,
					.format = rhi::VertexFormat::R32G32_FLOAT,
					.inputSlot = 0,
					.offsetBytes = 0,
					.normalized = false
				},
				rhi::VertexAttributeDesc{
					.semantic = rhi::VertexSemantic::Color,
					.semanticIndex = 0,
					.format = rhi::VertexFormat::R8G8B8A8_UNORM,
					.inputSlot = 0,
					.offsetBytes = 8,
					.normalized = true
				},
			};
			inputLayout_ = device_.CreateInputLayout(il);

			const std::filesystem::path shaderPath = corefs::ResolveAsset("shaders\\DebugText_dx12.hlsl");

			rendern::ShaderKey vsKey{};
			vsKey.stage = rhi::ShaderStage::Vertex;
			vsKey.name = "VS_DebugText";
			vsKey.filePath = shaderPath.string();
			rhi::ShaderHandle vs = shaderLibrary_.GetOrCreateShader(vsKey);

			rendern::ShaderKey psKey{};
			psKey.stage = rhi::ShaderStage::Pixel;
			psKey.name = "PS_DebugText";
			psKey.filePath = shaderPath.string();
			rhi::ShaderHandle ps = shaderLibrary_.GetOrCreateShader(psKey);

			psoText_ = psoCache_.GetOrCreate("PSO_DebugText", vs, ps, rhi::PrimitiveTopologyType::Triangle);

			EnsureVertexBufferCapacity(4096);

			initialized_ = true;
		}

		void EnsureVertexBufferCapacity(std::uint32_t vertexCount)
		{
			if (vertexBuffer_.id != 0 && vbCapacityVertices_ >= vertexCount)
			{
				return;
			}

			std::uint32_t newCap = std::max<std::uint32_t>(vertexCount, 4096u);
			if (vbCapacityVertices_ != 0)
			{
				newCap = std::max<std::uint32_t>(newCap, vbCapacityVertices_ * 2u);
			}
			vbCapacityVertices_ = newCap;

			if (vertexBuffer_.id != 0)
			{
				device_.DestroyBuffer(vertexBuffer_);
				vertexBuffer_ = {};
			}

			rhi::BufferDesc vbDesc{};
			vbDesc.bindFlag = rhi::BufferBindFlag::VertexBuffer;
			vbDesc.usageFlag = rhi::BufferUsageFlag::Dynamic;
			vbDesc.sizeInBytes = static_cast<std::size_t>(vbCapacityVertices_) * sizeof(DebugTextVertex);
			vbDesc.debugName = "DebugTextVB";
			vertexBuffer_ = device_.CreateBuffer(vbDesc);
		}

	private:
		rhi::IRHIDevice& device_;
		ShaderLibrary& shaderLibrary_;
		PSOCache& psoCache_;

		rhi::InputLayoutHandle inputLayout_{};
		rhi::PipelineHandle psoText_{};
		rhi::BufferHandle vertexBuffer_{};

		std::vector<DebugTextVertex> uploadScratch_{};
		std::uint32_t vbCapacityVertices_{ 0 };
		std::uint32_t lastVertexCount_{ 0 };
		bool initialized_{ false };
	};
}