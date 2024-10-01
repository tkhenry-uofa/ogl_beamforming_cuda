/* See LICENSE for license details. */
#ifndef _BEAMFORMER_H_
#define _BEAMFORMER_H_

#include <immintrin.h>

#include <glad.h>

#define GRAPHICS_API_OPENGL_43
#include <raylib.h>
#include <rlgl.h>

#include "util.h"

#define BG_COLOUR              (v4){.r = 0.15, .g = 0.12, .b = 0.13, .a = 1.0}
#define FG_COLOUR              (v4){.r = 0.92, .g = 0.88, .b = 0.78, .a = 1.0}
#define FOCUSED_COLOUR         (v4){.r = 0.86, .g = 0.28, .b = 0.21, .a = 1.0}
#define HOVERED_COLOUR         (v4){.r = 0.11, .g = 0.50, .b = 0.59, .a = 1.0}

#define INFO_COLUMN_WIDTH      560
/* NOTE: extra space used for allowing mouse clicks after end of text */
#define TEXT_BOX_EXTRA_X       10.0f

#define TEXT_HOVER_SPEED       5.0f

#define RECT_BTN_COLOUR        (Color){.r = 0x43, .g = 0x36, .b = 0x3a, .a = 0xff}
#define RECT_BTN_BORDER_COLOUR (Color){.r = 0x00, .g = 0x00, .b = 0x00, .a = 0xCC}
#define RECT_BTN_ROUNDNESS     0.3f
#define RECT_BTN_BORDER_WIDTH  6.0f

typedef union {
	struct { f32 x, y; };
	struct { f32 w, h; };
	f32 E[2];
	Vector2 rl;
} v2;

typedef union {
	struct { f32 x, y, z, w; };
	struct { f32 r, g, b, a; };
	struct { v3 xyz; f32 _1; };
	struct { f32 _2; v3 yzw; };
	struct { v2 xy, zw; };
	f32 E[4];
	Vector4 rl;
} v4;

typedef union {
	struct { v3 x, y, z; };
	v3  c[3];
	f32 E[9];
} m3;

typedef union {
	struct { v2 pos, size; };
	Rectangle rl;
} Rect;

enum program_flags {
	RELOAD_SHADERS = 1 << 0,
	GEN_MIPMAPS    = 1 << 29,
	DO_COMPUTE     = 1 << 30,
};

enum gl_vendor_ids {
	GL_VENDOR_AMD,
	GL_VENDOR_INTEL,
	GL_VENDOR_NVIDIA,
};

enum modifiable_value_flags {
	MV_CAUSES_COMPUTE = 1 << 0,
	MV_FLOAT          = 1 << 1,
	MV_INT            = 1 << 2,
	MV_GEN_MIPMAPS    = 1 << 29,
	MV_POWER_OF_TWO   = 1 << 30,
};
typedef struct {
	void *value;
	u32   flags;
	f32   scale;
	union {v2 flimits; iv2 ilimits;};
} BPModifiableValue;

typedef struct {
	char buf[64];
	BPModifiableValue store;
	i32  buf_len;
	i32  cursor;
	f32  cursor_hover_p;
	f32  cursor_blink_t;
	f32  cursor_blink_target;
} InputState;

#include "beamformer_parameters.h"
typedef struct {
	BeamformerParameters raw;
	enum compute_shaders compute_stages[16];
	u32                  compute_stages_count;
	b32                  upload;
} BeamformerParametersFull;

#if defined(__unix__)
	#include "os_unix.c"

	#ifdef _DEBUG
	#define DEBUG_EXPORT
	#define OS_DEBUG_LIB_NAME      "./beamformer.so"
	#define OS_DEBUG_LIB_TEMP_NAME "./beamformer_temp.so"
	#else
	#define DEBUG_EXPORT static
	#endif

	#define OS_CUDA_LIB_NAME      "./external/cuda_toolkit.so"
	#define OS_CUDA_LIB_TEMP_NAME "./external/cuda_toolkit_temp.so"

	#define OS_PIPE_NAME "/tmp/beamformer_data_fifo"
	#define OS_SMEM_NAME "/ogl_beamformer_parameters"
#elif defined(_WIN32)
	#include "os_win32.c"

	#ifdef _DEBUG
	#define DEBUG_EXPORT __declspec(dllexport)
	#define OS_DEBUG_LIB_NAME      "beamformer.dll"
	#define OS_DEBUG_LIB_TEMP_NAME "beamformer_temp.dll"
	#else
	#define DEBUG_EXPORT static
	#endif

	#define OS_CUDA_LIB_NAME      "external\\cuda_toolkit.dll"
	#define OS_CUDA_LIB_TEMP_NAME "external\\cuda_toolkit_temp.dll"

	#define OS_PIPE_NAME "\\\\.\\pipe\\beamformer_data_fifo"
	#define OS_SMEM_NAME "Local\\ogl_beamformer_parameters"
