/* See LICENSE for license details. */
#include "../util.h"
#include "ogl_beamformer_lib.h"
#include "../beamformer_work_queue.c"

#define PIPE_RETRY_PERIOD_MS (100ULL)

global BeamformerSharedMemory *g_bp;

#if defined(__linux__)
#include "../os_linux.c"
#elif defined(_WIN32)
#include "../os_win32.c"

#define PIPE_TYPE_BYTE      0x00
#define PIPE_ACCESS_INBOUND 0x01

#define PIPE_WAIT   0x00
#define PIPE_NOWAIT 0x01

#define ERROR_NO_DATA            232L
#define ERROR_PIPE_NOT_CONNECTED 233L
#define ERROR_PIPE_LISTENING     536L

W32(iptr) CreateNamedPipeA(c8 *, u32, u32, u32, u32, u32, u32, void *);
W32(b32)  DisconnectNamedPipe(iptr);
W32(iptr) OpenFileMappingA(u32, b32, c8 *);
W32(void) Sleep(u32);

#else
#error Unsupported Platform
#endif

#if defined(__linux__)

function Pipe
os_open_read_pipe(char *name)
{
	mkfifo(name, 0660);
	return (Pipe){.file = open(name, O_RDONLY|O_NONBLOCK), .name = name};
}

static void
os_disconnect_pipe(Pipe p)
{
}

static void
os_close_pipe(iptr *file, char *name)
{
	if (file) close(*file);
	if (name) unlink(name);
	*file = INVALID_FILE;
}

static b32
os_wait_read_pipe(Pipe p, void *buf, iz read_size, u32 timeout_ms)
{
	struct pollfd pfd = {.fd = p.file, .events = POLLIN};
	iz total_read = 0;
	if (poll(&pfd, 1, timeout_ms) > 0) {
		iz r;
		do {
			 r = read(p.file, (u8 *)buf + total_read, read_size - total_read);
			 if (r > 0) total_read += r;
		} while (r != 0);
	}
	return total_read == read_size;
}

static BeamformerSharedMemory *
os_open_shared_memory_area(char *name)
{
	BeamformerSharedMemory *result = 0;
	i32 fd = shm_open(name, O_RDWR, S_IRUSR|S_IWUSR);
	if (fd > 0) {
		void *new = mmap(0, BEAMFORMER_SHARED_MEMORY_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
		if (new != MAP_FAILED)
			result = new;
		close(fd);
	}
	return result;
}

#elif defined(_WIN32)

/* TODO(rnp): temporary workaround */
function OS_WAIT_ON_VALUE_FN(os_wait_on_value_stub)
{
	/* TODO(rnp): this doesn't work across processes on win32 (return 1 to cause a spin wait) */
	return 1;
	return WaitOnAddress(value, &current, sizeof(*value), timeout_ms);
}
#define os_wait_on_value os_wait_on_value_stub

static Pipe
os_open_read_pipe(char *name)
{
	iptr file = CreateNamedPipeA(name, PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE|PIPE_NOWAIT, 1,
	                             0, 1024UL * 1024UL, 0, 0);
	return (Pipe){.file = file, .name = name};
}

static void
os_disconnect_pipe(Pipe p)
{
	DisconnectNamedPipe(p.file);
}

static void
os_close_pipe(iptr *file, char *name)
{
	if (file) CloseHandle(*file);
	*file = INVALID_FILE;
}

static b32
os_wait_read_pipe(Pipe p, void *buf, iz read_size, u32 timeout_ms)
{
	iz elapsed_ms = 0, total_read = 0;
	while (elapsed_ms <= timeout_ms && read_size != total_read) {
		u8 data;
		i32 read;
		b32 result = ReadFile(p.file, &data, 0, &read, 0);
		if (!result) {
			i32 error = GetLastError();
			if (error != ERROR_NO_DATA &&
			    error != ERROR_PIPE_LISTENING &&
			    error != ERROR_PIPE_NOT_CONNECTED)
			{
				/* NOTE: pipe is in a bad state; we will never read anything */
				break;
			}
			Sleep(PIPE_RETRY_PERIOD_MS);
			elapsed_ms += PIPE_RETRY_PERIOD_MS;
		} else {
			ReadFile(p.file, (u8 *)buf + total_read, read_size - total_read, &read, 0);
			total_read += read;
		}
	}
	return total_read == read_size;
}

function BeamformerSharedMemory *
os_open_shared_memory_area(char *name)
{
	BeamformerSharedMemory *result = 0;
	iptr h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, 0, name);
	if (h != INVALID_FILE) {
		result = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, BEAMFORMER_SHARED_MEMORY_SIZE);
		CloseHandle(h);
	}

	return result;
}

