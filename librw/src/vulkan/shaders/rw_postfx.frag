#version 450
#extension GL_EXT_multiview : require

// A sampled array is required because FXAA follows edges into neighbouring
// texels. gl_ViewIndex selects the matching eye in the multiview image.
layout(set = 0, binding = 0) uniform sampler2DArray sceneColour;

layout(push_constant) uniform PostFxConstants
{
	vec4 blurColour;
	uvec4 mode;
} postFx;

layout(location = 0) out vec4 outColour;

vec2 ImageSize()
{
	return max(vec2(postFx.mode.zw), vec2(1.0));
}

vec3 SampleColour(vec2 uv)
{
	vec2 halfTexel = 0.5 / ImageSize();
	return texture(sceneColour,
	               vec3(clamp(uv, halfTexel, vec2(1.0) - halfTexel),
	                    float(gl_ViewIndex))).rgb;
}

float Luma(vec3 colour)
{
	return dot(colour, vec3(0.299, 0.587, 0.114));
}

vec3 ApplyFxaa(vec2 uv, vec3 middle)
{
	vec2 texel = 1.0 / ImageSize();
	vec3 nw = SampleColour(uv + vec2(-1.0, -1.0) * texel);
	vec3 ne = SampleColour(uv + vec2( 1.0, -1.0) * texel);
	vec3 sw = SampleColour(uv + vec2(-1.0,  1.0) * texel);
	vec3 se = SampleColour(uv + vec2( 1.0,  1.0) * texel);
	float lm = Luma(middle);
	float lnw = Luma(nw);
	float lne = Luma(ne);
	float lsw = Luma(sw);
	float lse = Luma(se);
	float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));
	float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));

	// Most of a Vice City frame is low contrast. Avoid the four directional
	// samples there, saving bandwidth on the Quest tiler.
	if(lmax - lmin < max(0.0312, lmax * 0.125))
		return middle;

	vec2 direction = vec2(-((lnw + lne) - (lsw + lse)),
	                       ((lnw + lsw) - (lne + lse)));
	float reduce = max((lnw + lne + lsw + lse) * 0.03125,
	                   0.0078125);
	direction = clamp(direction /
	                  (min(abs(direction.x), abs(direction.y)) + reduce),
	                  vec2(-8.0), vec2(8.0)) * texel;
	vec3 a = 0.5 * (
		SampleColour(uv + direction * (1.0 / 3.0 - 0.5)) +
		SampleColour(uv + direction * (2.0 / 3.0 - 0.5)));
	vec3 b = a * 0.5 + 0.25 * (
		SampleColour(uv + direction * -0.5) +
		SampleColour(uv + direction *  0.5));
	float lb = Luma(b);
	return (lb < lmin || lb > lmax) ? a : b;
}

void main()
{
	if(postFx.mode.x == 2u){
		outColour = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	vec2 uv = gl_FragCoord.xy / ImageSize();
	vec4 source = vec4(SampleColour(uv), 1.0);
	if(postFx.mode.y != 0u)
		source.rgb = ApplyFxaa(uv, source.rgb);
	if(postFx.mode.x != 1u){
		outColour = vec4(source.rgb, 1.0);
		return;
	}

	// Exact POSTFX_NORMAL colour filter used by the desktop Vice City path.
	float alpha = postFx.blurColour.a;
	vec4 doubled = clamp(postFx.blurColour * 2.0, 0.0, 1.0);
	vec4 destination = source;
	vec4 previous = destination;
	for(int i = 0; i < 5; i++){
		vec4 filtered = destination * (1.0 - alpha) +
		                previous * doubled * alpha;
		filtered += previous * postFx.blurColour;
		filtered += previous * postFx.blurColour;
		previous = clamp(filtered, 0.0, 1.0);
	}
	outColour = vec4(previous.rgb, 1.0);
}
