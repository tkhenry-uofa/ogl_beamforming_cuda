#version 430 core

layout(local_size_x = 32, local_size_y = 1, local_size_z = 32) in;

layout(rg32f, binding = 0)           uniform image3D u_out_img;
layout(rg32f, binding = 1) readonly  uniform image3D u_in_img;
layout(location = 1)                 uniform float   u_prescale = 1.0;

void main()
{
	ivec3 voxel = ivec3(gl_GlobalInvocationID);
	vec4  sum   = imageLoad(u_out_img, voxel) + u_prescale * imageLoad(u_in_img, voxel);
	imageStore(u_out_img, voxel, sum);
}
