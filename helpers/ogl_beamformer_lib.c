/* See LICENSE for license details. */
#include "../compiler.h"

#include "../util.h"
#include "../beamformer_parameters.h"
#include "ogl_beamformer_lib_base.h"
#include "../beamformer_work_queue.c"

#define PIPE_RETRY_PERIOD_MS (100ULL)

global SharedMemoryRegion      g_shared_memory;
global BeamformerSharedMemory *g_bp;
global BeamformerLibErrorKind  g_lib_last_error;

#if OS_LINUX
#include "../os_linux.c"
#elif OS_WINDOWS
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

#if OS_LINUX

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

function SharedMemoryRegion
os_open_shared_memory_area(char *name)
{
	SharedMemoryRegion result = {0};
	i32 fd = shm_open(name, O_RDWR, S_IRUSR|S_IWUSR);
	if (fd > 0) {
		void *new = mmap(0, BEAMFORMER_SHARED_MEMORY_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
		if (new != MAP_FAILED) result.region = new;
		close(fd);
	}
	return result;
}

#elif OS_WINDOWS

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

function SharedMemoryRegion
os_open_shared_memory_area(char *name)
{
	SharedMemoryRegion result = {0};
	iptr h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, 0, name);
	if (h != INVALID_FILE) {
		void *new = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0,
		                          os_round_up_to_page_size(BEAMFORMER_SHARED_MEMORY_SIZE));
		if (new) {
			u8 buffer[1024];
			Stream sb = {.data = buffer, .cap = 1024};
			stream_append_s8s(&sb, c_str_to_s8(name), s8("_lock_"));
			local_persist iptr semaphores[BeamformerSharedMemoryLockKind_Count];
			local_persist w32_shared_memory_context ctx = {.semaphores = semaphores};
			b32 all_semaphores = 1;
			for (i32 i = 0; i < countof(semaphores); i++) {
				Stream lb = sb;
				stream_append_i64(&lb, i);
				stream_append_byte(&lb, 0);
				semaphores[i] = CreateSemaphoreA(0, 1, 1, (c8 *)lb.data);
				all_semaphores &= semaphores[i] != INVALID_FILE;
			}
			if (all_semaphores) {
				result.region     = new;
				result.os_context = (iptr)&ctx;
			}
		}
		CloseHandle(h);
	}
	return result;
}

#endif

function b32
check_shared_memory(void)
{
	b32 result = 1;
	if (!g_shared_memory.region) {
		g_shared_memory = os_open_shared_memory_area(OS_SHARED_MEMORY_NAME);
		if (!g_shared_memory.region) {
			result = 0;
			g_lib_last_error = BF_LIB_ERR_KIND_SHARED_MEMORY;
		}
	} else if (((BeamformerSharedMemory *)g_shared_memory.region)->version
	           != BEAMFORMER_SHARED_MEMORY_VERSION)
	{
		g_lib_last_error = BF_LIB_ERR_KIND_VERSION_MISMATCH;
		result = 0;
	}
	if (result) g_bp = g_shared_memory.region;
	return result;
}

function BeamformWork *
try_push_work_queue(void)
{
	BeamformWork *result = beamform_work_queue_push(&g_bp->external_work_queue);
	if (!result) g_lib_last_error = BF_LIB_ERR_KIND_WORK_QUEUE_FULL;
	return result;
}

function b32
lib_try_lock(BeamformerSharedMemoryLockKind lock, i32 timeout_ms)
{
	b32 result = os_shared_memory_region_lock(&g_shared_memory, g_bp->locks, (i32)lock, timeout_ms);
	if (!result) g_lib_last_error = BF_LIB_ERR_KIND_SYNC_VARIABLE;
	return result;
}

function void
lib_release_lock(BeamformerSharedMemoryLockKind lock)
{
	os_shared_memory_region_unlock(&g_shared_memory, g_bp->locks, (i32)lock);
}

u32
beamformer_get_api_version(void)
{
	return BEAMFORMER_SHARED_MEMORY_VERSION;
}

const char *
beamformer_error_string(BeamformerLibErrorKind kind)
{
	#define X(type, num, string) string,
	local_persist const char *error_string_table[] = {BEAMFORMER_LIB_ERRORS "invalid error kind"};
	#undef X
	return error_string_table[MIN(kind, countof(error_string_table) - 1)];
}

BeamformerLibErrorKind
beamformer_get_last_error(void)
{
	return g_lib_last_error;
}

const char *
beamformer_get_last_error_string(void)
{
	return beamformer_error_string(beamformer_get_last_error());
}

