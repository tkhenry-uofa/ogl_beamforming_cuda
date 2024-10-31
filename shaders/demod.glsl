/* See LICENSE for license details. */
layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;

layout(std430, binding = 1) readonly restrict buffer buffer_1 {
	vec2 in_data[];
};

layout(std430, binding = 2) writeonly restrict buffer buffer_2 {
	vec2 out_data[];
};

layout(std430, binding = 3) readonly restrict buffer buffer_3 {
	float filter_coefficients[];
};

layout(location = 0) uniform uint u_filter_order = 0;

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
	for (int i = 0; i <= u_filter_order; i++) {
		vec2 data;
		/* NOTE: make sure data samples come from the same acquisition */
		if (time_sample >= i) data = in_data[off - i].xx * vec2(cos(arg), sin(arg));
		else                  data = vec2(0);
		sum += filter_coefficients[i] * data;
		arg -= arg_delta;
	}
	out_data[off] = sum;
}
