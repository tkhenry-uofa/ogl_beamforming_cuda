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

/* NOTE: This struct follows the OpenGL std140 layout. DO NOT modify unless you have
 * read and understood the rules, particulary with regards to _member alignment_ */
typedef struct {
	u16 channel_mapping[512];   /* Transducer Channel to Verasonics Channel */
	u32 uforces_channels[128];  /* Channels used for virtual UFORCES elements */
	f32 lpf_coefficients[64];   /* Low Pass Filter Cofficients */
	f32 xdc_origin[16];         /* [m] (4 v4s) Corner of transducer being treated as origin */
	f32 xdc_corner1[16];        /* [m] (4 v4s) Corner of transducer along first axis (arbitrary) */
	f32 xdc_corner2[16];        /* [m] (4 v4s) Corner of transducer along second axis (arbitrary) */
	uv4 dec_data_dim;           /* Samples * Channels * Acquisitions; last element ignored */
	uv4 output_points;          /* Width * Height * Depth * (Frame Average Count) */
	v4  output_min_coordinate;  /* [m] Back-Top-Left corner of output region (w ignored) */
	v4  output_max_coordinate;  /* [m] Front-Bottom-Right corner of output region (w ignored)*/
	uv2 rf_raw_dim;             /* Raw Data Dimensions */
	u32 xdc_count;              /* Number of Transducer Arrays (4 max) */
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
	f32 _pad[3];
} BeamformerParameters;
