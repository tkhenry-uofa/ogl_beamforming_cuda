#version 460 core
layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;

layout(std430, binding = 1) readonly restrict buffer buffer_1 {
	float rf_data[];
};

layout(std430, binding = 2) writeonly restrict buffer buffer_2 {
	float out_data[];
};

layout(location = 3)  uniform uvec3     u_rf_data_dim;
layout(location = 4)  uniform uvec3     u_out_data_dim;
layout(location = 5)  uniform float     u_sound_speed = 1452;
layout(location = 6)  uniform float     u_sampling_frequency = 2.0833e7;
layout(location = 7)  uniform float     u_focal_depth = 0.07;
//layout(location = 10) uniform sampler2D u_element_positions;

void main()
{
	vec3  scale     = vec3(u_rf_data_dim) / vec3(u_out_data_dim);
	vec2  pixel     = vec2(gl_GlobalInvocationID.xy);
	ivec3 rf_coord  = ivec3(gl_GlobalInvocationID.xyz * scale);
	ivec3 out_coord = ivec3(gl_GlobalInvocationID.xyz);

	/* NOTE: Convert pixel to physical coordinates */
	/* TODO: Send these in like the 3D program */

	vec2 xdc_upper_left   = vec2(-0.0096, -0.0096);
	vec2 xdc_bottom_right = vec2( 0.0096,  0.0096);
	//vec2 xdc_upper_left   = texture(u_element_positions, ivec2(0, 0)).xy;
	//vec2 xdc_bottom_right = texture(u_element_positions, ivec2(1, 1)).xy;
	vec2 xdc_size         = abs(xdc_upper_left - xdc_bottom_right);

	/* TODO: for now assume y-dimension is along transducer center */
	vec3 image_point = vec3(
		xdc_upper_left.x + pixel.x * xdc_size.x / u_out_data_dim.x,
		0,
		pixel.y * 80e-3 / u_out_data_dim.y
	);

	float dx     = xdc_size.x / float(u_rf_data_dim.y);
	float dzsign = sign(image_point.z - u_focal_depth);

	/* TODO: Send this into the GPU */
	float sparse_elems[] = {17, 33, 49, 65, 80, 96, 112};

	float sum = 0;
	float x   = image_point.x - xdc_upper_left.x;
	/* NOTE: skip first acquisition since its garbage */
	uint ridx = u_rf_data_dim.y * u_rf_data_dim.x;
	for (uint i = 1; i < u_rf_data_dim.z; i++) {
		vec3 focal_point    = vec3(sparse_elems[i - 1] * dx, 0, u_focal_depth);
		float transmit_dist = u_focal_depth + dzsign * distance(image_point, focal_point);

		vec2 rdist = vec2(x, image_point.z);
		for (uint j = 0; j < u_rf_data_dim.y; j++) {
			float dist = transmit_dist + length(rdist);
			uint rx = uint(dist * u_sampling_frequency / u_sound_speed);

			sum     += rf_data[ridx + rx];
			rdist.x -= dx;
			ridx    += u_rf_data_dim.x;
		}
		ridx += u_rf_data_dim.y * u_rf_data_dim.x;
	}
	uint oidx = u_out_data_dim.x * out_coord.y + out_coord.x;
	out_data[oidx] = sum;
}
