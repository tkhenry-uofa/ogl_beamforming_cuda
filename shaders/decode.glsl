/* See LICENSE for license details. */

/* NOTE(rnp): invoked with samples x channels x transmits
 * Each instance extracts a single time sample from a single channel for all transmits
 * and does a dot product with the appropriate row of the bound hadamard matrix
 * (unless decode_mode == DECODE_MODE_NONE). The result of this dot product is stored in the
 * output. In bulk this has the effect of computing a matrix multiply of the
 * sample-transmit plane with the bound hadamard matrix.
 */

#if   defined(INPUT_DATA_TYPE_FLOAT)
	#define INPUT_DATA_TYPE      float
	#define RF_SAMPLES_PER_INDEX 1
	#define RESULT_TYPE_CAST(x)  vec2(x, 0)
#elif defined(INPUT_DATA_TYPE_FLOAT_COMPLEX)
	#define INPUT_DATA_TYPE      vec2
	#define RF_SAMPLES_PER_INDEX 1
	#define RESULT_TYPE_CAST(x)  (x)
#else
	#define INPUT_DATA_TYPE      int
	#define RF_SAMPLES_PER_INDEX 2
	#define RESULT_TYPE_CAST(x)  vec2(x, 0)
#endif

layout(std430, binding = 1) readonly restrict buffer buffer_1 {
	INPUT_DATA_TYPE rf_data[];
};

layout(std430, binding = 2) writeonly restrict buffer buffer_2 {
	vec2 out_data[];
};

layout(r8i,  binding = 0) readonly restrict uniform iimage2D hadamard;
layout(r16i, binding = 1) readonly restrict uniform iimage1D channel_mapping;

INPUT_DATA_TYPE sample_rf_data(int index, uint lfs)
{
	INPUT_DATA_TYPE result;
#if defined(INPUT_DATA_TYPE_FLOAT) || defined(INPUT_DATA_TYPE_FLOAT_COMPLEX)
	result = rf_data[index];
#else
	/* NOTE(rnp): for i16 rf_data we grab 2 samples at a time. We need to shift
	 * arithmetically (maintaining the sign) to get the desired element. */
	result = (rf_data[index] << lfs) >> 16;
#endif
	return result;
}

void main()
{
	int time_sample = int(gl_GlobalInvocationID.x);
	int channel     = int(gl_GlobalInvocationID.y);
	int transmit    = int(gl_GlobalInvocationID.z);

	/* NOTE(rnp): stores output as a 3D matrix with ordering of {samples, channels, transmits} */
	uint out_off = dec_data_dim.x * dec_data_dim.y * transmit + dec_data_dim.x * channel + time_sample;

	int rf_channel = imageLoad(channel_mapping, channel).x;

	/* NOTE(rnp): samples input as 2D matrix of {samples * transmits + padding, channels} */
	int rf_stride = int(dec_data_dim.x) / RF_SAMPLES_PER_INDEX;
	int rf_offset = (int(rf_raw_dim.x) * rf_channel + time_sample) / RF_SAMPLES_PER_INDEX;

	uint lfs = ((~time_sample) & 1u) * 16;
	vec2 result = vec2(0);
	switch (decode) {
	case DECODE_MODE_NONE: {
		result = RESULT_TYPE_CAST(sample_rf_data(rf_offset + rf_stride * transmit, lfs));
	} break;
	case DECODE_MODE_HADAMARD: {
		INPUT_DATA_TYPE sum = INPUT_DATA_TYPE(0);
		for (int i = 0; i < dec_data_dim.z; i++) {
			sum += imageLoad(hadamard, ivec2(i, transmit)).x * sample_rf_data(rf_offset, lfs);
			rf_offset += rf_stride;
		}
		result = RESULT_TYPE_CAST(sum) / float(dec_data_dim.z);
	} break;
	}
	out_data[out_off] = result;
}
