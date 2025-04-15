/* See LICENSE for license details. */
layout(binding = 0) uniform sampler3D u_out_data_tex;

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
	ivec2 out_data_dim = textureSize(u_out_data_tex, 0).xz;

	//vec2 min_max = texelFetch(u_out_data_tex, ivec3(0), textureQueryLevels(u_out_data_tex) - 1).xy;

	/* TODO(rnp): select between x and y and specify slice */
	ivec2 coord     = ivec2(fragment_texture_coordinate * vec2(out_data_dim));
	ivec3 smp_coord = ivec3(coord.x, 0, coord.y);
	float smp       = length(texelFetch(u_out_data_tex, smp_coord, 0).xy);

	float threshold_val = pow(10.0f, u_threshold / 20.0f);
	smp = clamp(smp, 0.0f, threshold_val);
	smp = smp / threshold_val;
	smp = pow(smp, u_gamma);

	if (u_log_scale) {
		smp = 20 * log(smp) / log(10);
		smp = clamp(smp, -u_db_cutoff, 0) / -u_db_cutoff;
		smp = 1 - smp;
	}

	//v_out_colour = vec4(hsv2rgb(vec3(360 * smp, 0.8, 0.95)), 1);
	v_out_colour = vec4(smp, smp, smp, 1);
}
