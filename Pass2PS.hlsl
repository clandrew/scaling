Texture2D g_inputTexture : register(t0);
SamplerState g_sampler : register(s0);

struct PSInput
{
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD;
};

float4 main(PSInput input) : SV_TARGET
{
	float2 uv = input.uv;
	return g_inputTexture.Sample(g_sampler, uv);
}