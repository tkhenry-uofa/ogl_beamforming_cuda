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
	vec4  xdc_origin[4];          /* [m] Corner of transducer being treated as origin */
	vec4  xdc_corner1[4];         /* [m] Corner of transducer along first axis (arbitrary) */
	vec4  xdc_corner2[4];         /* [m] Corner of transducer along second axis (arbitrary) */
	uvec4 dec_data_dim;           /* Samples * Channels * Acquisitions; last element ignored */
	uvec4 output_points;          /* Width * Height * Depth * (Frame Average Count) */
	vec4  output_min_coord;       /* [m] Top left corner of output region */
	vec4  output_max_coord;       /* [m] Bottom right corner of output region */
	uvec2 rf_raw_dim;             /* Raw Data Dimensions */
	uint  xdc_count;              /* Number of Transducer Arrays (4 max) */
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

	/* NOTE: channel mapping is stored as u16s so we must do this to extract the final value */
	uint ch_array_idx = ((channel + channel_offset) / 8);
	uint ch_vec_idx   = ((channel + channel_offset) % 8) / 2;
	uint ch_elem_lfs  = ((channel + channel_offset) & 1u) * 16;
	uint rf_channel   = (channel_mapping[ch_array_idx][ch_vec_idx] << ch_elem_lfs) >> 16;

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