b32
set_beamformer_pipeline(i32 *stages, i32 stages_count)
{
	b32 result = 0;
	if (stages_count <= countof(g_bp->compute_stages)) {
		if (check_shared_memory()) {
			g_bp->compute_stages_count = 0;
			for (i32 i = 0; i < stages_count; i++) {
				if (BETWEEN(stages[i], 0, ComputeShaderKind_Count)) {
					g_bp->compute_stages[g_bp->compute_stages_count++] = stages[i];
				}
			}
			result = g_bp->compute_stages_count == stages_count;
			if (!result) {
				g_lib_last_error = BF_LIB_ERR_KIND_INVALID_COMPUTE_STAGE;
				g_bp->compute_stages_count = 0;
			}
		}
	} else {
		g_lib_last_error = BF_LIB_ERR_KIND_COMPUTE_STAGE_OVERFLOW;
	}
	return result;
}

b32
beamformer_start_compute(i32 timeout_ms)
{
	b32 result = 0;
	if (check_shared_memory()) {
		if (lib_try_lock(BeamformerSharedMemoryLockKind_DispatchCompute, 0)) {
			if (lib_try_lock(BeamformerSharedMemoryLockKind_DispatchCompute, timeout_ms)) {
				/* TODO(rnp): non-critical race condition */
				lib_release_lock(BeamformerSharedMemoryLockKind_DispatchCompute);
				result = 1;
			}
		}
	}
	return result;
}

function b32
beamformer_upload_buffer(void *data, u32 size, i32 store_offset, BeamformerSharedMemoryLockKind lock,
                         BeamformerUploadKind kind, i32 timeout_ms)
{
	b32 result = 0;
	if (check_shared_memory()) {
		BeamformWork *work = try_push_work_queue();
		result = work && lib_try_lock(lock, timeout_ms);
		if (result) {
			BeamformerUploadContext *uc = &work->upload_context;
			uc->shared_memory_offset = store_offset;
			uc->size = size;
			uc->kind = kind;
			work->type = BW_UPLOAD_BUFFER;
			work->lock = lock;
			mem_copy((u8 *)g_bp + store_offset, data, size);
			if ((atomic_load_u32(&g_bp->dirty_regions) & (1 << (lock - 1))) == 0) {
				atomic_or_u32(&g_bp->dirty_regions, (1 << (lock - 1)));
				beamform_work_queue_push_commit(&g_bp->external_work_queue);
			}
			lib_release_lock(lock);
		}
	}
	return result;
}

#define BEAMFORMER_UPLOAD_FNS \
	X(channel_mapping, i16, 1, ChannelMapping, CHANNEL_MAPPING) \
	X(sparse_elements, i16, 1, SparseElements, SPARSE_ELEMENTS) \
	X(focal_vectors,   f32, 2, FocalVectors,   FOCAL_VECTORS)

#define X(name, dtype, elements, lock_name, command) \
b32 beamformer_push_##name (dtype *data, u32 count, i32 timeout_ms) { \
	b32 result = 0;                                                                       \
	if (count <= countof(g_bp->name)) {                                                   \
		result = beamformer_upload_buffer(data, count * elements * sizeof(dtype),     \
		                                  offsetof(BeamformerSharedMemory, name),     \
		                                  BeamformerSharedMemoryLockKind_##lock_name, \
		                                  BU_KIND_##command, timeout_ms);             \
	} else {                                                                              \
		g_lib_last_error = BF_LIB_ERR_KIND_BUFFER_OVERFLOW;                           \
	}                                                                                     \
	return result;                                                                        \
}
BEAMFORMER_UPLOAD_FNS
#undef X

b32
beamformer_push_parameters(BeamformerParameters *bp, i32 timeout_ms)
{
	b32 result = beamformer_upload_buffer(bp, sizeof(*bp),
	                                      offsetof(BeamformerSharedMemory, parameters),
	                                      BeamformerSharedMemoryLockKind_Parameters,
	                                      BU_KIND_PARAMETERS, timeout_ms);
	return result;
}

function b32
beamformer_push_data_base(void *data, u32 data_size, i32 timeout_ms, b32 start_from_main)
{
	b32 result = 0;
	if (data_size <= BEAMFORMER_MAX_RF_DATA_SIZE) {
		result = beamformer_upload_buffer(data, data_size, BEAMFORMER_RF_DATA_OFF,
		                                  BeamformerSharedMemoryLockKind_RawData,
		                                  BU_KIND_RF_DATA, timeout_ms);
		if (result && start_from_main) atomic_store_u32(&g_bp->start_compute_from_main, 1);
	} else {
		g_lib_last_error = BF_LIB_ERR_KIND_BUFFER_OVERFLOW;
	}
	return result;
}

b32
beamformer_push_data(void *data, u32 data_size, i32 timeout_ms)
{
	return beamformer_push_data_base(data, data_size, timeout_ms, 1);
}

