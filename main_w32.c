/* See LICENSE for license details. */
#ifndef _WIN32
#error This file is only meant to be compiled for Win32
#endif

#include "beamformer.h"

#include "os_win32.c"

#define OS_DEBUG_LIB_NAME      ".\\beamformer.dll"
#define OS_DEBUG_LIB_TEMP_NAME ".\\beamformer_temp.dll"

#define OS_CUDA_LIB_NAME       "external\\cuda_toolkit.dll"
#define OS_CUDA_LIB_TEMP_NAME  "external\\cuda_toolkit_temp.dll"

#define OS_RENDERDOC_SONAME    "renderdoc.dll"

#define OS_PATH_SEPERATOR      "\\"

iptr glfwGetWGLContext(iptr);

function iptr
os_get_native_gl_context(iptr window)
{
	return glfwGetWGLContext(window);
}

#include "static.c"

function void
dispatch_file_watch(OS *os, FileWatchDirectory *fw_dir, u8 *buf, Arena arena)
{
	i64 offset = 0;
	TempArena save_point = {0};
	w32_file_notify_info *fni = (w32_file_notify_info *)buf;
	do {
		end_temp_arena(save_point);
		save_point = begin_temp_arena(&arena);

		Stream path = {.data = arena_commit(&arena, KB(1)), .cap = KB(1)};

		if (fni->action != FILE_ACTION_MODIFIED) {
			stream_append_s8(&path, s8("unknown file watch event: "));
			stream_append_u64(&path, fni->action);
			stream_append_byte(&path, '\n');
			os->write_file(os->stderr, stream_to_s8(&path));
			stream_reset(&path, 0);
		}

		stream_append_s8(&path, fw_dir->name);
		stream_append_byte(&path, '\\');

		s8 file_name = s16_to_s8(&arena, (s16){.data = fni->filename,
		                                       .len  = fni->filename_size / 2});
		stream_append_s8(&path, file_name);
		stream_append_byte(&path, 0);
		stream_commit(&path, -1);

		u64 hash = s8_hash(file_name);
		for (u32 i = 0; i < fw_dir->count; i++) {
			FileWatch *fw = fw_dir->data + i;
			if (fw->hash == hash) {
				fw->callback(os, stream_to_s8(&path), fw->user_data, arena);
				break;
			}
		}

		offset = fni->next_entry_offset;
		fni    = (w32_file_notify_info *)((u8 *)fni + offset);
	} while (offset);
}

static void
clear_io_queue(OS *os, BeamformerInput *input, Arena arena)
{
	w32_context *ctx = (w32_context *)os->context;

	iptr handle = ctx->io_completion_handle;
	w32_overlapped *overlapped;
	u32  bytes_read;
	uptr user_data;
	while (GetQueuedCompletionStatus(handle, &bytes_read, &user_data, &overlapped, 0)) {
		w32_io_completion_event *event = (w32_io_completion_event *)user_data;
		switch (event->tag) {
		case W32_IO_FILE_WATCH: {
			FileWatchDirectory *dir = (FileWatchDirectory *)event->context;
			dispatch_file_watch(os, dir, dir->buffer.beg, arena);
			zero_struct(overlapped);
			ReadDirectoryChangesW(dir->handle, dir->buffer.beg, 4096, 0,
			                      FILE_NOTIFY_CHANGE_LAST_WRITE, 0, overlapped, 0);
		} break;
		case W32_IO_PIPE: break;
		}
	}
}

int
main(void)
{
	BeamformerCtx   ctx   = {0};
	BeamformerInput input = {.executable_reloaded = 1};
	Arena temp_memory = os_alloc_arena((Arena){0}, MB(16));
	ctx.error_stream  = stream_alloc(&temp_memory, MB(1));

	ctx.ui_backing_store        = sub_arena(&temp_memory, MB(2), KB(4));
	ctx.os.compute_worker.arena = sub_arena(&temp_memory, MB(2), KB(4));

	#define X(name) ctx.os.name = os_ ## name;
	OS_FNS
	#undef X

	w32_context w32_ctx = {0};
	w32_ctx.io_completion_handle = CreateIoCompletionPort(INVALID_FILE, 0, 0, 0);

	ctx.os.context               = (iptr)&w32_ctx;
	ctx.os.compute_worker.asleep = 1;
	ctx.os.stderr                = GetStdHandle(STD_ERROR_HANDLE);
	ctx.os.export_pipe_name      = OS_EXPORT_PIPE_NAME;

	debug_init(&ctx.os, (iptr)&input, &temp_memory);
	setup_beamformer(&ctx, &temp_memory);
	os_wake_waiters(&ctx.os.compute_worker.sync_variable);

	while (!ctx.should_exit) {
		clear_io_queue(&ctx.os, &input, temp_memory);

		input.last_mouse = input.mouse;
		input.mouse.rl   = GetMousePosition();

		beamformer_frame_step(&ctx, &temp_memory, &input);

		input.executable_reloaded = 0;
	}
}
