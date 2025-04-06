/* See LICENSE for license details. */
#include "beamformer_work_queue.h"

static BeamformWork *
beamform_work_queue_pop(BeamformWorkQueue *q)
{
	BeamformWork *result = 0;

	static_assert(ISPOWEROF2(ARRAY_COUNT(q->work_items)), "queue capacity must be a power of 2");
	u64 val  = atomic_load(&q->queue);
	u64 mask = ARRAY_COUNT(q->work_items) - 1;
	u32 widx = val       & mask;
	u32 ridx = val >> 32 & mask;

	if (ridx != widx)
		result = q->work_items + ridx;

	return result;
}

static void
beamform_work_queue_pop_commit(BeamformWorkQueue *q)
{
	atomic_add(&q->queue, 0x100000000ULL);
}

DEBUG_EXPORT BEAMFORM_WORK_QUEUE_PUSH_FN(beamform_work_queue_push)
{
	BeamformWork *result = 0;

	static_assert(ISPOWEROF2(ARRAY_COUNT(q->work_items)), "queue capacity must be a power of 2");
	u64 val  = atomic_load(&q->queue);
	u64 mask = ARRAY_COUNT(q->work_items) - 1;
	u32 widx = val       & mask;
	u32 ridx = val >> 32 & mask;
	u32 next = (widx + 1) & mask;

	if (val & 0x80000000)
		atomic_and(&q->queue, ~0x80000000);

	if (next != ridx) {
		result = q->work_items + widx;
		zero_struct(result);
	}

	return result;
}

DEBUG_EXPORT BEAMFORM_WORK_QUEUE_PUSH_COMMIT_FN(beamform_work_queue_push_commit)
{
	atomic_add(&q->queue, 1);
}
