/* See LICENSE for license details. */
/* NOTE: inspired by nob: https://github.com/tsoding/nob.h */

/* TODO(rnp):
 * [ ]: bake shaders and font data into binary
 *      - for shaders there is a way of making a separate data section and referring
 *        to it with extern from the C source (bake both data and size)
 *      - use objcopy, maybe need linker script maybe command line flags for ld will work
 * [ ]: cross compile/override baked compiler
 */

#define COMMON_FLAGS "-std=c11", "-Wall", "-Iexternal/include"

#define OUTDIR    "out"
#define OUTPUT(s) OUTDIR "/" s

#include "util.h"

#include <stdarg.h>
#include <stdio.h>

#define is_aarch64 0
#define is_amd64   0
#define is_unix    0
#define is_w32     0
#define is_clang   0

#ifdef __ARM_ARCH_ISA_A64
#undef is_aarch64
#define is_aarch64 1
#elif __x86_64__
#undef is_amd64
#define is_amd64 1
#else
#error unsupported architecture
#endif

#if defined(__linux__)

  #undef  is_unix
  #define is_unix 1

  #include <errno.h>
  #include <string.h>
  #include <sys/select.h>
  #include <sys/wait.h>

  #include "os_linux.c"

  #define OS_SHARED_LIB(s)  s ".so"
  #define OS_MAIN "main_linux.c"

#elif defined(_WIN32)

  #undef  is_w32
  #define is_w32 1

  #include "os_win32.c"

  #define OS_SHARED_LIB(s)  s ".dll"
  #define OS_MAIN "main_w32.c"

#else
  #error Unsupported Platform
#endif

#ifdef __clang__
#undef  is_clang
#define is_clang 1
#define COMPILER "clang"
#else
#define COMPILER "cc"
#endif

#define shift(list, count) ((count)--, *(list)++)

#define da_append_count(a, s, items, item_count) do { \
	da_reserve((a), (s), (s)->count + (item_count));                            \
	mem_copy((s)->data + (s)->count, (items), sizeof(*(items)) * (item_count)); \
	(s)->count += (item_count);                                                 \
} while (0)

#define cmd_append_count da_append_count
#define cmd_append(a, s, ...) da_append_count(a, s, ((char *[]){__VA_ARGS__}), \
                                             (sizeof((char *[]){__VA_ARGS__}) / sizeof(char *)))

typedef struct {
	char **data;
	iz     count;
	iz     capacity;
} CommandList;

typedef struct {
	b32   debug;
	b32   generic;
	b32   report;
	b32   sanitize;
} Options;

#define die(fmt, ...) die_("%s: " fmt, __FUNCTION__, ##__VA_ARGS__)
function void __attribute__((noreturn))
die_(char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	/* TODO(rnp): proper log */
	vfprintf(stderr, format, ap);
	va_end(ap);
	os_fatal(s8(""));
}

function b32
s8_contains(s8 s, u8 byte)
{
	b32 result = 0;
	for (iz i = 0 ; !result && i < s.len; i++)
		result |= s.data[i] == byte;
	return result;
}

function void
stream_push_command(Stream *s, CommandList *c)
{
	if (!s->errors) {
		for (iz i = 0; i < c->count; i++) {
			s8 item    = c_str_to_s8(c->data[i]);
			if (item.len) {
				b32 escape = s8_contains(item, ' ') || s8_contains(item, '"');
				if (escape) stream_append_byte(s, '\'');
				stream_append_s8(s, item);
				if (escape) stream_append_byte(s, '\'');
				if (i != c->count - 1) stream_append_byte(s, ' ');
			}
		}
	}
}

#if defined(__unix__)

function b32
os_rename_file(char *name, char *new)
{
	b32 result = rename(name, new) != -1;
	return result;
}

function b32
os_remove_file(char *name)
{
	b32 result = remove(name) != -1;
	return result;
}

function void
os_make_directory(char *name)
{
	mkdir(name, 0770);
}

function u64
os_get_filetime(char *file)
{
	struct stat sb;
	u64 result = (u64)-1;
	if (stat(file, &sb) != -1)
		result = sb.st_mtim.tv_sec;
	return result;
}

