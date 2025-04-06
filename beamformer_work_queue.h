/* See LICENSE for license details. */
#ifndef _BEAMFORMER_WORK_QUEUE_H_
#define _BEAMFORMER_WORK_QUEUE_H_

typedef struct BeamformComputeFrame BeamformComputeFrame;
typedef struct ComputeShaderReloadContext ComputeShaderReloadContext;

typedef enum {
	BW_COMPUTE,
	BW_RELOAD_SHADER,
	BW_SAVE_FRAME,
	BW_SEND_FRAME,
	BW_UPLOAD_CHANNEL_MAPPING,
	BW_UPLOAD_FOCAL_VECTORS,
	BW_UPLOAD_PARAMS,
	BW_UPLOAD_RF_DATA,
	BW_UPLOAD_SPARSE_ELEMENTS,
} BeamformWorkType;

typedef struct {
	BeamformComputeFrame *frame;
	iptr                  file_handle;
} BeamformOutputFrameContext;

/* NOTE: discriminated union based on type */
typedef struct {
	union {
		BeamformComputeFrame       *frame;
		BeamformOutputFrameContext  output_frame_ctx;
		ComputeShaderReloadContext *reload_shader_ctx;
		void                       *generic;
	};
	/* NOTE(rnp): mostly for __external__ processes to sync on. when passed from external
	 * process this should be an offset from base of shared_memory */
	iptr completion_barrier;

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

typedef struct {
	BeamformerParameters raw;

	ComputeShaderID compute_stages[16];
	u32             compute_stages_count;

	i32   raw_data_sync;
	u32   raw_data_size;

	/* TODO(rnp): these shouldn't be needed */
	b32 upload;
	b32 start_compute;
	b32 export_next_frame;

	/* TODO(rnp): probably remove this */
	c8  export_pipe_name[256];

	u16 channel_mapping[256];
	u16 sparse_elements[256];
	v2  transmit_angles_focal_depths[256];

	BeamformWorkQueue external_work_queue;
} BeamformerSharedMemory;

#endif /* _BEAMFORMER_WORK_QUEUE_H_ */
