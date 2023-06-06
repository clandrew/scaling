
struct VSOutput
{
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD;
};

VSOutput main( float4 pos : POSITION, float4 uv : TEXCOORD)
{
	VSOutput result;

	result.position = pos;
	result.uv = uv.xy;

	return result;
}