#include "Bindings.hlsli"

struct PSInput
{
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD;
};

float4 main(PSInput input) : SV_TARGET
{
	float2 uv = input.uv;
	if (drawConstants.samplerIndex == 0)
	{
		return g_inputTexture.Sample(g_sampler_point, uv);
	}
	else
	{
		return g_inputTexture.Sample(g_sampler_linear, uv);
	}
}