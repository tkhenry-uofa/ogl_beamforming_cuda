/* See LICENSE for license details. */
#version 460 core
layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;

layout(std430, binding = 1) readonly restrict buffer buffer_1 {
	vec2 in_data[];
};

layout(std430, binding = 2) writeonly restrict buffer buffer_2 {
	vec2 out_data[];
};

layout(std140, binding = 0) uniform parameters {
	uvec4 channel_mapping[64];    /* Transducer Channel to Verasonics Channel */
	uvec4 uforces_channels[32];   /* Channels used for virtual UFORCES elements */
	vec4  lpf_coefficients[16];   /* Low Pass Filter Cofficients */
	vec4  xdc_origin[4];          /* [m] Corner of transducer being treated as origin */
	vec4  xdc_corner1[4];         /* [m] Corner of transducer along first axis (arbitrary) */
	vec4  xdc_corner2[4];         /* [m] Corner of transducer along second axis (arbitrary) */
	uvec4 dec_data_dim;           /* Samples * Channels * Acquisitions; last element ignored */
	uvec4 output_points;          /* Width * Height * Depth * (Frame Average Count) */
	vec4  output_min_coord;       /* [m] Top left corner of output region */
	vec4  output_max_coord;       /* [m] Bottom right corner of output region */
	uvec2 rf_raw_dim;             /* Raw Data Dimensions */
	uint  array_count;            /* Number of Arrays (4 max) */
	uint  channel_offset;         /* Offset into channel_mapping: 0 or 128 (rows or columns) */
	uint  lpf_order;              /* Order of Low Pass Filter */
	float speed_of_sound;         /* [m/s] */
	float sampling_frequency;     /* [Hz]  */
	float center_frequency;       /* [Hz]  */
	float focal_depth;            /* [m]   */
	float time_offset;            /* pulse length correction time [s]   */
	uint  uforces;                /* mode is UFORCES (1) or FORCES (0) */
	float off_axis_pos;           /* [m] Position on screen normal to beamform in 2D HERCULES */
	int   beamform_plane;         /* Plane to Beamform in 2D HERCULES */
};

void main()
{
	uint time_sample = gl_GlobalInvocationID.x;
	uint channel     = gl_GlobalInvocationID.y;
	uint acq         = gl_GlobalInvocationID.z;

	/* NOTE: offsets for storing the results in the output data */
	uint stride = dec_data_dim.x * dec_data_dim.y;
	uint off    = dec_data_dim.x * channel + stride * acq + time_sample;

	/* NOTE: for calculating full-band I-Q data; needs to be stepped in loop */
	float arg       = radians(360) * center_frequency * time_sample / sampling_frequency;
	float arg_delta = radians(360) * center_frequency / sampling_frequency;

	vec2 sum = vec2(0);
	for (int i = 0; i <= lpf_order; i++) {
		vec2 data;
		/* NOTE: make sure data samples come from the same acquisition */
		if (time_sample >= i) data = in_data[off - i].xx * vec2(cos(arg), sin(arg));
		else                  data = vec2(0);
		sum += lpf_coefficients[i / 4][i % 4] * data;
		arg -= arg_delta;
	}
	out_data[off] = sum;
}
