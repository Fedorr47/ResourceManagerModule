#include <gtest/gtest.h>

import core;

using namespace mathUtils;

TEST(MathUtils, Vec2)
{
	Vec2 vector2{ 1.0f, 2.0f };
	EXPECT_FLOAT_EQ(vector2.x, 1.0f);
	EXPECT_FLOAT_EQ(vector2.y, 2.0f);
}

TEST(MathUtils, Vec3)
{
	Vec3 vector3{ 1.0f, 2.0f, 3.0f };
	EXPECT_FLOAT_EQ(vector3.x, 1.0f);
	EXPECT_FLOAT_EQ(vector3.y, 2.0f);
	EXPECT_FLOAT_EQ(vector3.z, 3.0f);
}

TEST(MathUtils, Vec4)
{
	Vec4 vector4{ 1.0f, 2.0f, 3.0f, 1.0f };
	EXPECT_FLOAT_EQ(vector4.x, 1.0f);
	EXPECT_FLOAT_EQ(vector4.y, 2.0f);
	EXPECT_FLOAT_EQ(vector4.z, 3.0f);
	EXPECT_FLOAT_EQ(vector4.w, 1.0f);
}

TEST(MathUtils, Vec2Operations)
{
	Vec2 vector2_1{ 1.0f, 2.0f };
	Vec2 vector2_2{ 3.0f, 4.0f };

	const float value = 2.0f;

	Vec2 added = vector2_1 + vector2_2;
	EXPECT_FLOAT_EQ(added.x, 4.0f);
	EXPECT_FLOAT_EQ(added.y, 6.0f);

	Vec2 subtracted = vector2_1 - vector2_2;
	EXPECT_FLOAT_EQ(subtracted.x, -2.0f);
	EXPECT_FLOAT_EQ(subtracted.y, -2.0f);

	Vec2 multiplied = vector2_1 * value;
	EXPECT_FLOAT_EQ(multiplied.x, 2.0f);
	EXPECT_FLOAT_EQ(multiplied.y, 4.0f);

	Vec2 multiplied2 = value * vector2_1;
	EXPECT_FLOAT_EQ(multiplied2.x, 2.0f);
	EXPECT_FLOAT_EQ(multiplied2.y, 4.0f);

	Vec2 divided = vector2_1 / value;
	EXPECT_FLOAT_EQ(divided.x, 0.5f);
	EXPECT_FLOAT_EQ(divided.y, 1.0f);

	float DotProduct = Dot(vector2_1, vector2_2);
	EXPECT_FLOAT_EQ(DotProduct, 11.0f);

	Vec3 CrossProduct2 = Cross2(vector2_1, vector2_2);
	EXPECT_FLOAT_EQ(CrossProduct2.z, -4.0f);
}

TEST(MathUtils, Vec3Operations)
{
	Vec3 vector3_1{ 1.0f, 2.0f, 3.0f };
	Vec3 vector3_2{ 4.0f, 5.0f, 6.0f };

	const float value = 2.0f;

	Vec3 added = vector3_1 + vector3_2;
	EXPECT_FLOAT_EQ(added.x, 5.0f);
	EXPECT_FLOAT_EQ(added.y, 7.0f);
	EXPECT_FLOAT_EQ(added.z, 9.0f);

	Vec3 subtracted = vector3_1 - vector3_2;
	EXPECT_FLOAT_EQ(subtracted.x, -3.0f);
	EXPECT_FLOAT_EQ(subtracted.y, -3.0f);
	EXPECT_FLOAT_EQ(subtracted.z, -3.0f);

	Vec3 multiplied = vector3_1 * value;
	EXPECT_FLOAT_EQ(multiplied.x, 2.0f);
	EXPECT_FLOAT_EQ(multiplied.y, 4.0f);
	EXPECT_FLOAT_EQ(multiplied.z, 6.0f);

	Vec3 multiplied2 = value * vector3_1;
	EXPECT_FLOAT_EQ(multiplied2.x, 2.0f);
	EXPECT_FLOAT_EQ(multiplied2.y, 4.0f);
	EXPECT_FLOAT_EQ(multiplied2.z, 6.0f);

	Vec3 divided = vector3_1 / value;
	EXPECT_FLOAT_EQ(divided.x, 0.5f);
	EXPECT_FLOAT_EQ(divided.y, 1.0f);
	EXPECT_FLOAT_EQ(divided.z, 1.5f);

	float DotProduct = Dot(vector3_1, vector3_2);
	EXPECT_FLOAT_EQ(DotProduct, 32.0f);

	Vec3 CrossProduct = Cross(vector3_1, vector3_2);
	EXPECT_FLOAT_EQ(CrossProduct.x, -3.0f);
	EXPECT_FLOAT_EQ(CrossProduct.y, 6.0f);
	EXPECT_FLOAT_EQ(CrossProduct.z, -3.0f);
}

