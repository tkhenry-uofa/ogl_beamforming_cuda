/* See LICENSE for license details. */
#version 430 core

in  vec2 fragTexCoord;
out vec4 v_out_colour;

layout(location = 1) uniform sampler3D u_out_data_tex;
layout(location = 2) uniform float     u_db_cutoff = -60;

/* input:  h [0,360] | s,v [0, 1] *
 * output: rgb [0,1]              */
vec3 hsv2rgb(vec3 hsv)
{
	vec3 k = mod(vec3(5, 3, 1) + hsv.x / 60, 6);
	k = max(min(min(k, 4 - k), 1), 0);
	return hsv.z - hsv.z * hsv.y * k;
}

void main()
{
	ivec3 out_data_dim = textureSize(u_out_data_tex, 0);
	ivec2 coord  = ivec2(fragTexCoord * out_data_dim.xy);
	vec2 min_max = texelFetch(u_out_data_tex, ivec3(0), textureQueryLevels(u_out_data_tex) - 1).xy;

	float smp    = texelFetch(u_out_data_tex, ivec3(coord.x, coord.y, 0), 0).x;
	float absmax = max(abs(min_max.y), abs(min_max.x));

	smp = 20 * log(abs(smp) / absmax);
	smp = clamp(smp, u_db_cutoff, 0) / u_db_cutoff;
	smp = 1 - smp;

	//v_out_colour = vec4(hsv2rgb(vec3(360 * smp + 120, 0.8, 0.95)), 1);
	v_out_colour = vec4(smp, smp, smp, 1);
}