#endif

function b32
check_shared_memory(void)
{
	b32 result = 1;
	if (!g_bp) {
		g_bp = os_open_shared_memory_area(OS_SHARED_MEMORY_NAME);
		if (!g_bp) result = 0;
	}
	return result;
}

b32
set_beamformer_pipeline(i32 *stages, i32 stages_count)
{
	if (stages_count > ARRAY_COUNT(g_bp->compute_stages)) {
		//error_msg("maximum stage count is %lu", ARRAY_COUNT(g_bp->compute_stages));
		return 0;
	}

	if (!check_shared_memory())
		return 0;

	for (i32 i = 0; i < stages_count; i++) {
		b32 valid = 0;
		#define X(en, number, sfn, nh, pn) if (number == stages[i]) valid = 1;
		COMPUTE_SHADERS
		#undef X

		if (!valid) {
			//error_msg("invalid shader stage: %d", stages[i]);
			return 0;
		}

		g_bp->compute_stages[i] = stages[i];
	}
	g_bp->compute_stages_count = stages_count;

	return 1;
}

b32
beamformer_start_compute(u32 image_plane_tag)
{
	b32 result = image_plane_tag < IPT_LAST && check_shared_memory();
	if (result) {
		result = !atomic_load(&g_bp->dispatch_compute_sync);
		if (result) {
			g_bp->current_image_plane = image_plane_tag;
			atomic_store(&g_bp->dispatch_compute_sync, 1);
		}
	}
	return result;
}

function b32
beamformer_upload_buffer(void *data, u32 size, i32 store_offset, i32 sync_offset,
                         BeamformerUploadKind kind, i32 timeout_ms)
{
	b32 result = check_shared_memory();
	if (result) {
		BeamformWork *work = beamform_work_queue_push(&g_bp->external_work_queue);
		result = work && try_wait_sync((i32 *)((u8 *)g_bp + sync_offset), timeout_ms, os_wait_on_value);
		if (result) {
			BeamformerUploadContext *uc = &work->upload_context;
			uc->shared_memory_offset = store_offset;
			uc->size = size;
			uc->kind = kind;
			work->type = BW_UPLOAD_BUFFER;
			work->completion_barrier = sync_offset;
			mem_copy((u8 *)g_bp + store_offset, data, size);
			beamform_work_queue_push_commit(&g_bp->external_work_queue);
		}
	}
	return result;
}

#define BEAMFORMER_UPLOAD_FNS \
	X(channel_mapping, i16, 1, CHANNEL_MAPPING) \
	X(sparse_elements, i16, 1, SPARSE_ELEMENTS) \
	X(focal_vectors,   f32, 2, FOCAL_VECTORS)

