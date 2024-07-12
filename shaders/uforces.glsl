/* See LICENSE for license details. */
#version 460 core
layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;

layout(std430, binding = 1) readonly restrict buffer buffer_1 {
	vec2 rf_data[];
};

layout(std140, binding = 0) uniform parameters {
	uvec4 channel_mapping[64];    /* Transducer Channel to Verasonics Channel */
	uvec4 uforces_channels[32];   /* Channels used for virtual UFORCES elements */
	vec4  lpf_coefficients[16];   /* Low Pass Filter Cofficients */
	uvec4 rf_data_dim;            /* Samples * Channels * Acquisitions; last element ignored */
	uvec4 output_points;          /* Width * Height * Depth; last element ignored */
	vec2  output_min_xz;          /* [m] Top left corner of output region */
	vec2  output_max_xz;          /* [m] Bottom right corner of output region */
	vec2  xdc_min_xy;             /* [m] Min center of transducer elements */
	vec2  xdc_max_xy;             /* [m] Max center of transducer elements */
	uint  channel_data_stride;    /* Data points between channels (samples * acq + padding) */
	uint  channel_offset;         /* Offset into channel_mapping: 0 or 128 (rows or columns) */
	uint  lpf_order;              /* Order of Low Pass Filter */
	float speed_of_sound;         /* [m/s] */
	float sampling_frequency;     /* [Hz]  */
	float center_frequency;       /* [Hz]  */
	float focal_depth;            /* [m]   */
};

layout(rg32f, location = 1) uniform image3D   u_out_data_tex;

#define C_SPLINE 0.5

/* NOTE: See: https://cubic.org/docs/hermite.htm */
vec2 cubic(uint ridx, float x)
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

	vec2 P1 = rf_data[ridx + xk];
	vec2 P2 = rf_data[ridx + xk + 1];
	vec2 T1 = C_SPLINE * (P2 - rf_data[ridx + xk - 1]);
	vec2 T2 = C_SPLINE * (rf_data[ridx + xk + 2] - P1);

	vec4 C1 = vec4(P1.x, P2.x, T1.x, T2.x);
	vec4 C2 = vec4(P1.y, P2.y, T1.y, T2.y);
	return vec2(dot(S, h * C1), dot(S, h * C2));
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

	vec2 sum = vec2(0);

	float time_scale = radians(360) * center_frequency;

	uint ridx = rf_data_dim.y * rf_data_dim.x;
	/* NOTE: skip first acquisition since its garbage */
	for (uint i = 1; i < rf_data_dim.z; i++) {
		uint base_idx = (i - 1) / 4;
		uint sub_idx  = (i - 1) - base_idx * 4;

		vec3  focal_point   = vec3(uforces_channels[base_idx][sub_idx] * dx, 0, focal_depth);
		float transmit_dist = focal_depth + dzsign * distance(image_point, focal_point);

		vec2 rdist = vec2(x, image_point.z);
		for (uint j = 0; j < rf_data_dim.y; j++) {
			float dist = transmit_dist + length(rdist);
			float time = dist / speed_of_sound;

			/* NOTE: do cubic interp between adjacent time samples */
			vec2 p   = cubic(ridx, time * sampling_frequency);
			p.x     *= cos(time_scale * time);
			p.y     *= sin(time_scale * time);
			sum     += p;
			rdist.x -= dx;
			ridx    += rf_data_dim.x;
		}
		ridx += rf_data_dim.y * rf_data_dim.x;
	}
	float val = length(sum);
	imageStore(u_out_data_tex, out_coord, vec4(val, val, 0, 0));
}
