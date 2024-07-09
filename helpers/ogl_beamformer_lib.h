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

typedef struct { u32 x, y, z; } uv3;
typedef struct { f32 x, y, z; } v3;

typedef struct {
	i16 channel_row_mapping[128];
	i16 channel_column_mapping[128];
	i16 uforces_channels[128];
	u32 channel_data_stride;
	f32 speed_of_sound;
	f32 sampling_frequency;
	uv3 output_points;
} BeamformerParameters;

LIB_FN void set_beamformer_parameters(BeamformerParameters *);
LIB_FN void send_data(char *, i16 *, uv3 data_dim);