function iptr
os_spawn_process(CommandList *cmd, Stream sb)
{
	pid_t result = fork();
	switch (result) {
	case -1: die("failed to fork command: %s: %s\n", cmd->data[0], strerror(errno)); break;
	case  0: {
		if (execvp(cmd->data[0], cmd->data) == -1)
			die("failed to exec command: %s: %s\n", cmd->data[0], strerror(errno));
		unreachable();
	} break;
	}
	return (iptr)result;
}

function b32
os_wait_close_process(iptr handle)
{
	b32 result = 0;
	for (;;) {
		i32   status;
		iptr wait_pid = (iptr)waitpid(handle, &status, 0);
		if (wait_pid == -1)
			die("failed to wait on child process: %s\n", strerror(errno));
		if (wait_pid == handle) {
			if (WIFEXITED(status)) {
				status = WEXITSTATUS(status);
				/* TODO(rnp): logging */
				result = status == 0;
				break;
			}
			if (WIFSIGNALED(status)) {
				/* TODO(rnp): logging */
				result = 0;
				break;
			}
		} else {
			/* TODO(rnp): handle multiple children */
			INVALID_CODE_PATH;
		}
	}
	return result;
}

#elif defined(_WIN32)

enum {
	MOVEFILE_REPLACE_EXISTING = 0x01,
};

W32(b32) CreateDirectoryA(c8 *, void *);
W32(b32) CreateProcessA(u8 *, u8 *, iptr, iptr, b32, u32, iptr, u8 *, iptr, iptr);
W32(b32) GetExitCodeProcess(iptr handle, u32 *);
W32(b32) GetFileTime(iptr, iptr, iptr, iptr);
W32(b32) MoveFileExA(c8 *, c8 *, u32);
W32(u32) WaitForSingleObject(iptr, u32);

function void
os_make_directory(char *name)
{
	CreateDirectoryA(name, 0);
}

function b32
os_rename_file(char *name, char *new)
{
	b32 result = MoveFileExA(name, new, MOVEFILE_REPLACE_EXISTING) != 0;
	return result;
}

function b32
os_remove_file(char *name)
{
	b32 result = DeleteFileA(name);
	return result;
}

function u64
os_get_filetime(char *file)
{
	u64 result = (u64)-1;
	iptr h = CreateFileA(file, 0, 0, 0, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
	if (h != INVALID_FILE) {
		struct { u32 low, high; } w32_filetime;
		GetFileTime(h, 0, 0, (iptr)&w32_filetime);
		result = (u64)w32_filetime.high << 32ULL | w32_filetime.low;
		CloseHandle(h);
	}
	return result;
}

function iptr
os_spawn_process(CommandList *cmd, Stream sb)
{
	struct {
		u32 cb;
		u8 *reserved, *desktop, *title;
		u32 x, y, x_size, y_size, x_count_chars, y_count_chars;
		u32 fill_attr, flags;
		u16 show_window, reserved_2;
		u8 *reserved_3;
		iptr std_input, std_output, std_error;
	} w32_startup_info = {
		.cb = sizeof(w32_startup_info),
		.flags = 0x100,
		.std_input  = GetStdHandle(STD_INPUT_HANDLE),
		.std_output = GetStdHandle(STD_OUTPUT_HANDLE),
		.std_error  = GetStdHandle(STD_ERROR_HANDLE),
	};

	struct {
		iptr phandle, thandle;
		u32  pid, tid;
	} w32_process_info = {0};

	/* TODO(rnp): warn if we need to clamp last string */
	sb.widx = MIN(sb.widx, KB(32) - 1);
	if (sb.widx < sb.cap) sb.data[sb.widx]     = 0;
	else                  sb.data[sb.widx - 1] = 0;

	iptr result = INVALID_FILE;
	if (CreateProcessA(0, sb.data, 0, 0, 1, 0, 0, 0, (iptr)&w32_startup_info,
	                   (iptr)&w32_process_info))
	{
		CloseHandle(w32_process_info.thandle);
		result = w32_process_info.phandle;
	}
	return result;
}

function b32
os_wait_close_process(iptr handle)
{
	b32 result = WaitForSingleObject(handle, -1) != 0xFFFFFFFFUL;
	if (result) {
		u32 status;
		GetExitCodeProcess(handle, &status);
		result = status == 0;
	}
	CloseHandle(handle);
	return result;
}

#endif

#define needs_rebuild(b, ...) needs_rebuild_(b, ((char *[]){__VA_ARGS__}), \
                                             (sizeof((char *[]){__VA_ARGS__}) / sizeof(char *)))