TEST(MathUtils, Vec3_CrossProduct)
{
	Vec3 vector3_1{ 1.0f, 0.0f, 0.0f };
	Vec3 vector3_2{ 0.0f, 1.0f, 0.0f };
	Vec3 vector3_3{ 0.0f, 0.0f, 1.0f };

	Vec3 CrossProduct = Cross(vector3_1, vector3_2);
	EXPECT_FLOAT_EQ(CrossProduct.x, 0.0f);
	EXPECT_FLOAT_EQ(CrossProduct.y, 0.0f);
	EXPECT_FLOAT_EQ(CrossProduct.z, 1.0f);

	float DotProduct = Dot(CrossProduct, vector3_1);
	EXPECT_FLOAT_EQ(DotProduct, 0.0f);
	DotProduct = Dot(CrossProduct, vector3_2);
	EXPECT_FLOAT_EQ(DotProduct, 0.0f);
}

TEST(MathUtils, Normalize)
{
	Vec3 vector3{ 3.0f, 4.0f, 0.0f };
	float length = Length(vector3);
	EXPECT_FLOAT_EQ(length, 5.0f);

	Vec3 normalized = Normalize(vector3);
	length = Length(normalized);
	EXPECT_FLOAT_EQ(length, 1.0f);

	Vec3 vector_zero{ 0.0f, 0.0f, 0.0f };
	Vec3 normalized_zero = Normalize(vector_zero);
	EXPECT_FLOAT_EQ(normalized_zero.x, 0.0f);
	EXPECT_FLOAT_EQ(normalized_zero.y, 0.0f);
	EXPECT_FLOAT_EQ(normalized_zero.z, 0.0f);
}

TEST(MathUtils, DegRad)
{
	float degrees = 180.0f;
	float radians = DegToRad(degrees);
	EXPECT_FLOAT_EQ(radians, Pi);

	float converted_back = RadToDeg(radians);
	EXPECT_FLOAT_EQ(converted_back, degrees);

	degrees = 90.0f;
	radians = DegToRad(degrees);
	EXPECT_FLOAT_EQ(radians, Pi / 2.0f);
	converted_back = RadToDeg(radians);
	EXPECT_FLOAT_EQ(converted_back, degrees);

	degrees = 45.0f;
	radians = DegToRad(degrees);
	EXPECT_FLOAT_EQ(radians, Pi / 4.0f);
	converted_back = RadToDeg(radians);
	EXPECT_FLOAT_EQ(converted_back, degrees);
}

TEST(MathUtils, Mat4Identity)
{
	Mat4 identity;
	EXPECT_FLOAT_EQ(identity[0][0], 1.0f);
	EXPECT_FLOAT_EQ(identity[1][1], 1.0f);
	EXPECT_FLOAT_EQ(identity[2][2], 1.0f);
	EXPECT_FLOAT_EQ(identity[3][3], 1.0f);
	EXPECT_FLOAT_EQ(identity[0][1], 0.0f);
	EXPECT_FLOAT_EQ(identity[0][2], 0.0f);
	EXPECT_FLOAT_EQ(identity[0][3], 0.0f);
	EXPECT_FLOAT_EQ(identity[1][0], 0.0f);
	EXPECT_FLOAT_EQ(identity[1][2], 0.0f);
	EXPECT_FLOAT_EQ(identity[1][3], 0.0f);
}