#else
	#error Unsupported Platform!
#endif

#define MAX_FRAMES_IN_FLIGHT 3

#define INIT_CUDA_CONFIGURATION_FN(name) void name(u32 *input_dims, u32 *decoded_dims, u16 *channel_mapping, b32 rx_cols)
typedef INIT_CUDA_CONFIGURATION_FN(init_cuda_configuration_fn);
#define REGISTER_CUDA_BUFFERS_FN(name) void name(u32 *rf_data_ssbos, u32 rf_buffer_count, u32 raw_data_ssbo)
typedef REGISTER_CUDA_BUFFERS_FN(register_cuda_buffers_fn);
#define CUDA_DECODE_FN(name) void name(size_t input_offset, u32 output_buffer_idx)
typedef CUDA_DECODE_FN(cuda_decode_fn);
#define CUDA_HILBERT_FN(name) void name(u32 input_buffer_idx, u32 output_buffer_idx)
typedef CUDA_HILBERT_FN(cuda_hilbert_fn);

typedef struct {
	os_library_handle           lib;
	os_filetime                 timestamp;
	init_cuda_configuration_fn *init_cuda_configuration;
	register_cuda_buffers_fn   *register_cuda_buffers;
	cuda_decode_fn             *cuda_decode;
	cuda_hilbert_fn            *cuda_hilbert;
} CudaLib;

typedef struct {
	u32 programs[CS_LAST];

	u32    timer_index;
	u32    timer_ids[MAX_FRAMES_IN_FLIGHT][CS_LAST];
	GLsync timer_fences[MAX_FRAMES_IN_FLIGHT];
	f32    last_frame_time[CS_LAST];

	/* NOTE: circular buffer of textures for averaging.
	 * Only allocated up to configured frame average count */
	u32 sum_textures[16];
	u32 sum_texture_index;

	/* NOTE: array output textures. Only allocated up to configured array count */
	u32 array_textures[4];

	/* NOTE: the raw_data_ssbo is allocated at 3x the required size to allow for tiled
	 * transfers when the GPU is running behind the CPU. It is not mapped on NVIDIA because
	 * their drivers _will_ store the buffer in the system memory. This doesn't happen
	 * for Intel or AMD and mapping the buffer is preferred. In either case incoming data can
	 * be written to the arena at the appropriate offset for the current raw_data_index. An
	 * additional BufferSubData is needed on NVIDIA to upload the data. */
	GLsync raw_data_fences[MAX_FRAMES_IN_FLIGHT];
	Arena  raw_data_arena;
	u32    raw_data_ssbo;
	u32    raw_data_index;

	/* NOTE: Decoded data is only relevant in the context of a single frame. We use two
	 * buffers so that they can be swapped when chaining multiple compute stages */
	u32 rf_data_ssbos[2];
	u32 last_output_ssbo_index;
	u32 hadamard_ssbo;
	uv2 hadamard_dim;

	u32 shared_ubo;

	uv4 dec_data_dim;
	uv2 rf_raw_dim;

	i32 mips_level_id;
	i32 volume_export_pass_id;
	i32 volume_export_dim_offset_id;
	i32 xdc_transform_id;
	i32 xdc_index_id;

	i32 sum_prescale_id;
} ComputeShaderCtx;

typedef struct {
	Shader          shader;
	RenderTexture2D output;
	i32             out_data_tex_id;
	i32             db_cutoff_id;
	f32             db;
} FragmentShaderCtx;

enum export_state {
	ES_START        = (1 <<  0),
	ES_COMPUTING    = (1 <<  1),
	ES_TIMER_ACTIVE = (1 <<  2),
};

typedef struct {
	Arena volume_buf;
	uv4   volume_dim;
	u32   timer_ids[2];
	f32   runtime;
	u32   volume_texture;
	u32   rf_data_ssbo;
	u32   state;
	u32   dispatch_index;
} ExportCtx;

typedef struct {
	uv2 window_size;
	u32 flags;
	enum gl_vendor_ids gl_vendor_id;

	f32 dt;

	/* UI Theming */
	Font font;
	Font small_font;

	InputState is;

	uv4 out_data_dim;
	u32 out_texture;
	u32 out_texture_unit;
	u32 out_texture_mips;

	ComputeShaderCtx  csctx;
	FragmentShaderCtx fsctx;
	ExportCtx         export_ctx;

	os_pipe data_pipe;
	u32     partial_transfer_count;

	CudaLib cuda_lib;

	BeamformerParametersFull *params;
} BeamformerCtx;

#endif /*_BEAMFORMER_H_ */
