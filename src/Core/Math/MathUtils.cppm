module;

#include <array>
#include <cstdint>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <string_view>

export module core:math_utils;

using namespace std::numbers;
using namespace std::string_view_literals;

export namespace mathUtils
{
	// Tiny engine-local math layer (GLM-like), to remove the external GLM dependency.
	//
	// Conventions are intentionally compatible with the previous GLM usage:
	//  - Mat4 is COLUMN-major and stores 4 columns (Vec4).
	//  - Indexing matches GLM: m[col][row].
	//  - Functions (Translate/Rotate/Scale/LookAtRH/PerspectiveRH_ZO/OrthoRH_ZO)
	//    follow the same formulas as GLM.

	inline constexpr float Pi = pi_v<double>;
	inline constexpr float TwoPi = Pi*2;

	struct Vec3
	{
		float x{ 0 }, y{ 0 }, z{ 0 };

		constexpr Vec3() = default;
		constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

		constexpr float& operator[](std::size_t i) noexcept
		{
			return (i == 0) ? x : (i == 1) ? y : z;
		}
		constexpr const float& operator[](std::size_t i) const noexcept
		{
			return (i == 0) ? x : (i == 1) ? y : z;
		}
	};

	struct alignas(16) Vec4
	{
		float x{ 0 }, y{ 0 }, z{ 0 }, w{ 0 };

		constexpr Vec4() = default;
		constexpr Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
		constexpr Vec4(const Vec3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}

		constexpr float& operator[](std::size_t i) noexcept
		{
			switch (i)
			{
			case 0: return x;
			case 1: return y;
			case 2: return z;
			default: return w;
			}
		}
		constexpr const float& operator[](std::size_t i) const noexcept
		{
			switch (i)
			{
			case 0: return x;
			case 1: return y;
			case 2: return z;
			default: return w;
			}
		}

		friend constexpr bool operator==(const Vec4& a, const Vec4& b) noexcept
		{
			return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
		}
	};

	struct Mat4
	{
		// 4 columns
		Vec4 c[4]{};

		constexpr Mat4() : Mat4(1.0f) {}
		explicit constexpr Mat4(float diag)
			: c{ Vec4(diag, 0, 0, 0), Vec4(0, diag, 0, 0), Vec4(0, 0, diag, 0), Vec4(0, 0, 0, diag) }
		{}

		constexpr Vec4& operator[](std::size_t col) noexcept { return c[col]; }
		constexpr const Vec4& operator[](std::size_t col) const noexcept { return c[col]; }
	};

	// --- basic ops ---
	constexpr Vec3 operator+(const Vec3& a, const Vec3& b) noexcept { return Vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
	constexpr Vec3 operator-(const Vec3& a, const Vec3& b) noexcept { return Vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
	constexpr Vec3 operator*(const Vec3& v, float s) noexcept { return Vec3(v.x * s, v.y * s, v.z * s); }
	constexpr Vec3 operator*(float s, const Vec3& v) noexcept { return v * s; }
	constexpr Vec3 operator/(const Vec3& v, float s) noexcept { return Vec3(v.x / s, v.y / s, v.z / s); }

	constexpr Vec4 operator+(const Vec4& a, const Vec4& b) noexcept { return Vec4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); }
	constexpr Vec4 operator-(const Vec4& a, const Vec4& b) noexcept { return Vec4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w); }
	constexpr Vec4 operator*(const Vec4& v, float s) noexcept { return Vec4(v.x * s, v.y * s, v.z * s, v.w * s); }
	constexpr Vec4 operator*(float s, const Vec4& v) noexcept { return v * s; }

	inline float Dot(const Vec3& a, const Vec3& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline Vec3 Cross(const Vec3& a, const Vec3& b) noexcept
	{
		return Vec3(
			a.y * b.z - a.z * b.y,
			a.z * b.x - a.x * b.z,
			a.x * b.y - a.y * b.x);
	}

	inline float Length(const Vec3& v) noexcept { return std::sqrt(Dot(v, v)); }

	inline Vec3 MakeUnitVector(const Vec3& v) noexcept
	{
		const float len = Length(v);
		if (len <= 0.0f)
		{
			return Vec3(0, 0, 0);
		}
		return v / len;
	}

	// Compatibility helpers (older code used these names).
	inline Vec3 Normalize(const Vec3& v) noexcept { return MakeUnitVector(v); }
	inline Vec3 Sub(const Vec3& a, const Vec3& b) noexcept { return a - b; }

	[[nodiscard]] inline constexpr float DegToRad(float degrees) noexcept
	{
		return degrees * (Pi / 180.0f);
	}

	[[nodiscard]] inline constexpr float RadToDeg(float radians) noexcept
	{
		return radians * (180.0f / Pi);
	}

	inline Mat4 Transpose(const Mat4& m) noexcept
	{
		Mat4 r(0.0f);
		for (int c = 0; c < 4; ++c)
		{
			for (int row = 0; row < 4; ++row)
			{
				r[row][c] = m[c][row];
			}
		}
		return r;
	}

	inline const float* ValuePtr(const Mat4& m) noexcept
	{
		return &m.c[0].x;
	}

	// Matrix * vector (column-vector convention): v' = M * v
	inline Vec4 Mul(const Mat4& m, const Vec4& v) noexcept
	{
		return m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3] * v.w;
	}

