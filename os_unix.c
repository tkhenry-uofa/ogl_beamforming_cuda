#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct timespec os_filetime;

typedef struct {
	size        filesize;
	os_filetime timestamp;
} os_file_stats;

static Arena
os_new_arena(size capacity)
{
	Arena a = {0};

	size pagesize = sysconf(_SC_PAGESIZE);
	if (capacity % pagesize != 0)
		capacity += (pagesize - capacity % pagesize);

	a.beg = mmap(0, capacity, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
	if (a.beg == MAP_FAILED)
		die("os_new_arena: couldn't allocate memory\n");
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
