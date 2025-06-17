/* See LICENSE for license details. */
#ifndef _BEAMFORMER_WORK_QUEUE_H_
#define _BEAMFORMER_WORK_QUEUE_H_

#define BEAMFORMER_SHARED_MEMORY_VERSION (5UL)

typedef struct BeamformComputeFrame BeamformComputeFrame;
typedef struct ShaderReloadContext  ShaderReloadContext;

typedef enum {
	BW_COMPUTE,
	BW_COMPUTE_INDIRECT,
	BW_RELOAD_SHADER,
	BW_SAVE_FRAME,
	BW_SEND_FRAME,
	BW_UPLOAD_BUFFER,
} BeamformWorkType;

typedef enum {
	BU_KIND_CHANNEL_MAPPING,
	BU_KIND_FOCAL_VECTORS,
	BU_KIND_PARAMETERS,
	BU_KIND_RF_DATA,
	BU_KIND_SPARSE_ELEMENTS,
	BU_KIND_LAST,
} BeamformerUploadKind;

typedef struct {
	i32 size;
	i32 shared_memory_offset;
	BeamformerUploadKind kind;
} BeamformerUploadContext;

typedef struct {
	BeamformComputeFrame *frame;
	iptr                  file_handle;
} BeamformOutputFrameContext;

#define BEAMFORMER_SHARED_MEMORY_LOCKS \
	X(None)            \
	X(Parameters)      \
	X(ParametersHead)  \
	X(ParametersUI)    \
	X(FocalVectors)    \
	X(ChannelMapping)  \
	X(SparseElements)  \
	X(RawData)         \
	X(DispatchCompute)

#define X(name) BeamformerSharedMemoryLockKind_##name,
typedef enum {BEAMFORMER_SHARED_MEMORY_LOCKS BeamformerSharedMemoryLockKind_Count} BeamformerSharedMemoryLockKind;
#undef X

/* NOTE: discriminated union based on type */
typedef struct {
	union {
		BeamformComputeFrame       *frame;
		BeamformerUploadContext     upload_context;
		BeamformOutputFrameContext  output_frame_ctx;
		ShaderReloadContext        *shader_reload_context;
		ImagePlaneTag               compute_indirect_plane;
		void                       *generic;
	};
	BeamformerSharedMemoryLockKind lock;

	BeamformWorkType type;
} BeamformWork;

typedef struct {
	union {
		u64 queue;
		struct {u32 widx, ridx;};
	};
	BeamformWork work_items[1 << 6];
} BeamformWorkQueue;

#define BEAMFORM_WORK_QUEUE_PUSH_FN(name) BeamformWork *name(BeamformWorkQueue *q)
typedef BEAMFORM_WORK_QUEUE_PUSH_FN(beamform_work_queue_push_fn);

#define BEAMFORM_WORK_QUEUE_PUSH_COMMIT_FN(name) void name(BeamformWorkQueue *q)
typedef BEAMFORM_WORK_QUEUE_PUSH_COMMIT_FN(beamform_work_queue_push_commit_fn);

#define BEAMFORMER_SHARED_MEMORY_SIZE (GB(2))
#define BEAMFORMER_RF_DATA_OFF        (sizeof(BeamformerSharedMemory) + 4096ULL \
                                       - (uintptr_t)(sizeof(BeamformerSharedMemory) & 4095ULL))
#define BEAMFORMER_MAX_RF_DATA_SIZE   (BEAMFORMER_SHARED_MEMORY_SIZE - BEAMFORMER_RF_DATA_OFF)

typedef align_as(64) struct {
	u32 version;

	/* NOTE(rnp): causes future library calls to fail.
	 * see note in beamformer_invalidate_shared_memory() */
	b32 invalid;

	/* NOTE(rnp): not used for locking on w32 but we can use these to peek at the status of
	 * the lock without leaving userspace. also this struct needs a bunch of padding */
	i32 locks[BeamformerSharedMemoryLockKind_Count];

	/* NOTE(rnp): used to coalesce uploads when they are not yet uploaded to the GPU */
	u32 dirty_regions;
	static_assert(BeamformerSharedMemoryLockKind_Count <= 32, "only 32 lock regions supported");

	/* NOTE(rnp): interleaved transmit angle, focal depth pairs */
	align_as(64) v2 focal_vectors[256];

	i16 channel_mapping[256];
	i16 sparse_elements[256];

	union {
		BeamformerParameters parameters;
		struct {
			BeamformerParametersHead parameters_head;
			BeamformerUIParameters   parameters_ui;
			BeamformerParametersTail parameters_tail;
		};
	};

	ComputeShaderKind compute_stages[MAX_COMPUTE_SHADER_STAGES];
	u32               compute_stages_count;

	/* TODO(rnp): hack: we need a different way of dispatching work for export */
	b32 start_compute_from_main;

	/* TODO(rnp): this shouldn't be needed */
	b32 export_next_frame;

	BeamformWorkQueue external_work_queue;
} BeamformerSharedMemory;

#endif /* _BEAMFORMER_WORK_QUEUE_H_ */
