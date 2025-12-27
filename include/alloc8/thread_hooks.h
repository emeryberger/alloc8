// alloc8/include/alloc8/thread_hooks.h
// Optional thread lifecycle hooks for thread-aware allocators
//
// Allocators that need per-thread state (TLABs, thread-local heaps) can
// implement these hooks to be notified of thread creation/destruction.

#ifndef ALLOC8_THREAD_HOOKS_H
#define ALLOC8_THREAD_HOOKS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * xxthread_init - Called when a new thread starts
 *
 * This function is called in the context of a newly created thread,
 * before the thread's user function runs. Use this to:
 * - Initialize per-thread heap structures (TLABs)
 * - Assign the thread to a heap from a thread pool
 * - Set up thread-local caches
 *
 * This function is OPTIONAL. If not provided by the allocator,
 * alloc8 will not interpose pthread_create.
 *
 * Note: malloc/free are fully operational when this is called.
 */
void xxthread_init(void);

/**
 * xxthread_cleanup - Called when a thread is about to exit
 *
 * This function is called just before a thread exits (via pthread_exit
 * or return from the thread function). Use this to:
 * - Flush thread-local allocation buffers
 * - Return per-thread heap to the pool
 * - Process any delayed cross-thread frees
 *
 * This function is OPTIONAL. If not provided by the allocator,
 * alloc8 will not interpose pthread_exit.
 *
 * Note: This is called before thread-local storage is destroyed.
 */
void xxthread_cleanup(void);

/**
 * xxthread_created_flag - Global flag set when first thread is created
 *
 * Allocators can use this for lock optimization: when false, the program
 * is single-threaded and locks can be skipped.
 *
 * This is OPTIONAL. If not provided, alloc8 maintains its own internal flag.
 *
 * Usage:
 *   volatile bool anyThreadCreated = false;
 *   // In allocator:
 *   if (anyThreadCreated) { acquire_lock(); }
 */
extern volatile int xxthread_created_flag;

#ifdef __cplusplus
}
#endif

#endif // ALLOC8_THREAD_HOOKS_H
