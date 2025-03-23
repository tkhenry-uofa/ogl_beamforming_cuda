/* See LICENSE for license details. */

/* X(enumarant, number, shader file name, needs header, pretty name) */
#define COMPUTE_SHADERS \
	X(CUDA_DECODE,  0, "",         0, "CUDA Decoding")  \
	X(CUDA_HILBERT, 1, "",         0, "CUDA Hilbert")   \
	X(DAS,          2, "das",      1, "DAS")            \
	X(DECODE,       3, "decode",   1, "Decoding")       \
	X(DECODE_FLOAT, 4, "",         1, "Decoding (F32)") \
	X(DEMOD,        5, "demod",    1, "Demodulation")   \
	X(MIN_MAX,      6, "min_max",  0, "Min/Max")        \
	X(SUM,          7, "sum",      0, "Sum")

typedef enum {
	#define X(e, n, s, h, pn) CS_ ##e = n,
	COMPUTE_SHADERS
	#undef X
	CS_LAST
} ComputeShaderID;

/* X(type, id, pretty name) */
#define DECODE_TYPES \
	X(NONE,     0, "None")     \
	X(HADAMARD, 1, "Hadamard")

/* X(type, id, pretty name, fixed transmits) */
#define DAS_TYPES \
	X(FORCES,    0, "FORCES",    1) \
	X(UFORCES,   1, "UFORCES",   0) \
	X(HERCULES,  2, "HERCULES",  1) \
	X(RCA_VLS,   3, "VLS",       0) \
	X(RCA_TPW,   4, "TPW",       0) \
	X(UHERCULES, 5, "UHERCULES", 0)

#define DAS_LOCAL_SIZE_X 32
#define DAS_LOCAL_SIZE_Y  1
#define DAS_LOCAL_SIZE_Z 32

#define MAX_BEAMFORMED_SAVED_FRAMES 16
/* NOTE: This struct follows the OpenGL std140 layout. DO NOT modify unless you have
 * read and understood the rules, particulary with regards to _member alignment_ */

typedef struct {
	v4  output_min_coordinate;  /* [m] Back-Top-Left corner of output region (w ignored) */
	v4  output_max_coordinate;  /* [m] Front-Bottom-Right corner of output region (w ignored)*/
	uv4 output_points;          /* Width * Height * Depth * (Frame Average Count) */
	f32 sampling_frequency;     /* [Hz]  */
	f32 center_frequency;       /* [Hz]  */
	f32 speed_of_sound;         /* [m/s] */
	f32 off_axis_pos;           /* [m] Position on screen normal to beamform in 2D HERCULES */
	i32 beamform_plane;         /* Plane to Beamform in 2D HERCULES */
	f32 f_number;               /* F# (set to 0 to disable) */
} BeamformerUIParameters;

typedef struct {
	u16 channel_mapping[256];   /* Transducer Channel to Verasonics Channel */
	u16 uforces_channels[256];  /* Channels used for virtual UFORCES elements */
	f32 focal_depths[256];      /* [m] Focal Depths for each transmit of a RCA imaging scheme*/
	f32 transmit_angles[256];   /* [radians] Transmit Angles for each transmit of a RCA imaging scheme*/
	f32 xdc_transform[16];      /* IMPORTANT: column major order */
	uv4 dec_data_dim;           /* Samples * Channels * Acquisitions; last element ignored */
	f32 xdc_element_pitch[2];   /* [m] Transducer Element Pitch {row, col} */
	uv2 rf_raw_dim;             /* Raw Data Dimensions */
	i32 transmit_mode;          /* Method/Orientation of Transmit */
	u32 decode;                 /* Decode or just reshape data */
	u32 das_shader_id;
	f32 time_offset;            /* pulse length correction time [s]   */

	/* TODO(rnp): actually use a substruct but generate a header compatible with MATLAB */
	/* UI Parameters */
	v4  output_min_coordinate;  /* [m] Back-Top-Left corner of output region (w ignored) */
	v4  output_max_coordinate;  /* [m] Front-Bottom-Right corner of output region (w ignored)*/
	uv4 output_points;          /* Width * Height * Depth * (Frame Average Count) */
	f32 sampling_frequency;     /* [Hz]  */
	f32 center_frequency;       /* [Hz]  */
	f32 speed_of_sound;         /* [m/s] */
	f32 off_axis_pos;           /* [m] Position on screen normal to beamform in 2D HERCULES */
	i32 beamform_plane;         /* Plane to Beamform in 2D HERCULES */
	f32 f_number;               /* F# (set to 0 to disable) */

	u32 readi_group_id;         /* Which readi group this data is from */
	u32 readi_group_size;       /* Size of readi transmit group */
} BeamformerParameters;

_Static_assert((offsetof(BeamformerParameters, output_min_coordinate) & 15) == 0,
               "BeamformerParameters.output_min_coordinate must lie on a 16 byte boundary");
_Static_assert((sizeof(BeamformerParameters) & 15) == 0,
               "sizeof(BeamformerParameters) must be a multiple of 16");

#define COMPUTE_SHADER_HEADER "\
#version 460 core\n\
\n\
layout(std140, binding = 0) uniform parameters {\n\
	uvec4 channel_mapping[32];    /* Transducer Channel to Verasonics Channel */\n\
	uvec4 uforces_channels[32];   /* Channels used for virtual UFORCES elements */\n\
	vec4  focal_depths[64];       /* [m] Focal Depths for each transmit of a RCA imaging scheme*/\n\
	vec4  transmit_angles[64];    /* [radians] Transmit Angles for each transmit of a RCA imaging scheme*/\n\
	mat4  xdc_transform;          /* IMPORTANT: column major order */\n\
	uvec4 dec_data_dim;           /* Samples * Channels * Acquisitions; last element ignored */\n\
	vec2  xdc_element_pitch;      /* [m] Transducer Element Pitch {row, col} */\n\
	uvec2 rf_raw_dim;             /* Raw Data Dimensions */\n\
	int   transmit_mode;          /* Method/Orientation of Transmit */\n\
	uint  decode;                 /* Decode or just reshape data */\n\
	uint  das_shader_id;\n\
	float time_offset;            /* pulse length correction time [s]   */\n\
	vec4  output_min_coord;       /* [m] Top left corner of output region */\n\
	vec4  output_max_coord;       /* [m] Bottom right corner of output region */\n\
	uvec4 output_points;          /* Width * Height * Depth * (Frame Average Count) */\n\
	float sampling_frequency;     /* [Hz]  */\n\
	float center_frequency;       /* [Hz]  */\n\
	float speed_of_sound;         /* [m/s] */\n\
	float off_axis_pos;           /* [m] Position on screen normal to beamform in 2D HERCULES */\n\
	int   beamform_plane;         /* Plane to Beamform in 2D HERCULES */\n\
	float f_number;               /* F# (set to 0 to disable) */\n\
	uint  readi_group_id;         /* Which readi group this data is from */\n\
	uint  readi_group_size;       /* Size of readi transmit group */\n\
};\n\n"