#define X(name, dtype, elements, command) \
b32 beamformer_push_##name (dtype *data, u32 count, i32 timeout_ms) { \
	b32 result = count <= ARRAY_COUNT(g_bp->name);                                           \
	if (result) {                                                                            \
		result = beamformer_upload_buffer(data, count * elements * sizeof(dtype),        \
		                                  offsetof(BeamformerSharedMemory, name),        \
		                                  offsetof(BeamformerSharedMemory, name##_sync), \
		                                  BU_KIND_##command, timeout_ms);                \
	}                                                                                        \
	return result;                                                                           \
}
BEAMFORMER_UPLOAD_FNS
#undef X

b32
beamformer_push_parameters(BeamformerParameters *bp, i32 timeout_ms)
{
	b32 result = beamformer_upload_buffer(bp, sizeof(*bp),
	                                      offsetof(BeamformerSharedMemory, parameters),
	                                      offsetof(BeamformerSharedMemory, parameters_sync),
	                                      BU_KIND_PARAMETERS, timeout_ms);
	return result;
}

b32
beamformer_push_data(void *data, u32 data_size, i32 timeout_ms)
{
	b32 result = data_size <= BEAMFORMER_MAX_RF_DATA_SIZE;
	if (result) {
		result = beamformer_upload_buffer(data, data_size, BEAMFORMER_RF_DATA_OFF,
		                                  offsetof(BeamformerSharedMemory, raw_data_sync),
		                                  BU_KIND_RF_DATA, timeout_ms);
	}
	return result;
}

b32
beamformer_push_parameters_ui(BeamformerUIParameters *bp, i32 timeout_ms)
{
	b32 result = check_shared_memory();
	if (result) {
		BeamformWork *work = beamform_work_queue_push(&g_bp->external_work_queue);
		result = work && try_wait_sync(&g_bp->parameters_ui_sync, timeout_ms, os_wait_on_value);
		if (result) {
			BeamformerUploadContext *uc = &work->upload_context;
			uc->shared_memory_offset = offsetof(BeamformerSharedMemory, parameters);
			uc->size = sizeof(g_bp->parameters);
			uc->kind = BU_KIND_PARAMETERS;
			work->type = BW_UPLOAD_BUFFER;
			work->completion_barrier = offsetof(BeamformerSharedMemory, parameters_ui_sync);
			mem_copy(&g_bp->parameters_ui, bp, sizeof(*bp));
			beamform_work_queue_push_commit(&g_bp->external_work_queue);
		}
	}
	return result;
}

b32
beamformer_push_parameters_head(BeamformerParametersHead *bp, i32 timeout_ms)
{
	b32 result = check_shared_memory();
	if (result) {
		BeamformWork *work = beamform_work_queue_push(&g_bp->external_work_queue);
		result = work && try_wait_sync(&g_bp->parameters_head_sync, timeout_ms, os_wait_on_value);
		if (result) {
			BeamformerUploadContext *uc = &work->upload_context;
			uc->shared_memory_offset = offsetof(BeamformerSharedMemory, parameters);
			uc->size = sizeof(g_bp->parameters);
			uc->kind = BU_KIND_PARAMETERS;
			work->type = BW_UPLOAD_BUFFER;
			work->completion_barrier = offsetof(BeamformerSharedMemory, parameters_head_sync);
			mem_copy(&g_bp->parameters_head, bp, sizeof(*bp));
			beamform_work_queue_push_commit(&g_bp->external_work_queue);
		}
	}
	return result;
}

b32
set_beamformer_parameters(BeamformerParametersV0 *new_bp)
{
	b32 result = 0;
	result |= beamformer_push_channel_mapping((i16 *)new_bp->channel_mapping,
	                                          countof(new_bp->channel_mapping), 0);
	result |= beamformer_push_sparse_elements((i16 *)new_bp->uforces_channels,
	                                          countof(new_bp->uforces_channels), 0);
	v2 focal_vectors[256];
	for (u32 i = 0; i < ARRAY_COUNT(focal_vectors); i++)
		focal_vectors[i] = (v2){{new_bp->transmit_angles[i], new_bp->focal_depths[i]}};
	result |= beamformer_push_focal_vectors((f32 *)focal_vectors, countof(focal_vectors), 0);
	result |= beamformer_push_parameters((BeamformerParameters *)&new_bp->xdc_transform, 0);
	return result;
}

b32
send_data(void *data, u32 data_size)
{
	b32 result = beamformer_push_data(data, data_size, 0);
	if (result) {
		if (beamformer_start_compute(0)) {
			/* TODO(rnp): should we just set timeout on acquiring the lock instead of this? */
			try_wait_sync(&g_bp->raw_data_sync, -1, os_wait_on_value);
			atomic_store(&g_bp->raw_data_sync, 1);
		} else {
			result = 0;
			/* TODO(rnp): HACK: this is strictly meant for matlab; we need a real
			 * recovery method. for most (all?) old api uses this won't be hit */
			//warning_msg("failed to start compute after sending data\n"
			//            "library in a borked state\n"
			//            "try calling beamformer_start_compute()");
		}
	}
	return result;
}

b32
beamform_data_synchronized(void *data, u32 data_size, u32 output_points[3], f32 *out_data, i32 timeout_ms)
{
	if (!check_shared_memory())
		return 0;

	output_points[0] = MIN(1, output_points[0]);
	output_points[1] = MIN(1, output_points[1]);
	output_points[2] = MIN(1, output_points[2]);

	g_bp->parameters.output_points[0] = output_points[0];
	g_bp->parameters.output_points[1] = output_points[1];
	g_bp->parameters.output_points[2] = output_points[2];
	g_bp->export_next_frame = 1;

	Pipe export_pipe = os_open_read_pipe(OS_EXPORT_PIPE_NAME);
	if (export_pipe.file == INVALID_FILE) {
		//error_msg("failed to open export pipe");
		return 0;
	}

	b32 result = send_data(data, data_size);
	if (result) {
		iz output_size = output_points[0] * output_points[1] * output_points[2] * sizeof(f32) * 2;
		result = os_wait_read_pipe(export_pipe, out_data, output_size, timeout_ms);
		//if (!result)
		//	warning_msg("failed to read full export data from pipe");
	}

	os_disconnect_pipe(export_pipe);
	os_close_pipe(&export_pipe.file, export_pipe.name);

	return result;
}
