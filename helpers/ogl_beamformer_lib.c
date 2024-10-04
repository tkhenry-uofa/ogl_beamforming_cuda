/* See LICENSE for license details. */

#include "ogl_beamformer_lib.h"
typedef struct {
	BeamformerParameters raw;
	enum compute_shaders compute_stages[16];
	u32                  compute_stages_count;
	b32                  upload;
} BeamformerParametersFull;

#define ARRAY_COUNT(a) (sizeof(a) / sizeof(*a))

#if defined(__unix__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define OS_INVALID_FILE (-1)
typedef i32 os_file;
typedef struct {
	os_file  file;
	char    *name;
} os_pipe;
#elif defined(_WIN32)
#define OPEN_EXISTING        3
#define GENERIC_WRITE        0x40000000
#define FILE_MAP_ALL_ACCESS  0x000F001F
#define INVALID_HANDLE_VALUE (void *)-1

#define W32(r) __declspec(dllimport) r __stdcall
W32(b32)    CloseHandle(void *);
W32(void *) CreateFileA(c8 *, u32, u32, void *, u32, u32, void *);
W32(void *) MapViewOfFile(void *, u32, u32, u32, u64);
W32(void *) OpenFileMappingA(u32, b32, c8 *);
W32(b32)    WriteFile(void *, u8 *, i32, i32 *, void *);

#define OS_INVALID_FILE (INVALID_HANDLE_VALUE)
typedef void *os_file;
typedef struct {
	os_file  file;
	char    *name;
} os_pipe;

#else
#error Unsupported Platform
#endif

static volatile BeamformerParametersFull *g_bp;
static os_pipe g_pipe = {.file = OS_INVALID_FILE};

#if defined(__unix__)
static os_pipe
os_open_named_pipe(char *name)
{
	return (os_pipe){.file = open(name, O_WRONLY), .name = name};
}

static size
os_write_to_pipe(os_pipe p, void *data, size len)
{
	size written = 0, w = 0;
	do {
		if (w != -1)
			written += w;
		w = write(p.file, data + written, len - written);
	} while(written != len && w != 0);
	return written;
}

static BeamformerParametersFull *
os_open_shared_memory_area(char *name)
{
	i32 fd = shm_open(name, O_RDWR, S_IRUSR|S_IWUSR);
	if (fd == -1)
		return NULL;

	BeamformerParametersFull *new;
	new = mmap(NULL, sizeof(*new), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);

	if (new == MAP_FAILED)
		return NULL;

	return new;
}

#elif defined(_WIN32)

static os_pipe
os_open_named_pipe(char *name)
{
	void *pipe = CreateFileA(name, GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
	return (os_pipe){.file = pipe, .name = name};
}

static size
os_write_to_pipe(os_pipe p, void *data, size len)
{
	i32 bytes_written;
	WriteFile(p.file, data, len, &bytes_written, 0);
	return bytes_written;
}

static BeamformerParametersFull *
os_open_shared_memory_area(char *name)
{
	void *h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, 0, name);
	if (h == OS_INVALID_FILE)
		return NULL;

	BeamformerParametersFull *new;
	new = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*new));
	CloseHandle(h);

	return new;
}
#endif

#if defined(MATLAB_CONSOLE)
#define mexErrMsgIdAndTxt  mexErrMsgIdAndTxt_800
#define mexWarnMsgIdAndTxt mexWarnMsgIdAndTxt_800
void mexErrMsgIdAndTxt(const c8 *, c8 *, ...);
void mexWarnMsgIdAndTxt(const c8 *, c8 *, ...);
#define error_msg(...)   mexErrMsgIdAndTxt(__func__, __VA_ARGS__)
#define warning_msg(...) mexWarnMsgIdAndTxt(__func__, __VA_ARGS__)
#else
#define error_msg(...)
#define warning_msg(...)
#endif

static b32
check_shared_memory(char *name)
{
	if (!g_bp) {
		g_bp = os_open_shared_memory_area(name);
		if (!g_bp) {
			error_msg("failed to open shared memory area");
			return 0;
		}
	}
	return 1;
}

b32
set_beamformer_pipeline(char *shm_name, i32 *stages, i32 stages_count)
{
	if (stages_count > ARRAY_COUNT(g_bp->compute_stages)) {
		error_msg("maximum stage count is %u", ARRAY_COUNT(g_bp->compute_stages));
		return 0;
	}

	if (!check_shared_memory(shm_name))
		return 0;

	for (i32 i = 0; i < stages_count; i++) {
		switch (stages[i]) {
		case CS_CUDA_DECODE:
		case CS_CUDA_HILBERT:
		case CS_DEMOD:
		case CS_HADAMARD:
		case CS_HERCULES:
		case CS_MIN_MAX:
		case CS_UFORCES:
			g_bp->compute_stages[i] = stages[i];
			break;
		default:
			error_msg("invalid shader stage: %d", stages[i]);
			return 0;
		}
	}
	g_bp->compute_stages_count = stages_count;

	return 1;
}

b32
send_data(char *pipe_name, char *shm_name, i16 *data, uv2 data_dim)
{
	if (g_pipe.file == OS_INVALID_FILE) {
		g_pipe = os_open_named_pipe(pipe_name);
		if (g_pipe.file == OS_INVALID_FILE) {
			error_msg("failed to open pipe");
			return 0;
		}
	}

	if (!check_shared_memory(shm_name))
		return 0;

	/* TODO: this probably needs a mutex around it if we want to change it here */
	g_bp->raw.rf_raw_dim = data_dim;
	size data_size       = data_dim.x * data_dim.y * sizeof(i16);
	size written         = os_write_to_pipe(g_pipe, data, data_size);
	if (written != data_size)
		warning_msg("failed to write full data to pipe: wrote: %ld", written);
	g_bp->upload = 1;

	return 1;
}

b32
set_beamformer_parameters(char *shm_name, BeamformerParameters *new_bp)
{
	if (!check_shared_memory(shm_name))
		return 0;

	u8 *src = (u8 *)new_bp, *dest = (u8 *)&g_bp->raw;
	for (size i = 0; i < sizeof(BeamformerParameters); i++)
		dest[i] = src[i];
	g_bp->upload = 1;

	return 1;
}
