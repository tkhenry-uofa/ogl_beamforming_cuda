/* See LICENSE for license details. */
enum compute_shaders {
	/* TODO: Probably this should be split up */
	CS_CUDA_DECODE_AND_DEMOD = 0,
	CS_DEMOD                 = 1,
	CS_HADAMARD              = 2,
	CS_HERCULES              = 3,
	CS_MIN_MAX               = 4,
	CS_UFORCES               = 5,
	CS_LAST
};

/* NOTE: This struct follows the OpenGL std140 layout. DO NOT modify unless you have
 * read and understood the rules, particulary with regards to _member alignment_ */
typedef struct {
	u32 channel_mapping[256];   /* Transducer Channel to Verasonics Channel */
	u32 uforces_channels[128];  /* Channels used for virtual UFORCES elements */
	f32 lpf_coefficients[64];   /* Low Pass Filter Cofficients */
	uv4 dec_data_dim;           /* Samples * Channels * Acquisitions; last element ignored */
	uv4 output_points;          /* Width * Height * Depth; last element ignored */
	uv2 rf_raw_dim;             /* Raw Data Dimensions */
	v2  output_min_xz;          /* [m] Top left corner of output region */
	v2  output_max_xz;          /* [m] Bottom right corner of output region */
	v2  xdc_min_xy;             /* [m] Min center of transducer elements */
	v2  xdc_max_xy;             /* [m] Max center of transducer elements */
	u32 channel_offset;         /* Offset into channel_mapping: 0 or 128 (rows or columns) */
	u32 lpf_order;              /* Order of Low Pass Filter */
	f32 speed_of_sound;         /* [m/s] */
	f32 sampling_frequency;     /* [Hz]  */
	f32 center_frequency;       /* [Hz]  */
	f32 focal_depth;            /* [m]   */
	f32 time_offset;            /* pulse length correction time [s]   */
	u32 uforces;                /* mode is UFORCES (1) or FORCES (0) */
	f32 off_axis_pos;           /* Where on the 3rd axis to render the image (Hercules only)*/
} BeamformerParameters;
