#include <mex.h>

#include <stddef.h>
#include <stdint.h>
typedef int16_t   i16;
typedef int32_t   i32;
typedef ptrdiff_t size;

#if defined(__unix__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define OS_INVALID_FILE (-1)
typedef i32 os_file;
typedef struct {
	os_file  file;
	char    *name;
} os_pipe;

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
os_close_pipe(os_pipe p)
{
	close(p.file);
}

#elif defined(_WIN32)
#include <windows.h>

#define OS_INVALID_FILE (INVALID_HANDLE_VALUE)
typedef HANDLE os_file;
typedef struct {
	os_file  file;
	char    *name;
} os_pipe;

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
os_close_pipe(os_pipe p)
{
	CloseHandle(p.file);
}

#else
#error Unsupported Platform
#endif

/* NOTE: usage: pipe_data_to_beamformer(pipe_name, data) */
void
mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
	if (nrhs != 2) {
		mexPrintf("usage: ogl_beamformer_pipe(pipe_name, data)\n");
		return;
	}

	/* TODO: send i16 data */
	char *pipe_name       = mxArrayToString(prhs[0]);
	const mxArray *mxdata = prhs[1];
	size data_size        = 0;

	os_pipe p = os_open_named_pipe(pipe_name);
	if (p.file == OS_INVALID_FILE) {
		mexPrintf("Couldn't open pipe\n");
		return;
	}

	if (mxIsInt16(mxdata)) {
		os_close_pipe(p);
		mexPrintf("Int16 is not yet supported; convert to Int32 first\n");
		return;
	} else if (mxIsInt32(mxdata)) {
		data_size = mxGetNumberOfElements(mxdata) * sizeof(i32);
	}

	void *data = mxGetPr(mxdata);
	size written = os_write_to_pipe(p, data, data_size);
	if (written != data_size)
		mexPrintf("Failed to write full data to pipe: wrote: %ld\n", written);

	os_close_pipe(p);
}
