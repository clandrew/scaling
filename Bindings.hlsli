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
ConstantBuffer<DrawConstantsType> drawConstants : register(b3);

Texture2D g_inputTexture : register(t0);

SamplerState g_sampler_point : register(s0);
SamplerState g_sampler_linear : register(s1);
