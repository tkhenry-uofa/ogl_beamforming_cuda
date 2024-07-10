/* See LICENSE for license details. */
#version 460 core
layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;

layout(std430, binding = 1) readonly restrict buffer buffer_1 {
	int rf_data[];
};

layout(std430, binding = 2) writeonly restrict buffer buffer_2 {
	float out_data[];
};

layout(std430, binding = 3) readonly restrict buffer buffer_3 {
	int hadamard[];
};

layout(std140, binding = 0) uniform parameters {
	uvec4 channel_mapping[64];    /* Transducer Channel to Verasonics Channel */
	uvec4 uforces_channels[32];   /* Channels used for virtual UFORCES elements */
	uvec4 rf_data_dim;            /* Samples * Channels * Acquisitions; last element ignored */
	uvec4 output_points;          /* Width * Height * Depth; last element ignored */
	vec2  output_min_xz;          /* [m] Top left corner of output region */
	vec2  output_max_xz;          /* [m] Bottom right corner of output region */
	uint  channel_data_stride;    /* Data points between channels (samples * acq + padding) */
	uint  channel_offset;         /* Offset into channel_mapping: 0 or 128 (rows or columns) */
	float speed_of_sound;         /* [m/s] */
	float sampling_frequency;     /* [Hz]  */
	float focal_depth;            /* [m]   */
};

void main()
{
	/* NOTE: each invocation takes a time sample (row) and a receive channel (column).
	 * it does the dot product of that column with the equivalent row of the hadamard matrix.
	 * the result is stored to the same row, column index of the output data.
	 */
	uint time_sample = gl_GlobalInvocationID.x;
	uint channel     = gl_GlobalInvocationID.y;
	uint acq         = gl_GlobalInvocationID.z;

	/* offset to get the correct column in hadamard matrix */
	uint hoff = rf_data_dim.z * acq;

	/* TODO: make sure incoming data is organized so that stride is 1
	 * i.e. each column should be a single time sample for all channels
	 * alternatively we can tell opengl to store the rf data in row major order
	 */

	/* offset to get the time sample and row in rf data */
	uint rstride = rf_data_dim.x * rf_data_dim.y;
	uint rfoff   = rf_data_dim.x * channel + time_sample;

	uint ridx       = rfoff / 2;
	uint ridx_delta = rstride / 2;

	/* NOTE: rf_data is i16 so each access grabs two time samples at time.
	 * We need to shift arithmetically (maintaining the sign) to get the
	 * desired element. If the time sample is even we take the upper half
	 * and if its odd we take the lower half. */
	uint lfs = ~(time_sample & 1) * 16;

	/* NOTE: Compute N-D dot product */
	int sum = 0;
	for (int i = 0; i < rf_data_dim.z; i++) {
		int data = (rf_data[ridx] << lfs) >> 16;
		sum += hadamard[hoff + i] * data;
		ridx += ridx_delta;
	}

	out_data[rfoff + rstride * acq] = float(sum);
}