function b32
needs_rebuild_(char *binary, char *deps[], iz deps_count)
{
	u64 binary_filetime = os_get_filetime(binary);
	b32 result = binary_filetime == (u64)-1;
	for (iz i = 0; i < deps_count; i++) {
		u64 filetime = os_get_filetime(deps[i]);
		result |= (filetime == (u64)-1) | (filetime > binary_filetime);
	}
	return result;
}

function b32
run_synchronous(Arena a, CommandList *command)
{
	Stream sb = arena_stream(a);
	stream_push_command(&sb, command);
	printf("%.*s\n", (i32)sb.widx, sb.data);
	return os_wait_close_process(os_spawn_process(command, sb));
}

function void
check_rebuild_self(Arena arena, i32 argc, char *argv[])
{
	char *binary = shift(argv, argc);
	if (needs_rebuild(binary, __FILE__, "os_win32.c", "os_linux.c", "util.c", "util.h")) {
		Stream name_buffer = arena_stream(arena);
		stream_append_s8s(&name_buffer, c_str_to_s8(binary), s8(".old"));
		char *old_name = (char *)arena_stream_commit_zero(&arena, &name_buffer).data;

		if (!os_rename_file(binary, old_name))
			die("failed to move: %s -> %s\n", binary, old_name);

		CommandList c = {0};
		cmd_append(&arena, &c, COMPILER, "-march=native", "-O3", COMMON_FLAGS);
		cmd_append(&arena, &c, "-Wno-unused-function", __FILE__, "-o", binary, (void *)0);
		if (!run_synchronous(arena, &c)) {
			os_rename_file(old_name, binary);
			die("failed to rebuild self\n");
		}
		os_remove_file(old_name);

		c.count = 0;
		cmd_append(&arena, &c, binary);
		cmd_append_count(&arena, &c, argv, argc);
		cmd_append(&arena, &c, (void *)0);
		if (!run_synchronous(arena, &c))
			os_exit(1);

		os_exit(0);
	}
}

function b32
s8_equal(s8 a, s8 b)
{
	b32 result = a.len == b.len;
	for (iz i = 0; result && i < a.len; i++)
		result = a.data[i] == b.data[i];
	return result;
}

function void
usage(char *argv0)
{
	die("%s [--debug] [--report] [--sanitize]\n"
	    "    --debug:       dynamically link and build with debug symbols\n"
	    "    --generic:     compile for a generic target (x86-64-v3 or aarch64 with NEON)\n"
	    "    --report:      print compilation stats (clang only)\n"
	    "    --sanitize:    build with ASAN and UBSAN\n"
	    , argv0);
}

function Options
parse_options(i32 argc, char *argv[])
{
	Options result = {0};

	char *argv0 = shift(argv, argc);
	while (argc > 0) {
		char *arg = shift(argv, argc);
		s8 str    = c_str_to_s8(arg);
		if (s8_equal(str, s8("--debug"))) {
			result.debug = 1;
		} else if (s8_equal(str, s8("--generic"))) {
			result.generic = 1;
		} else if (s8_equal(str, s8("--report"))) {
			result.report = 1;
		} else if (s8_equal(str, s8("--sanitize"))) {
			result.sanitize = 1;
		} else {
			usage(argv0);
		}
	}

	return result;
}

function CommandList
cmd_base(Arena *a, Options *o)
{
	CommandList result = {0};
	cmd_append(a, &result, COMPILER);

	/* TODO(rnp): support cross compiling with clang */
	if (!o->generic)     cmd_append(a, &result, "-march=native");
	else if (is_amd64)   cmd_append(a, &result, "-march=x86-64-v3");
	else if (is_aarch64) cmd_append(a, &result, "-march=aarch64");

	cmd_append(a, &result, COMMON_FLAGS);

	if (o->debug) cmd_append(a, &result, "-O0", "-D_DEBUG", "-Wno-unused-function");
	else          cmd_append(a, &result, "-O3");

	if (o->debug && is_unix) cmd_append(a, &result, "-ggdb");

	if (o->sanitize) cmd_append(a, &result, "-fsanitize=address,undefined");

	if (o->report) {
		if (is_clang) cmd_append(a, &result, "-fproc-stat-report");
		else printf("warning: timing not supported with this compiler\n");
		/* TODO(rnp): basic timing */
	}

	return result;
}

