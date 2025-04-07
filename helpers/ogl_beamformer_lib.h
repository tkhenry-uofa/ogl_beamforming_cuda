/* See LICENSE for license details. */
#include <stddef.h>
#include <stdint.h>

typedef int16_t   i16;
typedef uint16_t  u16;
typedef int32_t   i32;
typedef uint32_t  u32;
typedef uint32_t  b32;
typedef float     f32;
typedef struct { u32 x, y; }       uv2;
typedef struct { u32 x, y, z, w; } uv4;
typedef struct { f32 x, y, z, w; } v4;

#include "../beamformer_parameters.h"

#if defined(_WIN32)
#define LIB_FN __declspec(dllexport)
#else
#define LIB_FN
#endif

/* IMPORTANT: timeout of -1 will block forever */

LIB_FN b32 set_beamformer_parameters(char *shm_name, BeamformerParametersV0 *);
LIB_FN b32 set_beamformer_pipeline(char *shm_name, i32 *stages, i32 stages_count);
LIB_FN b32 send_data(char *pipe_name, char *shm_name, void *data, u32 data_size);

/* NOTE: sends data and waits for (complex) beamformed data to be returned.
 * out_data: must be allocated by the caller as 2 f32s per output point. */
LIB_FN b32 beamform_data_synchronized(char *pipe_name, char *shm_name, void *data, u32 data_size,
                                      uv4 output_points, f32 *out_data, i32 timeout_ms);

LIB_FN b32 beamformer_start_compute(char *shm_name, u32 image_plane_tag);

/* NOTE: these functions only queue an upload; you must flush (old data functions or start_compute) */
LIB_FN b32 beamformer_push_data(char *shm_name, void *data, u32 data_size, i32 timeout_ms);
LIB_FN b32 beamformer_push_channel_mapping(char *shm_name, i16 *mapping, u32 count, i32 timeout_ms);
LIB_FN b32 beamformer_push_sparse_elements(char *shm_name, i16 *elements, u32 count, i32 timeout_ms);
LIB_FN b32 beamformer_push_focal_vectors(char *shm_name, f32 *vectors, u32 count, i32 timeout_ms);
LIB_FN b32 beamformer_push_parameters(char *shm_name, BeamformerParameters *bp, i32 timeout_ms);
