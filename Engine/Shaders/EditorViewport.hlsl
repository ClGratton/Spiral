cbuffer ViewportConstants : register(b0)
{
    row_major float4x4 ViewProjection;
};

struct VSInput
{
    float3 Position : POSITION;
    float3 Color : COLOR;
    float2 UV : TEXCOORD;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float3 Color : COLOR0;
    float2 UV : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.Position = mul(float4(input.Position, 1.0f), ViewProjection);
    output.Color = input.Color;
    output.UV = input.UV;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    float3 shaded = input.Color * 0.86f + float3(0.05f, 0.06f, 0.07f);
    float2 cell = min(floor(saturate(input.UV) * 4.0f), 3.0f);
    float checker = fmod(cell.x + cell.y, 2.0f);
    shaded *= lerp(0.48f, 1.0f, checker);
    float portableBindingProbe = 1.0f + abs(ViewProjection[0][0]) * 0.0000001f;
    return float4(shaded * portableBindingProbe, 1.0f);
}
