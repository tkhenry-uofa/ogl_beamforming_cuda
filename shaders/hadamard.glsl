/* See LICENSE for license details. */
layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;

layout(std430, binding = 1) readonly restrict buffer buffer_1 {
	int rf_data[];
};

layout(std430, binding = 2) writeonly restrict buffer buffer_2 {
	vec2 out_data[];
};

layout(r8i, binding = 0) readonly restrict uniform iimage2D hadamard;

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

	/* NOTE: offsets for storing the results in the output data */
	uint out_off = dec_data_dim.x * dec_data_dim.y * acq + dec_data_dim.x * channel + time_sample;

	channel += channel_offset;
	/* NOTE: channel mapping is stored as u16s so we must do this to extract the final value */
	uint ch_array_idx = (channel / 8);
	uint ch_vec_idx   = (channel % 8) / 2;
	uint ch_elem_lfs  = ((~channel) & 1u) * 16;
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
	if (decode == 1) {
		/* NOTE: offset to get the correct column in hadamard matrix */
		uint hoff = dec_data_dim.z * acq;

		for (int i = 0; i < dec_data_dim.z; i++) {
			int data = (rf_data[ridx] << lfs) >> 16;
			sum  += imageLoad(hadamard, ivec2(acq, i)).x * data;
			ridx += ridx_delta;
		}
	} else {
		ridx += ridx_delta * acq;
		sum   = (rf_data[ridx] << lfs) >> 16;
	}
	out_data[out_off] = vec2(float(sum), 0);
}
