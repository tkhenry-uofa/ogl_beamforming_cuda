/* See LICENSE for license details. */
#version 460 core
layout(local_size_x = 32, local_size_y = 1, local_size_z = 32) in;

layout(std430, binding = 1) readonly restrict buffer buffer_1 {
	vec2 rf_data[];
};

layout(std140, binding = 0) uniform parameters {
	uvec4 channel_mapping[64];    /* Transducer Channel to Verasonics Channel */
	uvec4 uforces_channels[32];   /* Channels used for virtual UFORCES elements */
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
	float speed_of_sound;         /* [m/s] */
	float sampling_frequency;     /* [Hz]  */
	float center_frequency;       /* [Hz]  */
	float focal_depth;            /* [m]   */
	float time_offset;            /* pulse length correction time [s]   */
	float off_axis_pos;           /* [m] Position on screen normal to beamform in 2D HERCULES */
	int   beamform_plane;         /* Plane to Beamform in 2D HERCULES */
};

layout(rg32f, binding = 0) writeonly uniform image3D u_out_data_tex;

layout(location = 2) uniform int   u_volume_export_pass;
layout(location = 3) uniform ivec3 u_volume_export_dim_offset;
layout(location = 4) uniform mat3  u_xdc_transform;
layout(location = 5) uniform int   u_xdc_index;

#define C_SPLINE 0.5

#if 1
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

vec3 calc_image_point(vec3 voxel)
{
	ivec3 out_data_dim = imageSize(u_out_data_tex);
	vec4 output_size   = abs(output_max_coord - output_min_coord);
	vec3 image_point   = output_min_coord.xyz + voxel * output_size.xyz / out_data_dim.xyz;

	/* TODO: fix the math so that the image plane can be aritrary */
	image_point.y = 0;

	/* NOTE: move the image point into xdc space */
	image_point = u_xdc_transform * image_point;
	return image_point;
}

void main()
{
	vec3  voxel        = vec3(gl_GlobalInvocationID);
	ivec3 out_coord    = ivec3(gl_GlobalInvocationID);

	/* NOTE: Convert voxel to physical coordinates */
	vec3 edge1         = xdc_corner1[u_xdc_index].xyz - xdc_origin[u_xdc_index].xyz;
	vec3 edge2         = xdc_corner2[u_xdc_index].xyz - xdc_origin[u_xdc_index].xyz;
	vec3 image_point   = calc_image_point(voxel);

	/* NOTE: used for constant F# dynamic receive apodization. This is implemented as:
	 *
	 *                  /        |x_e - x_i|\
	 *    a(x, z) = cos(F# * π * ----------- ) ^ 2
	 *                  \        |z_e - z_i|/
	 *
	 * where x,z_e are transducer element positions and x,z_i are image positions. */
	float f_num    = 0.5;
	float apod_arg = f_num * 0.5 * radians(360) / abs(image_point.z);

	/* NOTE: lerp along a line from one edge of the xdc to the other in the imaging plane */
	vec3 delta      = edge1 / float(dec_data_dim.y);
	vec3 xdc_start  = xdc_origin[u_xdc_index].xyz;
	xdc_start      += edge2 / 2;

	vec3 starting_point = image_point - xdc_start;

	vec2 sum   = vec2(0);
	/* NOTE: skip over channels corresponding to other arrays */
	uint ridx  = u_xdc_index * (dec_data_dim.y / xdc_count) * dec_data_dim.x * dec_data_dim.z;
	/* NOTE: skip first acquisition in uforces since its garbage */
	uint uforces = uint(dec_data_dim.y != dec_data_dim.z);
	ridx += dec_data_dim.y * dec_data_dim.x * uforces;
	for (uint i = uforces; i < dec_data_dim.z; i++) {
		uint base_idx = (i - uforces) / 4;
		uint sub_idx  = (i - uforces) % 4;

		vec3  focal_point   = uforces_channels[base_idx][sub_idx] * delta + xdc_start;
		float transmit_dist = distance(image_point, focal_point);
		vec3 rdist = starting_point;
		for (uint j = 0; j < dec_data_dim.y; j++) {
			float dist = transmit_dist + length(rdist);
			float time = dist / speed_of_sound + time_offset;

			/* NOTE: apodization value for this transducer element */
			float a  = cos(clamp(abs(apod_arg * rdist.x), 0, 0.25 * radians(360)));
			a        = a * a;

			vec2 p   = cubic(ridx, time * sampling_frequency);
			sum     += p * a;
			rdist   -= delta;
			ridx    += dec_data_dim.x;
		}
	}
	imageStore(u_out_data_tex, out_coord, vec4(sum.x, sum.y, 0, 0));
}
