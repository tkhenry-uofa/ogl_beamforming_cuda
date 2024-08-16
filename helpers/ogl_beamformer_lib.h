/* NOTE: mex.h can still be used to get access to functions that print to the matlab console */
#include <mex.h>

#include <stddef.h>
#include <stdint.h>

typedef uint8_t   u8;
typedef int16_t   i16;
typedef int32_t   i32;
typedef uint32_t  u32;
typedef uint32_t  b32;
typedef float     f32;
typedef double    f64;
typedef ptrdiff_t size;

#if defined(_WIN32)
#define LIB_FN __declspec(dllexport)
#else
#define LIB_FN
#endif

typedef struct { f32 x, y; }       v2;
typedef struct { u32 x, y; }       uv2;
typedef struct { u32 x, y, z, w; } uv4;

#include "../beamformer_parameters.h"

LIB_FN void set_beamformer_parameters(char *shm_name, BeamformerParameters *);
LIB_FN void set_beamformer_pipeline(char *shm_name, i32 *stages, i32 stages_count);
LIB_FN void send_data(char *pipe_name, char *shm_name, i16 *data, uv2 data_dim);