/* NOTE(rnp): produce pdbs on w32 */
function void
cmd_pdb(Arena *a, CommandList *cmd)
{
	if (is_w32 && is_clang)
		cmd_append(a, cmd, "-fuse-ld=lld", "-g", "-gcodeview", "-Wl,--pdb=");
}

function void
git_submodule_update(Arena a, char *name)
{
	Stream sb = arena_stream(a);
	stream_append_s8s(&sb, c_str_to_s8(name), s8(OS_PATH_SEPARATOR), s8(".git"));
	stream_append_byte(&sb, 0);
	arena_commit(&a, sb.widx);

	CommandList git = {0};
	/* NOTE(rnp): cryptic bs needed to get a simple exit code if name is dirty */
	cmd_append(&a, &git, "git", "diff-index", "--quiet", "HEAD", "--", name, (void *)0);
	if (!os_file_exists((c8 *)sb.data) || !run_synchronous(a, &git)) {
		git.count = 1;
		cmd_append(&a, &git, "submodule", "update", "--init", "--depth=1", name, (void *)0);
		if (!run_synchronous(a, &git))
			die("failed to clone required module: %s\n", name);
	}
}

function b32
build_shared_library(Arena a, CommandList cc, char *name, char **deps, iz deps_count)
{
	b32 result = 0;
	cmd_append(&a, &cc, "-fPIC", "-shared");
	cmd_append_count(&a, &cc, deps, deps_count);
	cmd_append(&a, &cc, "-o", name, (void *)0);
	result = run_synchronous(a, &cc);
	return result;
}

function b32
build_static_library(Arena a, CommandList cc, char *name, char **deps, char **outputs, iz count)
{
	/* TODO(rnp): refactor to not need outputs */
	b32 result = 0;
	b32 all_success = 1;
	for (iz i = 0; i < count; i++) {
		cmd_append(&a, &cc, "-c", deps[i], "-o", outputs[i], (void *)0);
		all_success &= run_synchronous(a, &cc);
		cc.count -= 5;
	}
	if (all_success) {
		CommandList ar = {0};
		cmd_append(&a, &ar, "ar", "rc", name);
		cmd_append_count(&a, &ar, outputs, count);
		cmd_append(&a, &ar, (void *)0);
		result = run_synchronous(a, &ar);
	}
	return result;
}

function void
check_build_raylib(Arena a, CommandList cc, b32 shared)
{
	char *libraylib = shared ? OS_SHARED_LIB("libraylib") : OUTPUT("libraylib.a");

	if (needs_rebuild(libraylib, __FILE__, "external/include/rlgl.h", "external/raylib")) {
		git_submodule_update(a, "external/raylib");
		os_copy_file("external/raylib/src/rlgl.h", "external/include/rlgl.h");

		if (is_unix) cmd_append(&a, &cc, "-D_GLFW_X11");
		cmd_append(&a, &cc, "-DPLATFORM_DESKTOP_GLFW", "-DGRAPHICS_API_OPENGL_43");
		cmd_append(&a, &cc, "-Wno-unused-but-set-variable");
		cmd_append(&a, &cc, "-Iexternal/raylib/src", "-Iexternal/raylib/src/external/glfw/include");
		#define RAYLIB_SOURCES \
			X(rglfw)     \
			X(rshapes)   \
			X(rtext)     \
			X(rtextures) \
			X(utils)
		#define X(name) "external/raylib/src/" #name ".c",
		char *srcs[] = {"external/rcore_extended.c", RAYLIB_SOURCES};
		#undef X
		#define X(name) OUTPUT(#name ".o"),
		char *outs[] = {OUTPUT("rcore_extended.o"), RAYLIB_SOURCES};
		#undef X

		b32 success;
		if (shared) {
			cmd_append(&a, &cc, "-DBUILD_LIBTYPE_SHARED", "-D_GLFW_BUILD_DLL");
			cmd_append(&a, &cc, "-fPIC", "-shared");
			cmd_pdb(&a, &cc);
			cmd_append_count(&a, &cc, srcs, countof(srcs));
			cmd_append(&a, &cc, "-o", libraylib);
			if (is_w32) cmd_append(&a, &cc, "-L.", "-lgdi32", "-lwinmm");
			cmd_append(&a, &cc, (void *)0);
			success = run_synchronous(a, &cc);
		} else {
			success = build_static_library(a, cc, libraylib, srcs, outs, countof(srcs));
		}
		if (!success) die("failed to build libary: %s\n", libraylib);
	}
}

