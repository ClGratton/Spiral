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
    float2 uv = saturate(input.UV);
    float3 shaded = input.Color * 0.86f + float3(0.05f, 0.06f, 0.07f);
    float2 cell = min(floor(uv * 4.0f), 3.0f);
    float checker = fmod(cell.x + cell.y, 2.0f);
    shaded *= lerp(0.48f, 1.0f, checker);

    // A procedural face frame makes the prototype read as a deliberate
    // material demo while keeping the portable constant-buffer-only layout.
    float2 edge = min(uv, 1.0f - uv);
    float edgeDistance = min(edge.x, edge.y);
    float edgeAA = max(fwidth(edgeDistance), 0.001f);
    float outerFrame = 1.0f - smoothstep(0.035f - edgeAA, 0.075f + edgeAA, edgeDistance);
    float innerFrame = 1.0f - smoothstep(
        0.012f - edgeAA,
        0.012f + edgeAA,
        abs(edgeDistance - 0.135f));

    float2 cornerVector = min(uv, 1.0f - uv);
    float cornerDistance = length(cornerVector);
    float cornerGlow = 1.0f - smoothstep(0.08f, 0.24f, cornerDistance);
    float3 accent = lerp(input.Color, float3(0.82f, 0.94f, 1.0f), 0.56f);
    shaded = lerp(shaded, accent * 1.18f, outerFrame * 0.88f);
    shaded += accent * (innerFrame * 0.16f + cornerGlow * 0.24f);

    float portableBindingProbe = 1.0f + abs(ViewProjection[0][0]) * 0.0000001f;
    return float4(saturate(shaded) * portableBindingProbe, 1.0f);
}
