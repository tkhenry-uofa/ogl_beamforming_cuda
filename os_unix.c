#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define OS_INVALID_FILE (-1)
typedef i32 os_file;
typedef struct {
	os_file  file;
	char    *name;
} os_pipe;

typedef struct timespec os_filetime;

typedef void *os_library_handle;

typedef struct {
	size        filesize;
	os_filetime timestamp;
} os_file_stats;

static Arena
os_alloc_arena(Arena a, size capacity)
{
	size pagesize = sysconf(_SC_PAGESIZE);
	if (capacity % pagesize != 0)
		capacity += pagesize - capacity % pagesize;

	size oldsize = a.end - a.beg;
	if (oldsize > capacity)
		return a;

	if (a.beg)
		munmap(a.beg, oldsize);

	a.beg = mmap(0, capacity, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
	if (a.beg == MAP_FAILED)
		die("os_alloc_arena: couldn't allocate memory\n");
	a.end = a.beg + capacity;
	return a;
}

static s8
os_read_file(Arena *a, char *fname, size fsize)
{
	i32 fd = open(fname, O_RDONLY);
	if (fd < 0)
		die("os_read_file: couldn't open file: %s\n", fname);

	s8 ret = s8alloc(a, fsize);

	size rlen = read(fd, ret.data, ret.len);
	close(fd);

	if (rlen != ret.len)
		die("os_read_file: couldn't read file: %s\n", fname);

	return ret;
}

static os_file_stats
os_get_file_stats(char *fname)
{
	struct stat st;

	if (stat(fname, &st) < 0) {
		fputs("os_get_file_stats: couldn't stat file\n",stderr);
		return (os_file_stats){0};
	}

	return (os_file_stats){
		.filesize  = st.st_size,
		.timestamp = st.st_mtim,
	};
}

static os_pipe
os_open_named_pipe(char *name)
{
	mkfifo(name, 0660);
	return (os_pipe){.file = open(name, O_RDONLY|O_NONBLOCK), .name = name};
}

static void
os_close_named_pipe(os_pipe p)
{
	close(p.file);
	unlink(p.name);
}

static b32
os_poll_pipe(os_pipe p)
{
	struct pollfd pfd = {.fd = p.file, .events = POLLIN};
	poll(&pfd, 1, 0);
	return !!(pfd.revents & POLLIN);
}

static size
os_read_pipe_data(os_pipe p, void *buf, size len)
{
	size r = 0, total_read = 0;
	do {
		if (r != -1)
			total_read += r;
		r = read(p.file, buf + total_read, len - total_read);
	} while (r);
	return total_read;
}

static BeamformerParametersFull *
os_open_shared_memory_area(char *name)
{
	i32 fd = shm_open(name, O_CREAT|O_RDWR, S_IRUSR|S_IWUSR);
	if (fd == -1)
		return NULL;

	if (ftruncate(fd, sizeof(BeamformerParametersFull)) == -1) {
		close(fd);
		return NULL;
	}

	BeamformerParametersFull *new;
	new = mmap(NULL, sizeof(*new), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);

	if (new == MAP_FAILED)
		return NULL;

	return new;
}

static void
os_remove_shared_memory(char *name)
{
	shm_unlink(name);
}

static os_library_handle
os_load_library(char *name)
{
	os_library_handle res = dlopen(name, RTLD_NOW|RTLD_LOCAL);
	if (!res)
		TraceLog(LOG_WARNING, "os_load_library(%s): %s\n", name, dlerror());
	return res;
}

static void *
os_lookup_dynamic_symbol(os_library_handle h, char *name)
{
	void *res = dlsym(h, name);
	if (!res)
		TraceLog(LOG_WARNING, "os_lookup_dynamic_symbol(%s): %s\n", name, dlerror());
	return res;
}

#ifdef _DEBUG
static void
os_close_library(os_library_handle h)
{
	/* NOTE: glibc is buggy gnuware so we need to check this */
	if (h)
		dlclose(h);
}
#endif /* _DEBUG */
