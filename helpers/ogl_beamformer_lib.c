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
#include <windows.h>

#define OS_INVALID_FILE (INVALID_HANDLE_VALUE)
typedef HANDLE os_file;
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
		written += w;
		w = write(p.file, data, len);
	} while(written != len && w != 0);
	return written;
}

static void
os_close_pipe(void)
{
	close(g_pipe.file);
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
	HANDLE pipe = CreateFileA(name, GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
	return (os_pipe){.file = pipe, .name = name};
}

static size
os_write_to_pipe(os_pipe p, void *data, size len)
{
	DWORD bytes_written;
	WriteFile(p.file, data, len, &bytes_written, 0);
	return bytes_written;
}

static void
os_close_pipe(void)
{
	CloseHandle(g_pipe.file);
}

static BeamformerParametersFull *
os_open_shared_memory_area(char *name)
{
	HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
	if (h == OS_INVALID_FILE)
		return NULL;

	BeamformerParametersFull *new;
	new = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*new));
	CloseHandle(h);

	return new;
}
#endif

static void
check_shared_memory(char *name)
{
	if (g_bp)
		return;
	g_bp = os_open_shared_memory_area(name);
	if (g_bp == NULL)
		mexErrMsgIdAndTxt("ogl_beamformer:shared_memory",
		                  "failed to open shared memory area");
}

void
set_beamformer_pipeline(char *shm_name, i32 *stages, i32 stages_count)
{
	if (stages_count > ARRAY_COUNT(g_bp->compute_stages)) {
		mexErrMsgIdAndTxt("ogl_beamformer:config", "maximum stage count is %u",
		                  ARRAY_COUNT(g_bp->compute_stages));
		return;
	}

	check_shared_memory(shm_name);

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
			mexErrMsgIdAndTxt("ogl_beamformer:config", "invalid shader stage: %d",
			                  stages[i]);
			return;
		}
	}

	g_bp->compute_stages_count = stages_count;
}

void
send_data(char *pipe_name, char *shm_name, i16 *data, uv2 data_dim)
{
	if (g_pipe.file == OS_INVALID_FILE) {
		g_pipe = os_open_named_pipe(pipe_name);
		if (g_pipe.file == OS_INVALID_FILE) {
			mexErrMsgIdAndTxt("ogl_beamformer:pipe_error", "failed to open pipe");
			return;
		}
	}

	check_shared_memory(shm_name);
	/* TODO: this probably needs a mutex around it if we want to change it here */
	g_bp->raw.rf_raw_dim = data_dim;
	size data_size       = data_dim.x * data_dim.y * sizeof(i16);
	size written         = os_write_to_pipe(g_pipe, data, data_size);
	if (written != data_size)
		mexWarnMsgIdAndTxt("ogl_beamformer:write_error",
		                   "failed to write full data to pipe: wrote: %ld", written);
	g_bp->upload = 1;
}

void
set_beamformer_parameters(char *shm_name, BeamformerParameters *new_bp)
{
	check_shared_memory(shm_name);

	if (!g_bp)
		return;

	u8 *src = (u8 *)new_bp, *dest = (u8 *)&g_bp->raw;
	for (size i = 0; i < sizeof(BeamformerParameters); i++)
		dest[i] = src[i];
	g_bp->upload = 1;
}
