/* See LICENSE for license details. */

/* NOTE: This struct follows the OpenGL std140 layout. DO NOT modify unless you have
 * read and understood the rules, particulary with regards to _member alignment_ */
typedef struct {
	u32 channel_mapping[256];   /* Transducer Channel to Verasonics Channel */
	u32 uforces_channels[128];  /* Channels used for virtual UFORCES elements */
	uv4 rf_data_dim;            /* Samples * Channels * Acquisitions; last element ignored */
	uv4 output_points;          /* Width * Height * Depth; last element ignored */
	u32 channel_data_stride;    /* Data points between channels (samples * acq + padding) */
	u32 channel_offset;         /* Offset into channel_mapping: 0 or 128 (rows or columns) */
	f32 speed_of_sound;         /* [m/s] */
	f32 sampling_frequency;     /* [Hz]  */
	f32 focal_depth;            /* [m]   */
} BeamformerParameters;
