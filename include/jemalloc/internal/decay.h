#ifndef JEMALLOC_INTERNAL_DECAY_H
#define JEMALLOC_INTERNAL_DECAY_H

#include "jemalloc/internal/smoothstep.h"

/*
 * The decay_t computes the number of pages we should purge at any given time.
 * Page allocators inform a decay object when pages enter a decay-able state
 * (i.e. dirty or muzzy), and query it to determine how many pages should be
 * purged at any given time.
 *
 * This is mostly a single-threaded data structure and doesn't care about
 * synchronization at all; it's the caller's responsibility to manage their
 * synchronization on their own.  There are two exceptions:
 * 1) It's OK to racily call decay_ms_read (i.e. just the simplest state query).
 * 2) The mtx and purging fields live (and are initialized) here, but are
 *    logically owned by the page allocator.  This is just a convenience (since
 *    those fields would be duplicated for both the dirty and muzzy states
 *    otherwise).
 */
typedef struct decay_s decay_t;
struct decay_s {
	/* Synchronizes all non-atomic fields. */
	malloc_mutex_t mtx;
	/*
	 * True if a thread is currently purging the extents associated with
	 * this decay structure.
	 */
	bool purging;
	/*
	 * Approximate time in milliseconds from the creation of a set of unused
	 * dirty pages until an equivalent set of unused dirty pages is purged
	 * and/or reused.
	 */
	atomic_zd_t time_ms;
	/* time / SMOOTHSTEP_NSTEPS. */
	nstime_t interval;
	/*
	 * Time at which the current decay interval logically started.  We do
	 * not actually advance to a new epoch until sometime after it starts
	 * because of scheduling and computation delays, and it is even possible
	 * to completely skip epochs.  In all cases, during epoch advancement we
	 * merge all relevant activity into the most recently recorded epoch.
	 */
	nstime_t epoch;
	/* Deadline randomness generator. */
	uint64_t jitter_state;
	/*
	 * Deadline for current epoch.  This is the sum of interval and per
	 * epoch jitter which is a uniform random variable in [0..interval).
	 * Epochs always advance by precise multiples of interval, but we
	 * randomize the deadline to reduce the likelihood of arenas purging in
	 * lockstep.
	 */
	nstime_t deadline;
	/*
	 * The number of pages we cap ourselves at in the current epoch, per
	 * decay policies.  Updated on an epoch change.  After an epoch change,
	 * the caller should take steps to try to purge down to this amount.
	 */
	size_t npages_limit;
	/*
	 * Number of unpurged pages at beginning of current epoch.  During epoch
	 * advancement we use the delta between arena->decay_*.nunpurged and
	 * ecache_npages_get(&arena->ecache_*) to determine how many dirty pages,
	 * if any, were generated.
	 */
	size_t nunpurged;
	/*
	 * Trailing log of how many unused dirty pages were generated during
	 * each of the past SMOOTHSTEP_NSTEPS decay epochs, where the last
	 * element is the most recent epoch.  Corresponding epoch times are
	 * relative to epoch.
	 *
	 * Updated only on epoch advance, triggered by
	 * decay_maybe_advance_epoch, below.
	 */
	size_t backlog[SMOOTHSTEP_NSTEPS];

	/* Peak number of pages in associated extents.  Used for debug only. */
	uint64_t ceil_npages;
};

static inline ssize_t
decay_ms_read(const decay_t *decay) {
	return atomic_load_zd(&decay->time_ms, ATOMIC_RELAXED);
}

static inline size_t
decay_npages_limit_get(const decay_t *decay) {
	return decay->npages_limit;
}

/* How many unused dirty pages were generated during the last epoch. */
static inline size_t
decay_epoch_npages_delta(const decay_t *decay) {
	return decay->backlog[SMOOTHSTEP_NSTEPS - 1];
}

bool decay_ms_valid(ssize_t decay_ms);

/*
 * As a precondition, the decay_t must be zeroed out (as if with memset).
 *
 * Returns true on error.
 */
bool decay_init(decay_t *decay, ssize_t decay_ms);

/*
 * Given an already-initialized decay_t, reinitialize it with the given decay
 * time.  The decay_t must have previously been initialized (and should not then
 * be zeroed).
 */
void decay_reinit(decay_t *decay, ssize_t decay_ms);

/* Returns true if the epoch advanced and there are pages to purge. */
bool decay_maybe_advance_epoch(decay_t *decay, nstime_t *new_time,
    size_t current_npages);

#endif /* JEMALLOC_INTERNAL_DECAY_H */