/* NOTE(rnp): gcc requires these to appear at the end for no reason at all */
function void
cmd_append_ldflags(Arena *a, CommandList *cc, b32 shared)
{
	cmd_pdb(a, cc);
	cmd_append(a, cc, "-lm");
	if (shared && !is_w32) cmd_append(a, cc, "-Wl,-rpath,.");
	if (shared) cmd_append(a, cc, "-L.", "-lraylib");
	if (is_w32) cmd_append(a, cc, "-lgdi32", "-lwinmm", "-lSynchronization");
}

function b32
build_helper_library(Arena arena, CommandList cc)
{
	/////////////
	// library
	char *library = OUTPUT(OS_SHARED_LIB("ogl_beamformer_lib"));
	char *srcs[]  = {"helpers/ogl_beamformer_lib.c"};

	cmd_append(&arena, &cc, "-Wno-unused-function", "-fPIC", "-shared");
	cmd_append_count(&arena, &cc, srcs, countof(srcs));
	cmd_append(&arena, &cc, "-o", library);
	if (is_w32) cmd_append(&arena, &cc, "-lSynchronization");
	cmd_append(&arena, &cc, (void *)0);
	b32 result = run_synchronous(arena, &cc);
	if (!result) fprintf(stderr, "failed to build: %s\n", library);

	/////////////
	// header
	char *lib_header_out = OUTPUT("ogl_beamformer_lib.h");

	b32 rebuild_lib_header = needs_rebuild(lib_header_out, "beamformer_parameters.h",
	                                       "helpers/ogl_beamformer_lib_base.h");
	if (rebuild_lib_header) {
		s8 parameters_header = os_read_whole_file(&arena, "beamformer_parameters.h");
		s8 base_header       = os_read_whole_file(&arena, "helpers/ogl_beamformer_lib_base.h");
		if (parameters_header.len != 0 && base_header.len != 0 &&
		    parameters_header.data + parameters_header.len == base_header.data)
		{
			s8 output_file   = parameters_header;
			output_file.len += base_header.len;
			os_write_new_file(lib_header_out, output_file);
		} else {
			result = 0;
			fprintf(stderr, "failed to build: %s\n", lib_header_out);
		}
	}

	return result;
}

i32
main(i32 argc, char *argv[])
{
	Arena arena = os_alloc_arena((Arena){0}, MB(8));
	check_rebuild_self(arena, argc, argv);

	Options options = parse_options(argc, argv);

	os_make_directory(OUTDIR);

	CommandList c = cmd_base(&arena, &options);
	check_build_raylib(arena, c, options.debug);

	build_helper_library(arena, c);

	/////////////////////////
	// hot reloadable portion
	if (options.debug) {
		iz c_count = c.count;
		if (is_w32) cmd_append_ldflags(&arena, &c, 1);
		if (!build_shared_library(arena, c, OS_SHARED_LIB("beamformer"),
	                                  (char *[]){"beamformer.c"}, 1))
		{
			die("failed to build: " OS_SHARED_LIB("beamfomer") "\n");
		}
		c.count = c_count;
	}

	//////////////////
	// static portion
	cmd_append(&arena, &c, OS_MAIN, "-o", "ogl");
	if (!options.debug) cmd_append(&arena, &c, OUTPUT("libraylib.a"));
	cmd_append_ldflags(&arena, &c, options.debug);
	cmd_append(&arena, &c, (void *)0);

	return !run_synchronous(arena, &c);
}