TEST(MathUtils, Mat4Transponse)
{
	Mat4 matrix{};

	float counter = 1.0f;
	for (int col = 0; col < 4; ++col)
	{
		for (int row = 0; row < 4; ++row)
		{
			matrix[col][row] = counter++;
		}
	}

	Mat4 transposed = Transpose(matrix);
	for (int col = 0; col < 4; ++col)
	{
		for (int row = 0; row < 4; ++row)
		{
			EXPECT_FLOAT_EQ(transposed[col][row], matrix[row][col]);
		}
	}
}

TEST(MathUtils, Mat4Mul)
{
	Mat4 matrixA{};
	Mat4 matrixB{};

	float counter = 1.0f;
	for (int col = 0; col < 4; ++col)
	{
		for (int row = 0; row < 4; ++row)
		{
			matrixA[col][row] = counter++;
			matrixB[col][row] = counter++;
		}
	}

	Mat4 multiplied = matrixA * matrixB;
	for (int col = 0; col < 4; ++col)
	{
		for (int row = 0; row < 4; ++row)
		{
			float expected_value = 0.0f;
			for (int k = 0; k < 4; ++k)
			{
				expected_value += matrixA[k][row] * matrixB[col][k];
			}
			EXPECT_FLOAT_EQ(multiplied[col][row], expected_value);
		}
	}
}

TEST(MathUtils, Mat4Vec4Mul)
{
	Mat4 matrix{};
	Vec4 vector{ 1.0f, 2.0f, 3.0f, 1.0f };
	float counter = 1.0f;
	for (int col = 0; col < 4; ++col)
	{
		for (int row = 0; row < 4; ++row)
		{
			matrix[col][row] = counter++;
		}
	}
	Vec4 result = matrix * vector;
	for (int row = 0; row < 4; ++row)
	{
		float expected_value = 0.0f;
		for (int k = 0; k < 4; ++k)
		{
			expected_value += matrix[k][row] * vector[k];
		}
		EXPECT_FLOAT_EQ(result[row], expected_value);
	}
}

TEST(MathUtils, Mat4Inverse)
{
	Mat4 matrix(1.0f);
	matrix.columns[0] = Vec4(1.0f, 0.0f, 0.0f, 0.0f);
	matrix.columns[1] = Vec4(1.0f, 1.0f, 0.0f, 0.0f);
	matrix.columns[2] = Vec4(0.0f, 1.0f, 1.0f, 0.0f);
	matrix.columns[3] = Vec4(0.0f, 0.0f, 1.0f, 1.0f);

	Mat4 invTest(1.0f);
	invTest.columns[0] = Vec4(1.0f, 0.0f, 0.0f, 0.0f);
	invTest.columns[1] = Vec4(-1.0f, 1.0f, 0.0f, 0.0f);
	invTest.columns[2] = Vec4(1.0f, -1.0f, 1.0f, 0.0f);
	invTest.columns[3] = Vec4(-1.0f, 1.0f, -1.0f, 1.0f);

	Mat4 inv = Inverse(matrix);

	for (int col = 0; col < 4; ++col)
	{
		for (int row = 0; row < 4; ++row)
		{
			EXPECT_FLOAT_EQ(invTest[row][col], inv[row][col]);
		}
	}

	Mat4 I1 = matrix * inv;
	Mat4 I2 = inv * matrix;

	for (int col = 0; col < 4; ++col)
	{
		for (int row = 0; row < 4; ++row)
		{
			const float expected = (col == row) ? 1.0f : 0.0f;
			EXPECT_FLOAT_EQ(I1[col][row], expected);
		}
	}
}