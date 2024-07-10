#include "ogl_beamformer_lib.h"

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

static volatile BeamformerParameters *g_bp;
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

static BeamformerParameters *
os_open_shared_memory_area(char *name)
{
	i32 fd = shm_open(name, O_RDWR, S_IRUSR|S_IWUSR);
	if (fd == -1)
		return NULL;

	BeamformerParameters *new;
	new = mmap(NULL, sizeof(BeamformerParameters), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
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

static BeamformerParameters *
os_open_shared_memory_area(char *name)
{
	HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
	if (h == OS_INVALID_FILE)
		return NULL;

	BeamformerParameters *new;
	new = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BeamformerParameters));
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
send_data(char *pipe_name, i16 *data, uv4 data_dim)
{
	if (g_pipe.file == OS_INVALID_FILE) {
		g_pipe = os_open_named_pipe(pipe_name);
		if (g_pipe.file == OS_INVALID_FILE) {
			mexErrMsgIdAndTxt("ogl_beamformer:pipe_error", "failed to open pipe");
			return;
		}
	}

	check_shared_memory();
	/* TODO: this probably needs a mutex around it if we want to change it here */
	g_bp->rf_data_dim = data_dim;
	size data_size    = data_dim.x * data_dim.y * data_dim.z * sizeof(i16);
	size written      = os_write_to_pipe(g_pipe, data, data_size);
	if (written != data_size)
		mexWarnMsgIdAndTxt("ogl_beamformer:write_error",
		                   "failed to write full data to pipe: wrote: %ld", written);
}

void
set_beamformer_parameters(char *shm_name, BeamformerParameters *new_bp)
{
	check_shared_memory(shm_name);
	u8 *src = (u8 *)new_bp, *dest = (u8 *)g_bp;
	for (size i = 0; i < sizeof(BeamformerParameters); i++)
		dest[i] = src[i];
}
