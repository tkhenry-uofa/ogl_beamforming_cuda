/* See LICENSE for license details. */

/* NOTE: Does a binary search in 3D for smallest and largest output values */
layout(local_size_x = 32, local_size_y = 1, local_size_z = 32) in;

layout(rg32f, binding = 0) readonly  uniform image3D u_out_data_tex;
layout(rg32f, binding = 1) writeonly uniform image3D u_mip_view_tex;

void main()
{
	ivec3 out_coord = ivec3(gl_GlobalInvocationID.xyz);

	ivec3 idx = out_coord * 2;
	vec2 min_max = vec2(1000000000, 0);
	for (int i = 0; i < 2; i++) {
		for (int j = 0; j < 2; j++) {
			vec2 a    = imageLoad(u_out_data_tex, idx + ivec3(i, j, 0)).xy;
			vec2 b    = imageLoad(u_out_data_tex, idx + ivec3(i, j, 1)).xy;
			min_max.x = min(min_max.x, min(a.x, b.x));
			min_max.y = max(min_max.y, max(a.y, b.y));
		}
	}
	imageStore(u_mip_view_tex, out_coord, vec4(min_max, 0, 1));
}
