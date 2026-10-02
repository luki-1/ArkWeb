// Spider-Man's frame -> (r, g, b, distance in meters) where something is nearer than g_range, else 0.
// In sky mode only the hero and his webs are that near (New York is a kilometer below). The output is
// the 3D view only: a window of another shape shows it letterboxed, at (g_colX0, g_colY0).
Texture2D<float4>   g_color : register(t0);
ByteAddressBuffer   g_depth : register(t1);
RWTexture2D<float4> g_out : register(u0);

cbuffer Params : register(b0)
{
	uint  g_outW, g_outH;    // output = the 3D view's rectangle in the back buffer
	uint  g_depW, g_depH;    // depth buffer size (the render resolution)
	uint  g_depPitch;        // bytes per depth row (copy footprint)
	float g_near;            // projection near plane, meters
	float g_range;           // keep what is nearer than this, meters
	uint  g_reversed;        // 1: reversed Z (1 at the near plane, 0 at infinity)
	uint  g_colX0, g_colY0;  // where the 3D view starts in the back buffer
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= g_outW || id.y >= g_outH) return;
	uint  dx = min(g_depW - 1, (uint)((id.x + 0.5) * g_depW / g_outW));
	uint  dy = min(g_depH - 1, (uint)((id.y + 0.5) * g_depH / g_outH));
	float z = asfloat(g_depth.Load(dy * g_depPitch + dx * 4));
	float d = g_reversed ? (z > 0.0 ? g_near / z : 1e30) : (z < 1.0 ? g_near / (1.0 - z) : 1e30);
	float4 c = g_color[uint2(id.x + g_colX0, id.y + g_colY0)];
	g_out[id.xy] = d < g_range ? float4(c.rgb, d) : float4(0, 0, 0, 0);
}