b32
beamformer_push_data_with_compute(void *data, u32 data_size, u32 image_plane_tag, i32 timeout_ms)
{
	b32 result = beamformer_push_data_base(data, data_size, timeout_ms, 0);
	if (result) {
		result = image_plane_tag < IPT_LAST;
		if (result) {
			BeamformWork *work = try_push_work_queue();
			result = work != 0;
			if (result) {
				work->type = BW_COMPUTE_INDIRECT;
				work->compute_indirect_plane = image_plane_tag;
				beamform_work_queue_push_commit(&g_bp->external_work_queue);
			}
		} else {
			g_lib_last_error = BF_LIB_ERR_KIND_INVALID_IMAGE_PLANE;
		}
	}
	return result;
}

b32
beamformer_push_parameters_ui(BeamformerUIParameters *bp, i32 timeout_ms)
{
	b32 result = 0;
	if (check_shared_memory()) {
		BeamformWork *work = try_push_work_queue();
		result = work && lib_try_lock(BeamformerSharedMemoryLockKind_ParametersUI, timeout_ms);
		if (result) {
			BeamformerUploadContext *uc = &work->upload_context;
			uc->shared_memory_offset = offsetof(BeamformerSharedMemory, parameters);
			uc->size = sizeof(g_bp->parameters);
			uc->kind = BU_KIND_PARAMETERS;
			work->type = BW_UPLOAD_BUFFER;
			work->lock = BeamformerSharedMemoryLockKind_ParametersUI;
			mem_copy(&g_bp->parameters_ui, bp, sizeof(*bp));
			beamform_work_queue_push_commit(&g_bp->external_work_queue);
			lib_release_lock(BeamformerSharedMemoryLockKind_ParametersUI);
		}
	}
	return result;
}

b32
beamformer_push_parameters_head(BeamformerParametersHead *bp, i32 timeout_ms)
{
	b32 result = 0;
	if (check_shared_memory()) {
		BeamformWork *work = try_push_work_queue();
		result = work && lib_try_lock(BeamformerSharedMemoryLockKind_ParametersHead, timeout_ms);
		if (result) {
			BeamformerUploadContext *uc = &work->upload_context;
			uc->shared_memory_offset = offsetof(BeamformerSharedMemory, parameters);
			uc->size = sizeof(g_bp->parameters);
			uc->kind = BU_KIND_PARAMETERS;
			work->type = BW_UPLOAD_BUFFER;
			work->lock = BeamformerSharedMemoryLockKind_ParametersHead;
			mem_copy(&g_bp->parameters_head, bp, sizeof(*bp));
			beamform_work_queue_push_commit(&g_bp->external_work_queue);
			lib_release_lock(BeamformerSharedMemoryLockKind_ParametersHead);
		}
	}
	return result;
}

b32
set_beamformer_parameters(BeamformerParametersV0 *new_bp)
{
	b32 result = 1;
	result &= beamformer_push_channel_mapping((i16 *)new_bp->channel_mapping,
	                                          countof(new_bp->channel_mapping), 0);
	result &= beamformer_push_sparse_elements((i16 *)new_bp->uforces_channels,
	                                          countof(new_bp->uforces_channels), 0);
	v2 focal_vectors[256];
	for (u32 i = 0; i < countof(focal_vectors); i++)
		focal_vectors[i] = (v2){{new_bp->transmit_angles[i], new_bp->focal_depths[i]}};
	result &= beamformer_push_focal_vectors((f32 *)focal_vectors, countof(focal_vectors), 0);
	result &= beamformer_push_parameters((BeamformerParameters *)&new_bp->xdc_transform, 0);
	return result;
}

b32
send_data(void *data, u32 data_size)
{
	b32 result = 0;
	if (beamformer_push_data(data, data_size, 0))
		result = beamformer_start_compute(-1);
	return result;
}

b32
beamform_data_synchronized(void *data, u32 data_size, u32 output_points[3], f32 *out_data, i32 timeout_ms)
{
	b32 result = 0;
	if (check_shared_memory()) {
		output_points[0] = MAX(1, output_points[0]);
		output_points[1] = MAX(1, output_points[1]);
		output_points[2] = MAX(1, output_points[2]);

		g_bp->parameters.output_points[0] = output_points[0];
		g_bp->parameters.output_points[1] = output_points[1];
		g_bp->parameters.output_points[2] = output_points[2];
		g_bp->export_next_frame = 1;

		Pipe export_pipe = os_open_read_pipe(OS_EXPORT_PIPE_NAME);
		if (export_pipe.file != INVALID_FILE) {
			if (send_data(data, data_size)) {
				iz output_size = output_points[0] * output_points[1] *
				                 output_points[2] * sizeof(f32) * 2;
				result = os_wait_read_pipe(export_pipe, out_data, output_size, timeout_ms);
				if (!result) g_lib_last_error = BF_LIB_ERR_KIND_READ_EXPORT_PIPE;
			}

			os_disconnect_pipe(export_pipe);
			os_close_pipe(&export_pipe.file, export_pipe.name);
		} else {
			g_lib_last_error = BF_LIB_ERR_KIND_OPEN_EXPORT_PIPE;
		}
	}
	return result;
}
