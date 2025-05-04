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

/* NOTE: discriminated union based on type */
typedef struct {
	union {
		BeamformComputeFrame       *frame;
		BeamformerUploadContext     upload_context;
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
	/* NOTE(rnp): interleaved transmit angle, focal depth pairs */
	_Alignas(64) v2 focal_vectors[256];

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

	ComputeShaderID compute_stages[MAX_COMPUTE_SHADER_STAGES];
	u32             compute_stages_count;

	i32 parameters_sync;
	i32 parameters_head_sync;
	i32 parameters_ui_sync;
	i32 focal_vectors_sync;
	i32 channel_mapping_sync;
	i32 sparse_elements_sync;
	i32 raw_data_sync;

	i32           dispatch_compute_sync;
	ImagePlaneTag current_image_plane;

	/* TODO(rnp): these shouldn't be needed */
	b32 export_next_frame;

	BeamformWorkQueue external_work_queue;
} BeamformerSharedMemory;

#endif /* _BEAMFORMER_WORK_QUEUE_H_ */
