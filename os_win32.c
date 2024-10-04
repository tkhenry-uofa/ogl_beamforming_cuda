/* See LICENSE for license details. */
#include "util.h"

#define PAGE_READWRITE 0x04
#define MEM_COMMIT     0x1000
#define MEM_RESERVE    0x2000
#define MEM_RELEASE    0x8000
#define GENERIC_WRITE  0x40000000
#define GENERIC_READ   0x80000000

#define PIPE_TYPE_BYTE      0x00
#define PIPE_ACCESS_INBOUND 0x01

#define FILE_MAP_ALL_ACCESS 0x000F001F

#define INVALID_HANDLE_VALUE (void *)-1

#define CREATE_ALWAYS  2
#define OPEN_EXISTING  3

typedef struct {
	u16  wProcessorArchitecture;
	u16  _pad1;
	u32  dwPageSize;
	size lpMinimumApplicationAddress;
	size lpMaximumApplicationAddress;
	u64  dwActiveProcessorMask;
	u32  dwNumberOfProcessors;
	u32  dwProcessorType;
	u32  dwAllocationGranularity;
	u16  wProcessorLevel;
	u16  wProcessorRevision;
} w32_sys_info;

typedef struct {
	u32 dwFileAttributes;
	u64 ftCreationTime;
	u64 ftLastAccessTime;
	u64 ftLastWriteTime;
	u32 dwVolumeSerialNumber;
	u32 nFileSizeHigh;
	u32 nFileSizeLow;
	u32 nNumberOfLinks;
	u32 nFileIndexHigh;
	u32 nFileIndexLow;
} w32_file_info;

#define W32(r) __declspec(dllimport) r __stdcall
W32(b32)    CloseHandle(void *);
W32(b32)    CopyFileA(c8 *, c8 *, b32);
W32(void *) CreateFileA(c8 *, u32, u32, void *, u32, u32, void *);
W32(void *) CreateFileMappingA(void *, void *, u32, u32, u32, c8 *);
W32(void *) CreateNamedPipeA(c8 *, u32, u32, u32, u32, u32, u32, void *);
W32(b32)    DeleteFileA(c8 *);
W32(b32)    FreeLibrary(void *);
W32(b32)    GetFileInformationByHandle(void *, void *);
W32(i32)    GetLastError(void);
W32(void *) GetProcAddress(void *, c8 *);
W32(void)   GetSystemInfo(void *);
W32(void *) LoadLibraryA(c8 *);
W32(void *) MapViewOfFile(void *, u32, u32, u32, u64);
W32(b32)    PeekNamedPipe(void *, u8 *, i32, i32 *, i32 *, i32 *);
W32(b32)    ReadFile(void *, u8 *, i32, i32 *, void *);
W32(b32)    WriteFile(void *, u8 *, i32, i32 *, void *);
W32(void *) VirtualAlloc(u8 *, size, u32, u32);
W32(b32)    VirtualFree(u8 *, size, u32);

#define OS_INVALID_FILE (INVALID_HANDLE_VALUE)
typedef void *os_file;
typedef struct {
	os_file  file;
	char    *name;
} os_pipe;

typedef void *os_library_handle;

static Arena
os_alloc_arena(Arena a, size capacity)
{
	w32_sys_info Info;
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

	void *h = CreateFileA(fname, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
	if (h == INVALID_HANDLE_VALUE)
		die("os_read_file: couldn't open file: %s\n", fname);

	s8 ret = s8alloc(a, fsize);

	i32 rlen = 0;
	if (!ReadFile(h, ret.data, ret.len, &rlen, 0) && rlen != ret.len)
		die("os_read_file: couldn't read file: %s\n", fname);
	CloseHandle(h);

	return ret;
}

static b32
os_write_file(char *fname, s8 raw)
{
	if (raw.len > (size)U32_MAX) {
		fputs("os_write_file: writing files > 4GB is not yet support on win32\n", stderr);
		return 0;
	}

	void *h = CreateFileA(fname, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
	if (h == INVALID_HANDLE_VALUE)
		return  0;

	i32 wlen;
	WriteFile(h, raw.data, raw.len, &wlen, 0);
	CloseHandle(h);
	return wlen == raw.len;
}

static FileStats
os_get_file_stats(char *fname)
{
	void *h = CreateFileA(fname, 0, 0, 0, OPEN_EXISTING, 0, 0);
	if (h == INVALID_HANDLE_VALUE) {
		return ERROR_FILE_STATS;
	}

	w32_file_info fileinfo;
	if (!GetFileInformationByHandle(h, &fileinfo)) {
		fputs("os_get_file_stats: couldn't get file info\n", stderr);
		CloseHandle(h);
		return ERROR_FILE_STATS;
	}
	CloseHandle(h);

	size filesize = (size)fileinfo.nFileSizeHigh << 32;
	filesize     |= (size)fileinfo.nFileSizeLow;

	return (FileStats){.filesize  = filesize, .timestamp = fileinfo.ftLastWriteTime};
}

/* NOTE: win32 doesn't pollute the filesystem so no need to waste the user's time */
static void
os_close_named_pipe(os_pipe p)
{
}

static os_pipe
os_open_named_pipe(char *name)
{
	void *h = CreateNamedPipeA(name, PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE, 1,
	                           0, 1 * MEGABYTE, 0, 0);
	return (os_pipe){.file = h, .name = name};
}

static b32
os_poll_pipe(os_pipe p)
{
	i32 bytes_available = 0;
	return PeekNamedPipe(p.file, 0, 1 * MEGABYTE, 0, &bytes_available, 0) && bytes_available;
}

static size
os_read_pipe_data(os_pipe p, void *buf, size len)
{
	i32 total_read = 0;
	ReadFile(p.file, buf, len, &total_read, 0);
	return total_read;
}

static BeamformerParametersFull *
os_open_shared_memory_area(char *name)
{
	void *h = CreateFileMappingA(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, 0,
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
os_load_library(char *name, char *temp_name)
{
	if (temp_name) {
		if (CopyFileA(name, temp_name, 0))
			name = temp_name;
	}

	os_library_handle res = LoadLibraryA(name);
	if (!res)
		TraceLog(LOG_WARNING, "os_load_library(%s): %d\n", name, GetLastError());

	if (temp_name)
		DeleteFileA(temp_name);

	return res;
}

static void *
os_lookup_dynamic_symbol(os_library_handle h, char *name)
{
	if (!h)
		return 0;
	void *res = GetProcAddress(h, name);
	if (!res)
		TraceLog(LOG_WARNING, "os_lookup_dynamic_symbol(%s): %d\n", name, GetLastError());
	return res;
}

static void
os_unload_library(os_library_handle h)
{
	FreeLibrary(h);
}
