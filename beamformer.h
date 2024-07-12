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
	struct { f32 w, h; };
	f32 E[2];
	Vector2 rl;
} v2;

typedef union {
	struct { f32 x, y, z, w; };
	struct { f32 r, g, b, a; };
	f32 E[4];
	Vector4 rl;
} v4;

typedef union {
	struct { v2 pos, size; };
	Rectangle rl;
} Rect;

enum compute_shaders {
//	CS_FORCES,
	CS_HADAMARD,
//	CS_HERCULES,
	CS_LPF,
	CS_MIN_MAX,
	CS_UFORCES,
	CS_LAST
};

enum program_flags {
	RELOAD_SHADERS = 1 << 0,
	ALLOC_SSBOS    = 1 << 1,
	ALLOC_OUT_TEX  = 1 << 2,
	UPLOAD_FILTER  = 1 << 3,
	DO_COMPUTE     = 1 << 30,
};

typedef struct {
	u32 programs[CS_LAST];

	/* NOTE: One SSBO for raw data and two for decoded data (swapped for chained stages)*/
	u32 raw_data_ssbo;
	u32 rf_data_ssbos[2];
	u32 last_active_ssbo_index;
	u32 hadamard_ssbo;
	uv2 hadamard_dim;

	u32 shared_ubo;

	uv4 rf_data_dim;
	i32 out_data_tex_id;
	i32 mip_view_tex_id;
	i32 mips_level_id;

	u32 lpf_ssbo;
	u32 lpf_order;
	i32 lpf_order_id;
} ComputeShaderCtx;

typedef struct {
	Shader          shader;
	RenderTexture2D output;
	i32             out_data_tex_id;
	i32             db_cutoff_id;
	f32             db;
} FragmentShaderCtx;

#include "beamformer_parameters.h"
typedef struct {
	BeamformerParameters raw;
	b32 upload;
} BeamformerParametersFull;

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

	/* UI Theming */
	v4 bg, fg;
	v4 focused_colour, hovered_colour;
	Font  font;
	u32   font_size;
	u32   font_spacing;

	uv4 out_data_dim;
	u32 out_texture;
	u32 out_texture_unit;
	u32 out_texture_mips;

	ComputeShaderCtx  csctx;
	FragmentShaderCtx fsctx;

	os_pipe data_pipe;
	u32     partial_transfer_count;

	BeamformerParametersFull *params;
} BeamformerCtx;

#endif /*_BEAMFORMER_H_ */
