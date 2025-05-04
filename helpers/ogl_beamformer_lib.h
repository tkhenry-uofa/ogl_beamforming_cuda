/* See LICENSE for license details. */
#include <stdint.h>
#include "../beamformer_parameters.h"

#if defined(_WIN32)
#define LIB_FN __declspec(dllexport)
#else
#define LIB_FN
#endif

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
