#version 460 core
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(std430, binding = 1) readonly restrict buffer buffer_1 {
	float rf_data[];
};

layout(std430, binding = 2) writeonly restrict buffer buffer_2 {
	float out_data[];
};

layout(location = 3) uniform uvec3 u_rf_data_dim;
layout(location = 4) uniform uvec3 u_out_data_dim;
layout(location = 5) uniform uint  u_acquisition;

uint rf_idx(uint x, uint y, uint z)
{
	return u_rf_data_dim.y * u_rf_data_dim.x * z + u_rf_data_dim.x * y + x;
}

uint out_idx(uint x, uint y, uint z)
{
	return u_out_data_dim.y * u_out_data_dim.x * z + u_out_data_dim.x * y + x;
}

void main()
{
	vec3  scale     = vec3(u_rf_data_dim) / vec3(u_out_data_dim);
	ivec3 rf_coord  = ivec3(gl_GlobalInvocationID.xyz * scale);
	ivec3 out_coord = ivec3(gl_GlobalInvocationID.xyz);

	uint x = rf_coord.x;
	uint y = rf_coord.y;
	uint z = u_acquisition;

	/* TODO: Probably should rotate in the fragment shader */
	uint oidx = out_idx(out_coord.y, out_coord.x, out_coord.z);

	uint ridx = rf_idx(x, y, z);
	out_data[oidx] = rf_data[ridx];
}
