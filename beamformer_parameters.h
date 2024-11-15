/* See LICENSE for license details. */
enum compute_shaders {
	CS_CUDA_DECODE           = 0,
	CS_CUDA_HILBERT          = 1,
	CS_DEMOD                 = 2,
	CS_HADAMARD              = 3,
	CS_HERCULES              = 4,
	CS_MIN_MAX               = 5,
	CS_SUM                   = 6,
	CS_UFORCES               = 7,
	CS_LAST
};

#define MAX_BEAMFORMED_SAVED_FRAMES 16
#define MAX_MULTI_XDC_COUNT         4
/* NOTE: This struct follows the OpenGL std140 layout. DO NOT modify unless you have
 * read and understood the rules, particulary with regards to _member alignment_ */
typedef struct {
	u16 channel_mapping[512];   /* Transducer Channel to Verasonics Channel */
	u32 uforces_channels[128];  /* Channels used for virtual UFORCES elements */
	f32 xdc_origin[4 * MAX_MULTI_XDC_COUNT];  /* [m] Corner of transducer being treated as origin */
	f32 xdc_corner1[4 * MAX_MULTI_XDC_COUNT]; /* [m] Corner of transducer along first axis */
	f32 xdc_corner2[4 * MAX_MULTI_XDC_COUNT]; /* [m] Corner of transducer along second axis */
	uv4 dec_data_dim;           /* Samples * Channels * Acquisitions; last element ignored */
	uv4 output_points;          /* Width * Height * Depth * (Frame Average Count) */
	v4  output_min_coordinate;  /* [m] Back-Top-Left corner of output region (w ignored) */
	v4  output_max_coordinate;  /* [m] Front-Bottom-Right corner of output region (w ignored)*/
	uv2 rf_raw_dim;             /* Raw Data Dimensions */
	u32 xdc_count;              /* Number of Transducer Arrays (4 max) */
	u32 channel_offset;         /* Offset into channel_mapping: 0 or 128 (rows or columns) */
	f32 speed_of_sound;         /* [m/s] */
	f32 sampling_frequency;     /* [Hz]  */
	f32 center_frequency;       /* [Hz]  */
	f32 focal_depth;            /* [m]   */
	f32 time_offset;            /* pulse length correction time [s]   */
	f32 off_axis_pos;           /* [m] Position on screen normal to beamform in 2D HERCULES */
	i32 beamform_plane;         /* Plane to Beamform in 2D HERCULES */
	f32 f_number;               /* F# (set to 0 to disable) */
} BeamformerParameters;

/* NOTE: garbage to get the prepocessor to properly stringize the value of a macro */
#define str_(x) #x
#define str(x) str_(x)

#define COMPUTE_SHADER_HEADER "\
#version 460 core\n\
\n\
layout(std140, binding = 0) uniform parameters {\n\
	uvec4 channel_mapping[64];    /* Transducer Channel to Verasonics Channel */\n\
	uvec4 uforces_channels[32];   /* Channels used for virtual UFORCES elements */\n\
	vec4  xdc_origin[" str(MAX_MULTI_XDC_COUNT) "];          /* [m] Corner of transducer being treated as origin */\n\
	vec4  xdc_corner1[" str(MAX_MULTI_XDC_COUNT) "];         /* [m] Corner of transducer along first axis (arbitrary) */\n\
	vec4  xdc_corner2[" str(MAX_MULTI_XDC_COUNT) "];         /* [m] Corner of transducer along second axis (arbitrary) */\n\
	uvec4 dec_data_dim;           /* Samples * Channels * Acquisitions; last element ignored */\n\
	uvec4 output_points;          /* Width * Height * Depth * (Frame Average Count) */\n\
	vec4  output_min_coord;       /* [m] Top left corner of output region */\n\
	vec4  output_max_coord;       /* [m] Bottom right corner of output region */\n\
	uvec2 rf_raw_dim;             /* Raw Data Dimensions */\n\
	uint  xdc_count;              /* Number of Transducer Arrays (4 max) */\n\
	uint  channel_offset;         /* Offset into channel_mapping: 0 or 128 (rows or columns) */\n\
	float speed_of_sound;         /* [m/s] */\n\
	float sampling_frequency;     /* [Hz]  */\n\
	float center_frequency;       /* [Hz]  */\n\
	float focal_depth;            /* [m]   */\n\
	float time_offset;            /* pulse length correction time [s]   */\n\
	float off_axis_pos;           /* [m] Position on screen normal to beamform in 2D HERCULES */\n\
	int   beamform_plane;         /* Plane to Beamform in 2D HERCULES */\n\
	float f_number;               /* F# (set to 0 to disable) */\n\
};\n\n"
