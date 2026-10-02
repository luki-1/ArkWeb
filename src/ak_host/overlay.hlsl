// Spider-Man's captured frame over Arkham's: a full-screen triangle that looks each pixel up in the
// capture (same camera, maybe another field of view) and keeps only what the capture holds. The four
// texels around the spot are blended bilinearly, counting only those that hold something: smooth
// scaling, soft edges (coverage is the alpha), no dark fringe from the empty texels around him.
// The same pass draws the zip-to-point marker: a ring on the grapple ledge L2 + R2 would launch to.
Texture2D<float4> g_cap : register(t0);
SamplerState      g_point : register(s0);

cbuffer Params : register(b0)
{
	float2 g_scale;   // Arkham uv -> capture uv about the centre: tan(Arkham half FOV) / tan(Spider-Man half FOV)
	float2 g_offset;  // capture uv shift
	float4 g_tint;    // rgb multiplier (Spider-Man's lighting vs Gotham's night), a = gamma (< 1 lifts his shadows)
	float4 g_marker;  // xy: marker centre (Arkham uv), z: ring radius (pixels), w: 1 = draw it
	float4 g_markerColor;
	float4 g_screen;  // xy: Arkham back buffer size (pixels), z: ring width (pixels)
};

void vs(uint a_id : SV_VertexID, out float4 o_pos : SV_Position, out float2 o_uv : TEXCOORD0)
{
	o_uv = float2((a_id << 1) & 2, a_id & 2);
	o_pos = float4(o_uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float MarkerAlpha(float2 a_uv)
{
	if (g_marker.w <= 0.0) return 0.0;
	float d = length((a_uv - g_marker.xy) * g_screen.xy);
	float r = g_marker.z, w = g_screen.z;
	float ring = saturate(1.0 - abs(d - r) / w);      // the ring
	float dot_ = saturate(1.0 - (d - w * 0.6) / w);   // and a dot in the middle
	return max(ring, dot_);
}

float4 ps(float4 a_pos : SV_Position, float2 a_uv : TEXCOORD0) : SV_Target
{
	float  ma = MarkerAlpha(a_uv) * g_markerColor.a;
	float2 uv = 0.5 + (a_uv - 0.5) * g_scale + g_offset;
	float4 sp = 0.0;
	if (all(uv >= 0.0) && all(uv <= 1.0)) {
		float2 dim;
		g_cap.GetDimensions(dim.x, dim.y);
		float2 f = frac(uv * dim - 0.5);
		// Gather order: (u0,v1) (u1,v1) (u1,v0) (u0,v0)
		float4 w = float4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));
		float4 a = g_cap.GatherAlpha(g_point, uv);  // distance; 0 = nothing near Spider-Man's camera there
		float4 m = w * (a > 0.0);
		float  cov = dot(m, 1.0);
		if (cov > 0.002) {
			float3 c = float3(dot(m, g_cap.GatherRed(g_point, uv)), dot(m, g_cap.GatherGreen(g_point, uv)), dot(m, g_cap.GatherBlue(g_point, uv))) / cov;
			c = pow(saturate(c), g_tint.a) * g_tint.rgb;
			sp = float4(c, saturate(cov));
		}
	}
	if (sp.a <= 0.0 && ma <= 0.0) discard;
	// Spider-Man in front of the marker (straight alpha, blended over Arkham's frame)
	float  outA = sp.a + ma * (1.0 - sp.a);
	float3 outC = (sp.rgb * sp.a + g_markerColor.rgb * ma * (1.0 - sp.a)) / max(outA, 1e-4);
	return float4(outC, outA);
}
