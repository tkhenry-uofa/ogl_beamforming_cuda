/* See LICENSE for license details. */
#version 460 core
layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;

layout(std430, binding = 1) readonly restrict buffer buffer_1 {
	float rf_data[];
};

layout(std140, binding = 0) uniform parameters {
	uvec4 channel_mapping[64];    /* Transducer Channel to Verasonics Channel */
	uvec4 uforces_channels[32];   /* Channels used for virtual UFORCES elements */
	uvec4 rf_data_dim;            /* Samples * Channels * Acquisitions; last element ignored */
	uvec4 output_points;          /* Width * Height * Depth; last element ignored */
	vec2  output_min_xz;          /* [m] Top left corner of output region */
	vec2  output_max_xz;          /* [m] Bottom right corner of output region */
	vec2  xdc_min_xy;             /* [m] Min center of transducer elements */
	vec2  xdc_max_xy;             /* [m] Max center of transducer elements */
	uint  channel_data_stride;    /* Data points between channels (samples * acq + padding) */
	uint  channel_offset;         /* Offset into channel_mapping: 0 or 128 (rows or columns) */
	float speed_of_sound;         /* [m/s] */
	float sampling_frequency;     /* [Hz]  */
	float focal_depth;            /* [m]   */
};
//layout(location = 6) uniform sampler2D u_element_positions;

layout(rg32f, location = 1) uniform image3D   u_out_data_tex;

#define C_SPLINE 0.5

/* NOTE: See: https://cubic.org/docs/hermite.htm */
float cubic(uint ridx, float x)
{
	mat4 h = mat4(
		 2, -3,  0, 1,
		-2,  3,  0, 0,
		 1, -2,  1, 0,
		 1, -1,  0, 0
	);

	uint  xk = uint(floor(x));
	float t  = (x  - float(xk));
	vec4  S  = vec4(t * t * t, t * t, t, 1);

	float P1 = rf_data[ridx + xk];
	float P2 = rf_data[ridx + xk + 1];
	float T1 = C_SPLINE * (P2 - rf_data[ridx + xk - 1]);
	float T2 = C_SPLINE * (rf_data[ridx + xk + 2] - P1);
	vec4  C  = vec4(P1, P2, T1, T2);
	return dot(S, h * C);
}

void main()
{
	vec2  pixel     = vec2(gl_GlobalInvocationID.xy);
	ivec3 out_coord = ivec3(gl_GlobalInvocationID.xyz);

	ivec3 out_data_dim = imageSize(u_out_data_tex);

	/* NOTE: Convert pixel to physical coordinates */
	vec2 xdc_size    = abs(xdc_max_xy - xdc_min_xy);
	vec2 output_size = abs(output_max_xz - output_min_xz);

	/* TODO: for now assume y-dimension is along transducer center */
	vec3 image_point = vec3(
		output_min_xz.x + pixel.x * output_size.x / out_data_dim.x,
		0,
		output_min_xz.y + pixel.y * output_size.y / out_data_dim.y
	);

	float x      = image_point.x - xdc_min_xy.x;
	float dx     = xdc_size.x / float(rf_data_dim.y);
	float dzsign = sign(image_point.z - focal_depth);

	float sum = 0;

	uint ridx = rf_data_dim.y * rf_data_dim.x;
	/* NOTE: skip first acquisition since its garbage */
	for (uint i = 1; i < rf_data_dim.z; i++) {
		uint base_idx = (i - 1) / 4;
		uint sub_idx  = (i - 1) - base_idx;

		vec3  focal_point   = vec3(uforces_channels[base_idx][sub_idx] * dx, 0, focal_depth);
		float transmit_dist = focal_depth + dzsign * distance(image_point, focal_point);

		vec2 rdist = vec2(x, image_point.z);
		for (uint j = 0; j < rf_data_dim.y; j++) {
			float dist    = transmit_dist + length(rdist);
			float rsample = dist * sampling_frequency / speed_of_sound;

			/* NOTE: do cubic interp between adjacent time samples */
			sum     += cubic(ridx, rsample);
			rdist.x -= dx;
			ridx    += rf_data_dim.x;
		}
		ridx += rf_data_dim.y * rf_data_dim.x;
	}
	imageStore(u_out_data_tex, out_coord, vec4(sum, sum, 0, 0));
}
