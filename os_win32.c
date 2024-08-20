/* See LICENSE for license details. */
#include <fileapi.h>
#include <handleapi.h>
#include <libloaderapi.h>
#include <memoryapi.h>
#include <sysinfoapi.h>

/* NOTE: copied from wtypes.h; we don't actually use this type but winbase.h needs it defined */
typedef void *HWND;
#include <winbase.h>

#include "util.h"

#define OS_INVALID_FILE (INVALID_HANDLE_VALUE)
typedef HANDLE os_file;
typedef struct {
	os_file  file;
	char    *name;
} os_pipe;

typedef FILETIME os_filetime;
typedef HANDLE   os_library_handle;

typedef struct {
	size        filesize;
	os_filetime timestamp;
} os_file_stats;

static Arena
os_alloc_arena(Arena a, size capacity)
{
	SYSTEM_INFO Info;
	GetSystemInfo(&Info);

	if (capacity % Info.dwPageSize != 0)
		capacity += (Info.dwPageSize - capacity % Info.dwPageSize);

	size oldsize = a.end - a.beg;
	if (oldsize > capacity)
		return a;

	if (a.beg)
		VirtualFree(a.beg, oldsize, MEM_RELEASE);

	a.beg = VirtualAlloc(0, capacity, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
	if (a.beg == NULL)
		die("os_alloc_arena: couldn't allocate memory\n");
	a.end = a.beg + capacity;
	return a;
}

static s8
os_read_file(Arena *a, char *fname, size fsize)
{
	if (fsize > (size)U32_MAX)
		die("os_read_file: %s\nHandling files >4GB is not yet "
		    "handled in win32 code\n", fname);

	HANDLE h = CreateFileA(fname, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
	if (h == INVALID_HANDLE_VALUE)
		die("os_read_file: couldn't open file: %s\n", fname);

	s8 ret = s8alloc(a, fsize);

	DWORD rlen = 0;
	if (!ReadFile(h, ret.data, ret.len, &rlen, 0) && rlen != ret.len)
		die("os_read_file: couldn't read file: %s\n", fname);
	CloseHandle(h);

	return ret;
}

static os_file_stats
os_get_file_stats(char *fname)
{
	HANDLE h = CreateFileA(fname, 0, 0, 0, OPEN_EXISTING, 0, 0);
	if (h == INVALID_HANDLE_VALUE) {
		fputs("os_get_file_stats: couldn't open file\n", stderr);
		return (os_file_stats){0};
	}

	BY_HANDLE_FILE_INFORMATION fileinfo;
	if (!GetFileInformationByHandle(h, &fileinfo)) {
		fputs("os_get_file_stats: couldn't get file info\n", stderr);
		CloseHandle(h);
		return (os_file_stats){0};
	}
	CloseHandle(h);

	size filesize = (size)fileinfo.nFileSizeHigh << 32;
	filesize     |= (size)fileinfo.nFileSizeLow;
	return (os_file_stats){
		.filesize  = filesize,
		.timestamp = fileinfo.ftLastWriteTime,
	};
}

/* NOTE: win32 doesn't pollute the filesystem so no need to waste the user's time */
static void
os_close_named_pipe(os_pipe p)
{
}

static os_pipe
os_open_named_pipe(char *name)
{
	HANDLE h = CreateNamedPipeA(name, PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE, 1,
	                            0, 1 * MEGABYTE, 0, 0);
	return (os_pipe){.file = h, .name = name};
}

static b32
os_poll_pipe(os_pipe p)
{
	DWORD bytes_available = 0;
	return PeekNamedPipe(p.file, 0, 1 * MEGABYTE, 0, &bytes_available, 0) && bytes_available;
}

static size
os_read_pipe_data(os_pipe p, void *buf, size len)
{
	DWORD total_read = 0;
	ReadFile(p.file, buf, len, &total_read, 0);
	return total_read;
}

static BeamformerParametersFull *
os_open_shared_memory_area(char *name)
{
	HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, 0,
	                              sizeof(BeamformerParametersFull), name);
	if (h == INVALID_HANDLE_VALUE)
		return NULL;

	BeamformerParametersFull *new;
	new = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*new));

	return new;
}

/* NOTE: closing the handle releases the memory and this happens when program terminates */
static void
os_remove_shared_memory(char *name)
{
}

static os_library_handle
os_load_library(char *name)
{
	os_library_handle res = LoadLibraryA(name);
	if (!res)
		TraceLog(LOG_WARNING, "os_load_library(%s): %d\n", name, GetLastError());
	return res;
}

static void *
os_lookup_dynamic_symbol(os_library_handle h, char *name)
{
	if (!h)
		return 0;
	void *res = GetProcAddress(h, name);
	if (!res)
		TraceLog(LOG_WARNING, "os_lookup_dynamic_symbol(%s): %s\n", name, GetLastError());
	return res;
}

#ifdef _DEBUG
static void
os_close_library(os_library_handle h)
{
	FreeLibrary(h);
}
#endif /* _DEBUG */
