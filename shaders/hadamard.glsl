/* See LICENSE for license details. */
#version 460 core
layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;

layout(std430, binding = 1) readonly restrict buffer buffer_1 {
	int rf_data[];
};

layout(std430, binding = 2) writeonly restrict buffer buffer_2 {
	vec2 out_data[];
};

layout(std430, binding = 3) readonly restrict buffer buffer_3 {
	int hadamard[];
};

layout(std140, binding = 0) uniform parameters {
	uvec4 channel_mapping[64];    /* Transducer Channel to Verasonics Channel */
	uvec4 uforces_channels[32];   /* Channels used for virtual UFORCES elements */
	vec4  lpf_coefficients[16];   /* Low Pass Filter Cofficients */
	uvec4 dec_data_dim;           /* Samples * Channels * Acquisitions; last element ignored */
	uvec4 output_points;          /* Width * Height * Depth; last element ignored */
	uvec2 rf_raw_dim;             /* Raw Data Dimensions */
	vec2  output_min_xz;          /* [m] Top left corner of output region */
	vec2  output_max_xz;          /* [m] Bottom right corner of output region */
	vec2  xdc_min_xy;             /* [m] Min center of transducer elements */
	vec2  xdc_max_xy;             /* [m] Max center of transducer elements */
	uint  channel_offset;         /* Offset into channel_mapping: 0 or 128 (rows or columns) */
	uint  lpf_order;              /* Order of Low Pass Filter */
	float speed_of_sound;         /* [m/s] */
	float sampling_frequency;     /* [Hz]  */
	float center_frequency;       /* [Hz]  */
	float focal_depth;            /* [m]   */
	float time_offset;            /* pulse length correction time [s]   */
	uint  uforces;                /* mode is UFORCES (1) or FORCES (0) */
	float off_axis_pos;           /* Where on the 3rd axis to render the image (Hercules only) */
};

void main()
{
	/* NOTE: each invocation takes a time sample and a receive channel.
	 * It first maps the column to the correct column in the rf data then
	 * does the dot product with the equivalent row of the hadamard matrix.
	 * The result is stored to the equivalent row, column index of the output.
	 */
	uint time_sample = gl_GlobalInvocationID.x;
	uint channel     = gl_GlobalInvocationID.y;
	uint acq         = gl_GlobalInvocationID.z;

	/* NOTE: offset to get the correct column in hadamard matrix */
	uint hoff = dec_data_dim.z * acq;

	/* NOTE: offsets for storing the results in the output data */
	uint out_off = dec_data_dim.x * dec_data_dim.y * acq + dec_data_dim.x * channel + time_sample;

	uint ch_base_idx = (channel + channel_offset) / 4;
	uint ch_sub_idx  = (channel + channel_offset) - ch_base_idx * 4;
	uint rf_channel  = channel_mapping[ch_base_idx][ch_sub_idx];

	/* NOTE: stride is the number of samples between acquistions; off is the
	 * index of the first acquisition for this channel and time sample  */
	uint rf_stride = dec_data_dim.x;
	uint rf_off    = rf_raw_dim.x * rf_channel + time_sample;

	/* NOTE: rf_data index and stride considering the data is i16 not i32 */
	uint ridx       = rf_off / 2;
	uint ridx_delta = rf_stride / 2;

	/* NOTE: rf_data is i16 so each access grabs two time samples at time.
	 * We need to shift arithmetically (maintaining the sign) to get the
	 * desired element. If the time sample is even we take the upper half
	 * and if its odd we take the lower half. */
	uint lfs = ((~time_sample) & 1u) * 16;

	/* NOTE: Compute N-D dot product */
	int sum = 0;
	for (int i = 0; i < dec_data_dim.z; i++) {
		int data = (rf_data[ridx] << lfs) >> 16;
		sum  += hadamard[hoff + i] * data;
		ridx += ridx_delta;
	}
	out_data[out_off] = vec2(float(sum), 0);
}
