// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// small additions to glm's quaternion type
namespace glm
{
	// rescales the vector part so that |q| = 1 while w stays unchanged
	inline quat normalize_xyz(const quat& q)
	{
		const float vecLen = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
		const float scale = std::sqrt(1.0f - q.w * q.w) / vecLen;
		return quat(q.w, q.x * scale, q.y * scale, q.z * scale);
	}

	// columns of the rotation matrix of a unit quaternion
	inline vec3 GetVectorX(const quat& q)
	{
		const float ww = q.w * q.w;
		return vec3(2.0f * (ww + q.x * q.x) - 1.0f, 2.0f * (q.x * q.y - q.w * q.z), 2.0f * (q.x * q.z + q.w * q.y));
	}

	inline vec3 GetVectorY(const quat& q)
	{
		const float ww = q.w * q.w;
		return vec3(2.0f * (q.x * q.y + q.w * q.z), 2.0f * (ww + q.y * q.y) - 1.0f, 2.0f * (q.y * q.z - q.w * q.x));
	}

	inline vec3 GetVectorZ(const quat& q)
	{
		const float ww = q.w * q.w;
		return vec3(2.0f * (q.x * q.z - q.w * q.y), 2.0f * (q.y * q.z + q.w * q.x), 2.0f * (ww + q.z * q.z) - 1.0f);
	}
}
