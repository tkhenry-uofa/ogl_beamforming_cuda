/* See LICENSE for license details. */
#version 460 core
layout(local_size_x = 32, local_size_y = 1, local_size_z = 32) in;

layout(std430, binding = 1) readonly restrict buffer buffer_1 {
	vec2 rf_data[];
};

layout(std140, binding = 0) uniform parameters {
	uvec4 channel_mapping[64];    /* Transducer Channel to Verasonics Channel */
	uvec4 uforces_channels[32];   /* Channels used for virtual UFORCES elements */
	vec4  lpf_coefficients[16];   /* Low Pass Filter Cofficients */
	uvec4 dec_data_dim;           /* Samples * Channels * Acquisitions; last element ignored */
	uvec4 output_points;          /* Width * Height * Depth; last element ignored */
	vec4  output_min_coord;       /* [m] Top left corner of output region */
	vec4  output_max_coord;       /* [m] Bottom right corner of output region */
	uvec2 rf_raw_dim;             /* Raw Data Dimensions */
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
	float off_axis_pos;           /* [m] Position on screen normal to beamform in 2D HERCULES */
	int   beamform_plane;         /* Plane to Beamform in 2D HERCULES */
};

layout(rg32f, location = 1) uniform writeonly image3D u_out_data_tex;
layout(r32f,  location = 2) uniform writeonly image3D u_out_volume_tex;

layout(location = 3) uniform int   u_volume_export_pass;
layout(location = 4) uniform ivec3 u_volume_export_dim_offset;

#define C_SPLINE 0.5

#define TX_ROWS 0
#define TX_COLS 1

#if 0
/* NOTE: interpolation is unnecessary if the data has been demodulated and not decimated */
vec2 cubic(uint ridx, float t)
{
	return rf_data[ridx + uint(floor(t))];
}
#else
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
#endif

void main()
{
	vec3  voxel        = vec3(gl_GlobalInvocationID.xyz)  + vec3(u_volume_export_dim_offset);
	ivec3 out_coord    = ivec3(gl_GlobalInvocationID.xyz) + u_volume_export_dim_offset;
	ivec3 out_data_dim;

	if (u_volume_export_pass == 0) out_data_dim = imageSize(u_out_data_tex);
	else                           out_data_dim = imageSize(u_out_volume_tex);

	/* NOTE: Convert pixel to physical coordinates */
	vec2 xdc_size      = abs(xdc_max_xy - xdc_min_xy);
	vec4 output_size   = abs(output_max_coord - output_min_coord);
	vec3 image_point   = output_min_coord.xyz + voxel * output_size.xyz / out_data_dim.xyz;

	if (u_volume_export_pass == 0)
		image_point.y = off_axis_pos;

	/* NOTE: used for constant F# dynamic receive apodization. This is implemented as:
	 *
	 *                  /        |x_e - x_i|\
	 *    a(x, z) = cos(F# * π * ----------- ) ^ 2
	 *                  \        |z_e - z_i|/
	 *
	 * where x,z_e are transducer element positions and x,z_i are image positions. */
	float f_num    = output_size.z / output_size.x;
	float apod_arg = f_num * 0.5 * radians(360) / abs(image_point.z);

	/* NOTE: for I-Q data phase correction */
	float iq_time_scale = (lpf_order > 0)? radians(360) * center_frequency : 0;

	vec3  starting_dist = image_point - vec3(xdc_min_xy.x, xdc_min_xy.y, 0);
	vec3  delta         = vec3(xdc_size.x, xdc_size.y, 0) / vec3(dec_data_dim.y);
	float dzsign        = sign(image_point.z - focal_depth);

	/* NOTE: offset correcting for both pulse length and low pass filtering */
	float time_correction = time_offset + lpf_order / sampling_frequency;

	vec2 sum   = vec2(0);
	vec3 rdist = starting_dist;

	/* TODO: pass this in (there is a problem in that it depends on the orientation
	 * of the array relative to the target/subject). */
	int   transmit_orientation = TX_ROWS;
	float transmit_dist;
	if (isinf(focal_depth)) {
		/* NOTE: plane wave */
		transmit_dist = image_point.z;
	} else {
		/* NOTE: cylindrical diverging wave */
		if (transmit_orientation == TX_ROWS)
			transmit_dist = length(vec2(image_point.y, image_point.z - focal_depth));
		else
			transmit_dist = length(vec2(image_point.x, image_point.z - focal_depth));
	}

	int  direction = beamform_plane * (u_volume_export_pass ^ 1);
	uint ridx      = 0;
	/* NOTE: For Each Acquistion in Raw Data */
	for (uint i = 0; i < dec_data_dim.z; i++) {
		uint base_idx = (i - uforces) / 4;
		uint sub_idx  = (i - uforces) % 4;

		/* NOTE: For Each Virtual Source */
		for (uint j = 0; j < dec_data_dim.y; j++) {
			float dist = transmit_dist + length(rdist);
			float time = dist / speed_of_sound + time_correction;

			/* NOTE: apodization value for this transducer element */
			float a  = cos(clamp(abs(apod_arg * rdist.x), 0, 0.25 * radians(360)));
			a        = a * a;

			vec2 p   = cubic(ridx, time * sampling_frequency);
			/* NOTE: tribal knowledge; this is a problem with the imaging sequence */
			if (i == 0) p *= inversesqrt(128);
			//p       *= vec2(cos(iq_time_scale * time), sin(iq_time_scale * time));
			sum     += p;

			rdist[direction] -= delta[direction];
			ridx             += dec_data_dim.x;
		}

		rdist[direction]      = starting_dist[direction];
		rdist[direction ^ 1] -= delta[direction ^ 1];
	}
	float val = length(sum);
	if (u_volume_export_pass == 0) imageStore(u_out_data_tex,   out_coord, vec4(val));
	else                           imageStore(u_out_volume_tex, out_coord, vec4(val));
}
