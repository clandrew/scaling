#pragma once

// A constant buffer that stores the three basic column-major matrices for composing geometry.
cbuffer ModelViewProjectionConstantBuffer : register(b0)
{
	matrix model;
	matrix view;
	matrix projection;
};

struct DrawConstantsType
{
	int samplerIndex;
};
ConstantBuffer<DrawConstantsType> drawConstants : register(b1);

Texture2D g_inputTexture : register(t0);

SamplerState g_samplers[2] : register(s0);
