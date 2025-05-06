/* See LICENSE for license details. */
#if defined(_WIN32)
#define LIB_FN __declspec(dllexport)
#else
#define LIB_FN
#endif

#define BEAMFORMER_LIB_ERRORS \
	X(NONE,                    0, "None") \
	X(VERSION_MISMATCH,        1, "host-library version mismatch")                \
	X(COMPUTE_STAGE_OVERFLOW,  2, "compute stage overflow: maximum stages: " str(MAX_COMPUTE_SHADER_STAGES)) \
	X(INVALID_COMPUTE_STAGE,   3, "invalid compute shader stage")                 \
	X(INVALID_IMAGE_PLANE,     4, "invalid image plane")                          \
	X(BUFFER_OVERFLOW,         5, "passed buffer size exceeds available space")   \
	X(WORK_QUEUE_FULL,         6, "work queue full")                              \
	X(OPEN_EXPORT_PIPE,        7, "failed to open export pipe")                   \
	X(READ_EXPORT_PIPE,        8, "failed to read full export data from pipe")    \
	X(SHARED_MEMORY,           9, "failed to open shared memory region")          \
	X(SYNC_VARIABLE,          10, "failed to acquire lock within timeout period")

#define X(type, num, string) BF_LIB_ERR_KIND_ ##type = num,
typedef enum {BEAMFORMER_LIB_ERRORS} BeamformerLibErrorKind;
#undef X

LIB_FN uint32_t beamformer_get_api_version(void);

LIB_FN BeamformerLibErrorKind beamformer_get_last_error(void);
LIB_FN const char *beamformer_get_last_error_string(void);
LIB_FN const char *beamformer_error_string(BeamformerLibErrorKind kind);

/* IMPORTANT: timeout of -1 will block forever */

LIB_FN uint32_t set_beamformer_parameters(BeamformerParametersV0 *);
LIB_FN uint32_t set_beamformer_pipeline(int32_t *stages, int32_t stages_count);
LIB_FN uint32_t send_data(void *data, uint32_t data_size);
/* NOTE: sends data and waits for (complex) beamformed data to be returned.
 * out_data: must be allocated by the caller as 2 floats per output point. */
LIB_FN uint32_t beamform_data_synchronized(void *data, uint32_t data_size, uint32_t output_points[3],
                                           float *out_data, int32_t timeout_ms);

LIB_FN uint32_t beamformer_start_compute(uint32_t image_plane_tag);

/* NOTE: these functions only queue an upload; you must flush (old data functions or start_compute) */
LIB_FN uint32_t beamformer_push_data(void *data, uint32_t size, int32_t timeout_ms);
LIB_FN uint32_t beamformer_push_channel_mapping(int16_t *mapping,  uint32_t count, int32_t timeout_ms);
LIB_FN uint32_t beamformer_push_sparse_elements(int16_t *elements, uint32_t count, int32_t timeout_ms);
LIB_FN uint32_t beamformer_push_focal_vectors(float     *vectors,  uint32_t count, int32_t timeout_ms);

LIB_FN uint32_t beamformer_push_parameters(BeamformerParameters *, int32_t timeout_ms);
LIB_FN uint32_t beamformer_push_parameters_ui(BeamformerUIParameters *, int32_t timeout_ms);
LIB_FN uint32_t beamformer_push_parameters_head(BeamformerParametersHead *, int32_t timeout_ms);
