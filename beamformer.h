/* See LICENSE for license details. */
#ifndef _BEAMFORMER_H_
#define _BEAMFORMER_H_

#include <immintrin.h>

#define GRAPHICS_API_OPENGL_43
#include <raylib.h>
#include <rlgl.h>

#include "util.h"

typedef union {
	struct { f32 x, y; };
	f32 E[2];
	Vector2 rl;
} v2;

typedef union {
	struct { f32 x, y, z, w; };
	struct { f32 r, g, b, a; };
	f32 E[4];
	Vector4 rl;
} v4;

enum compute_shaders {
//	CS_FORCES,
	CS_HADAMARD,
//	CS_HERCULES,
	CS_MIN_MAX,
	CS_UFORCES,
	CS_LAST
};

enum program_flags {
	RELOAD_SHADERS = 1 << 0,
	DO_COMPUTE     = 1 << 1,
};

typedef struct {
	u32 programs[CS_LAST];

	/* NOTE: One SSBO for raw data and one for decoded data */
	u32 rf_data_ssbos[2];
	u32 hadamard_ssbo;
	uv2 hadamard_dim;

	uv4 rf_data_dim;
	i32 rf_data_dim_id;
	i32 out_data_tex_id;
	i32 mip_view_tex_id;
	i32 mips_level_id;
} ComputeShaderCtx;

typedef struct {
	Shader          shader;
	RenderTexture2D output;
	i32             out_data_tex_id;
	i32             db_cutoff_id;
	f32             db;
} FragmentShaderCtx;

#include "beamformer_parameters.h"

#if defined(__unix__)
	#define GL_GLEXT_PROTOTYPES 1
	#include <GL/glcorearb.h>
	#include <GL/glext.h>
	#include "os_unix.c"
#elif defined(_WIN32)
	#include <glad.h>
	#include "os_win32.c"
#else
	#error Unsupported Platform!
#endif

typedef struct {
	uv2 window_size;
	u32 flags;

	Font font;

	Color bg, fg;

	uv4 out_data_dim;
	u32 out_texture;
	u32 out_texture_unit;
	u32 out_texture_mips;

	ComputeShaderCtx  csctx;
	FragmentShaderCtx fsctx;

	os_pipe data_pipe;
	u32     partial_transfer_count;

	BeamformerParameters *params;
} BeamformerCtx;

#endif /*_BEAMFORMER_H_ */
