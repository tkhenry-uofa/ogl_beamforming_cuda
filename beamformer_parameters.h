/* See LICENSE for license details. */
enum compute_shaders {
	CS_CUDA_DECODE           = 0,
	CS_CUDA_HILBERT          = 1,
	CS_DEMOD                 = 2,
	CS_HADAMARD              = 3,
	CS_HERCULES              = 4,
	CS_MIN_MAX               = 5,
	CS_UFORCES               = 6,
	CS_LAST
};

/* NOTE: This struct follows the OpenGL std140 layout. DO NOT modify unless you have
 * read and understood the rules, particulary with regards to _member alignment_ */
typedef struct {
	u32 channel_mapping[256];   /* Transducer Channel to Verasonics Channel */
	u32 uforces_channels[128];  /* Channels used for virtual UFORCES elements */
	f32 lpf_coefficients[64];   /* Low Pass Filter Cofficients */
	v4  xdc_origin;             /* [m] Corner of transducer being treated as origin */
	v4  xdc_corner1;            /* [m] Corner of transducer along first axis (arbitrary) */
	v4  xdc_corner2;            /* [m] Corner of transducer along second axis (arbitrary) */
	uv4 dec_data_dim;           /* Samples * Channels * Acquisitions; last element ignored */
	uv4 output_points;          /* Width * Height * Depth; last element ignored */
	v4  output_min_coordinate;  /* [m] Back-Top-Left corner of output region (w ignored) */
	v4  output_max_coordinate;  /* [m] Front-Bottom-Right corner of output region (w ignored)*/
	uv2 rf_raw_dim;             /* Raw Data Dimensions */
	u32 channel_offset;         /* Offset into channel_mapping: 0 or 128 (rows or columns) */
	u32 lpf_order;              /* Order of Low Pass Filter */
	f32 speed_of_sound;         /* [m/s] */
	f32 sampling_frequency;     /* [Hz]  */
	f32 center_frequency;       /* [Hz]  */
	f32 focal_depth;            /* [m]   */
	f32 time_offset;            /* pulse length correction time [s]   */
	u32 uforces;                /* mode is UFORCES (1) or FORCES (0) */
	f32 off_axis_pos;           /* [m] Position on screen normal to beamform in 2D HERCULES */
	i32 beamform_plane;         /* Plane to Beamform in 2D HERCULES */
} BeamformerParameters;
