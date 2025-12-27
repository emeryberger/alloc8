// alloc8/src/platform/macos/mac_interpose.h
// macOS DYLD interposition via __DATA,__interpose section
//
// Reference: Heap-Layers macinterpose.h by Emery Berger

#ifndef ALLOC8_MAC_INTERPOSE_H
#define ALLOC8_MAC_INTERPOSE_H

#ifndef __APPLE__
#error "This file is for macOS only"
#endif

// Interposition data structure - pairs of function pointers
typedef struct alloc8_interpose_s {
  void* replacement;  // New function to call
  void* original;     // Original function being replaced
} alloc8_interpose_t;

/**
 * MAC_INTERPOSE: Create an interposition entry.
 *
 * Places the entry in the __DATA,__interpose section which dyld reads
 * at library load time to redirect function calls.
 *
 * Usage:
 *   MAC_INTERPOSE(my_malloc, malloc);
 *   // Now calls to malloc() will go to my_malloc()
 */
#define MAC_INTERPOSE(replacement, original) \
  __attribute__((used)) \
  static const alloc8_interpose_t alloc8_interpose_##replacement##_##original \
  __attribute__((section("__DATA, __interpose"))) = { \
    (void*)replacement, \
    (void*)original \
  }

#endif // ALLOC8_MAC_INTERPOSE_H