	inline Vec4 operator*(const Mat4& m, const Vec4& v) noexcept { return Mul(m, v); }

	inline Mat4 Mul(const Mat4& a, const Mat4& b) noexcept
	{
		Mat4 r(0.0f);
		// Each column of result is a * (column of b)
		for (int col = 0; col < 4; ++col)
		{
			r[col] = Mul(a, b[col]);
		}
		return r;
	}

	// Operator sugar (keeps calling code close to previous GLM style).
	//inline Vec4 operator*(const Mat4& m, const Vec4& v) noexcept { return Mul(m, v); }
	//inline Mat4 operator*(const Mat4& a, const Mat4& b) noexcept { return Mul(a, b); }

	inline Mat4 operator*(const Mat4& a, const Mat4& b) noexcept { return Mul(a, b); }

	// --- GLM-compatible transforms (column-major, post-multiply by transform) ---
	inline Mat4 Translate(const Mat4& m, const Vec3& v) noexcept
	{
		// GLM ext/matrix_transform.inl translate()
		Mat4 Result = m;
		Result[3] = m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3];
		return Result;
	}

	inline Mat4 Scale(const Mat4& m, const Vec3& v) noexcept
	{
		// GLM ext/matrix_transform.inl scale()
		Mat4 Result(0.0f);
		Result[0] = m[0] * v.x;
		Result[1] = m[1] * v.y;
		Result[2] = m[2] * v.z;
		Result[3] = m[3];
		return Result;
	}

	inline Mat4 Rotate(const Mat4& m, float angleRad, const Vec3& axisIn) noexcept
	{
		// GLM ext/matrix_transform.inl rotate()
		const float c = std::cos(angleRad);
		const float s = std::sin(angleRad);
		const Vec3 axis = MakeUnitVector(axisIn);
		const Vec3 temp = (1.0f - c) * axis;

		Mat4 RotateM(1.0f);
		RotateM[0][0] = c + temp.x * axis.x;
		RotateM[0][1] = temp.x * axis.y + s * axis.z;
		RotateM[0][2] = temp.x * axis.z - s * axis.y;

		RotateM[1][0] = temp.y * axis.x - s * axis.z;
		RotateM[1][1] = c + temp.y * axis.y;
		RotateM[1][2] = temp.y * axis.z + s * axis.x;

		RotateM[2][0] = temp.z * axis.x + s * axis.y;
		RotateM[2][1] = temp.z * axis.y - s * axis.x;
		RotateM[2][2] = c + temp.z * axis.z;

		Mat4 Result(0.0f);
		Result[0] = m[0] * RotateM[0][0] + m[1] * RotateM[0][1] + m[2] * RotateM[0][2];
		Result[1] = m[0] * RotateM[1][0] + m[1] * RotateM[1][1] + m[2] * RotateM[1][2];
		Result[2] = m[0] * RotateM[2][0] + m[1] * RotateM[2][1] + m[2] * RotateM[2][2];
		Result[3] = m[3];
		return Result;
	}

	inline Mat4 LookAtRH(const Vec3& eye, const Vec3& center, const Vec3& up) noexcept
	{
		// GLM ext/matrix_transform.inl lookAtRH()
		const Vec3 f = MakeUnitVector(center - eye);
		const Vec3 s = MakeUnitVector(Cross(f, up));
		const Vec3 u = Cross(s, f);

		Mat4 Result(1.0f);
		Result[0][0] = s.x;
		Result[1][0] = s.y;
		Result[2][0] = s.z;
		Result[0][1] = u.x;
		Result[1][1] = u.y;
		Result[2][1] = u.z;
		Result[0][2] = -f.x;
		Result[1][2] = -f.y;
		Result[2][2] = -f.z;
		Result[3][0] = -Dot(s, eye);
		Result[3][1] = -Dot(u, eye);
		Result[3][2] = Dot(f, eye);
		return Result;
	}

	inline Mat4 LookAt(const Vec3& eye, const Vec3& center, const Vec3& up) noexcept
	{
		// Keep the previous behaviour (GLM default is RH unless configured otherwise).
		return LookAtRH(eye, center, up);
	}

	inline Mat4 OrthoRH_ZO(float left, float right, float bottom, float top, float zNear, float zFar) noexcept
	{
		// GLM ext/matrix_clip_space.inl orthoRH_ZO()
		Mat4 Result(1.0f);
		Result[0][0] = 2.0f / (right - left);
		Result[1][1] = 2.0f / (top - bottom);
		Result[2][2] = 1.0f / (zNear - zFar);
		Result[3][0] = -(right + left) / (right - left);
		Result[3][1] = -(top + bottom) / (top - bottom);
		Result[3][2] = -zNear / (zFar - zNear);
		return Result;
	}

	inline Mat4 PerspectiveRH_ZO(float fovy, float aspect, float zNear, float zFar) noexcept
	{
		// GLM ext/matrix_clip_space.inl perspectiveRH_ZO()
		const float tanHalfFovy = std::tan(fovy * 0.5f);
		Mat4 Result(0.0f);
		Result[0][0] = 1.0f / (aspect * tanHalfFovy);
		Result[1][1] = 1.0f / tanHalfFovy;
		Result[2][2] = zFar / (zNear - zFar);
		Result[2][3] = -1.0f;
		Result[3][2] = -(zFar * zNear) / (zFar - zNear);
		return Result;
	}

}
