/* See LICENSE for license details. */
layout(local_size_x = 32, local_size_y = 1, local_size_z = 32) in;

layout(std430, binding = 1) readonly restrict buffer buffer_1 {
	vec2 rf_data[];
};

layout(rg32f, binding = 0) writeonly uniform image3D u_out_data_tex;

layout(location = 2) uniform int   u_volume_export_pass;
layout(location = 3) uniform ivec3 u_volume_export_dim_offset;
layout(location = 4) uniform mat4  u_xdc_transform;
layout(location = 5) uniform int   u_xdc_index;

#define C_SPLINE 0.5

#define TX_ROWS 0
#define TX_COLS 1

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
	vec4 image_point   = vec4(output_min_coord.xyz + voxel * output_size.xyz / out_data_dim, 1);

	switch (das_shader_id) {
	case DAS_ID_UFORCES:
		/* TODO: fix the math so that the image plane can be aritrary */
		image_point.y = 0;
		break;
	case DAS_ID_HERCULES:
		if (u_volume_export_pass == 0)
			image_point.y = off_axis_pos;
		break;
	}


	/* NOTE: move the image point into xdc space */
	image_point = u_xdc_transform * image_point;
	return image_point.xyz;
}

vec2 apodize(vec2 value, float apodization_arg, float distance)
{
	/* NOTE: apodization value for this transducer element */
	float a  = cos(clamp(abs(apodization_arg * distance), 0, 0.25 * radians(360)));
	return value * a * a;
}

float sample_index(float distance)
{
	float  time = distance / speed_of_sound + time_offset;
	return time * sampling_frequency;
}

vec2 HERCULES(vec3 image_point, vec3 delta, uint starting_offset, float apodization_arg)
{
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

	uint ridx      = starting_offset;
	vec3 rdist     = image_point;
	int  direction = beamform_plane * (u_volume_export_pass ^ 1);

	vec2 sum = vec2(0);
	/* NOTE: For Each Acquistion in Raw Data */
	for (uint i = 0; i < dec_data_dim.z; i++) {
		/* NOTE: For Each Virtual Source */
		for (uint j = 0; j < dec_data_dim.y; j++) {
			float sidx = sample_index(transmit_dist + length(rdist));
			vec2 valid = vec2(sidx < dec_data_dim.x);
			/* NOTE: tribal knowledge; this is a problem with the imaging sequence */
			if (i == 0) valid *= inversesqrt(128);

			sum += apodize(cubic(ridx, sidx), apodization_arg, rdist.x) * valid;

			rdist[direction] -= delta[direction];
			ridx             += dec_data_dim.x;
		}

		rdist[direction]      = image_point[direction];
		rdist[direction ^ 1] -= delta[direction ^ 1];
	}
	return sum;
}

vec2 uFORCES(vec3 image_point, vec3 delta, uint starting_offset, float apodization_arg)
{
	/* NOTE: skip first acquisition in uforces since its garbage */
	uint uforces = uint(dec_data_dim.y != dec_data_dim.z);
	uint ridx    = starting_offset + dec_data_dim.y * dec_data_dim.x * uforces;

	vec2 sum = vec2(0);
	for (uint i = uforces; i < dec_data_dim.z; i++) {
		uint base_idx = (i - uforces) / 4;
		uint sub_idx  = (i - uforces) % 4;

		vec3  rdist         = image_point;
		vec3  focal_point   = uforces_channels[base_idx][sub_idx] * delta;
		float transmit_dist = distance(image_point, focal_point);

		for (uint j = 0; j < dec_data_dim.y; j++) {
			float sidx  = sample_index(transmit_dist + length(rdist));
			vec2 valid  = vec2(sidx < dec_data_dim.x);
			sum        += apodize(cubic(ridx, sidx), apodization_arg, rdist.x) * valid;
			rdist      -= delta;
			ridx       += dec_data_dim.x;
		}
	}
	return sum;
}

void main()
{

	/* NOTE: Convert voxel to physical coordinates */
	ivec3 out_coord    = ivec3(gl_GlobalInvocationID);
	vec3  image_point  = calc_image_point(vec3(gl_GlobalInvocationID));

	/* NOTE: array edge vectors for calculating element step delta */
	vec3 edge1 = xdc_corner1[u_xdc_index].xyz - xdc_origin[u_xdc_index].xyz;
	vec3 edge2 = xdc_corner2[u_xdc_index].xyz - xdc_origin[u_xdc_index].xyz;

	/* NOTE: used for constant F# dynamic receive apodization. This is implemented as:
	 *
	 *                  /        |x_e - x_i|\
	 *    a(x, z) = cos(F# * π * ----------- ) ^ 2
	 *                  \        |z_e - z_i|/
	 *
	 * where x,z_e are transducer element positions and x,z_i are image positions. */
	float apod_arg = f_number * 0.5 * radians(360) / abs(image_point.z);

	/* NOTE: skip over channels corresponding to other arrays */
	uint starting_offset = u_xdc_index * (dec_data_dim.y / xdc_count) * dec_data_dim.x * dec_data_dim.z;

	/* NOTE: in (u)FORCES we step along line elements */
	vec3 delta;

	vec2 sum;
	switch (das_shader_id) {
	case DAS_ID_UFORCES:
		/* TODO: there should be a smarter way of detecting this */
		if (edge2.x != 0) delta = vec3(edge2.x, 0, 0) / float(dec_data_dim.y);
		else              delta = vec3(edge1.x, 0, 0) / float(dec_data_dim.y);
		sum = uFORCES(image_point, delta, starting_offset, apod_arg);
		break;
	case DAS_ID_HERCULES:
		/* TODO: there should be a smarter way of detecting this */
		if (edge2.x != 0) delta = vec3(edge2.x, edge1.y, 0) / float(dec_data_dim.y);
		else              delta = vec3(edge1.x, edge2.y, 0) / float(dec_data_dim.y);
		sum = HERCULES(image_point, delta, starting_offset, apod_arg);
		break;
	}

	imageStore(u_out_data_tex, out_coord, vec4(sum.x, sum.y, 0, 0));
}
