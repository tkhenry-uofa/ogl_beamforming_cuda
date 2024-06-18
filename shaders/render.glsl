#version 430

in  vec2 fragTexCoord;
out vec4 v_out_colour;

layout(std430, binding = 1) readonly buffer beamformed_data
{
	float out_data[];
};

layout(location = 1) uniform uvec3 u_out_data_dim;

/* input:  h [0,360] | s,v [0, 1] *
 * output: rgb [0,1]              */
vec3 hsv2rgb(vec3 hsv)
{
	vec3 k = mod(vec3(5, 3, 1) + hsv.x / 60, 6);
	k = max(min(min(k, 4 - k), 1), 0);
	return hsv.z - hsv.z * hsv.y * k;
}

void main()
{
	ivec2 coord = ivec2(fragTexCoord * u_out_data_dim.xy);
	float smp = out_data[coord.y * u_out_data_dim.x + coord.x];
	smp = 20 * log(abs(smp) + 1e-12) + 60;

	v_out_colour = vec4(hsv2rgb(vec3(smp, 0.8, 0.95)), 1);
}
