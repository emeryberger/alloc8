// alloc8/src/platform/macos/mac_zones.cpp
// macOS malloc_zone_t implementation
//
// This file is included by mac_wrapper.cpp
// Reference: Heap-Layers macwrapper.cpp by Emery Berger

#ifndef __APPLE__
#error "This file is for macOS only"
#endif

// ─── DEFAULT ZONE ─────────────────────────────────────────────────────────────

static const char* theOneTrueZoneName = "alloc8DefaultZone";

static bool initializeZone(malloc_zone_t& zone);

static malloc_zone_t* getDefaultZone() {
  static malloc_zone_t theDefaultZone;
  static bool initialized = initializeZone(theDefaultZone);
  (void)initialized;
  return &theDefaultZone;
}

// Force zone initialization very early during library load.
// Priority 101 runs after basic C++ runtime setup (priority ~100) but before
// most other constructors. This ensures the zone is ready before dyld triggers
// any interposed malloc calls.
__attribute__((constructor(101)))
static void alloc8_early_zone_init() {
  (void)getDefaultZone();
}

// ─── ZONE FUNCTION IMPLEMENTATIONS ────────────────────────────────────────────

extern "C" {

size_t replace_internal_malloc_zone_size(malloc_zone_t*, const void* ptr) {
  return replace_malloc_usable_size((void*)ptr);
}

malloc_zone_t* replace_malloc_create_zone(vm_size_t, unsigned) {
  return getDefaultZone();
}

malloc_zone_t* replace_malloc_default_zone() {
  return getDefaultZone();
}

malloc_zone_t* replace_malloc_default_purgeable_zone() {
  return getDefaultZone();
}

void replace_malloc_destroy_zone(malloc_zone_t*) {
  // NOP - we don't actually destroy zones
}

kern_return_t replace_malloc_get_all_zones(
    task_t,
    memory_reader_t,
    vm_address_t** addresses,
    unsigned* count) {
  *addresses = nullptr;
  *count = 0;
  return KERN_SUCCESS;
}

const char* replace_malloc_get_zone_name(malloc_zone_t* zone) {
  return zone->zone_name;
}

void replace_malloc_set_zone_name(malloc_zone_t*, const char*) {
  // NOP
}

int replace_malloc_jumpstart(int) {
  return 1;
}

// ─── ZONE ALLOCATION FUNCTIONS ────────────────────────────────────────────────

void* replace_malloc_zone_malloc(malloc_zone_t*, size_t size) {
  return replace_malloc(size);
}

void* replace_malloc_zone_calloc(malloc_zone_t*, size_t count, size_t size) {
  return replace_calloc(count, size);
}

void* replace_malloc_zone_realloc(malloc_zone_t*, void* ptr, size_t size) {
  return replace_realloc(ptr, size);
}

void* replace_malloc_zone_valloc(malloc_zone_t*, size_t size) {
  return replace_valloc(size);
}

void* replace_malloc_zone_memalign(malloc_zone_t*, size_t alignment, size_t size) {
  return replace_memalign(alignment, size);
}

void replace_malloc_zone_free(malloc_zone_t*, void* ptr) {
  xxfree(ptr);
}

void replace_malloc_zone_free_definite_size(malloc_zone_t*, void* ptr, size_t) {
  xxfree(ptr);
}

// ─── ZONE BATCH OPERATIONS ────────────────────────────────────────────────────

unsigned replace_malloc_zone_batch_malloc(
    malloc_zone_t*,
    size_t size,
    void** results,
    unsigned num_requested) {
  for (unsigned i = 0; i < num_requested; i++) {
    results[i] = replace_malloc(size);
    if (!results[i]) {
      return i;
    }
  }
  return num_requested;
}

void replace_malloc_zone_batch_free(
    malloc_zone_t*,
    void** to_be_freed,
    unsigned num) {
  for (unsigned i = 0; i < num; i++) {
    xxfree(to_be_freed[i]);
  }
}

// ─── ZONE INTROSPECTION ───────────────────────────────────────────────────────

bool replace_malloc_zone_check(malloc_zone_t*) {
  return true;
}

malloc_zone_t* replace_malloc_zone_from_ptr(const void*) {
  return getDefaultZone();
}

void replace_malloc_zone_log(malloc_zone_t*, void*) {
  // NOP
}

void replace_malloc_zone_print(malloc_zone_t*, bool) {
  // NOP
}

void replace_malloc_zone_print_ptr_info(void*) {
  // NOP
}

void replace_malloc_zone_register(malloc_zone_t*) {
  // NOP
}

void replace_malloc_zone_unregister(malloc_zone_t*) {
  // NOP
}

} // extern "C"

// ─── ZONE INITIALIZATION ──────────────────────────────────────────────────────

static bool initializeZone(malloc_zone_t& zone) {
  zone.size = replace_internal_malloc_zone_size;
  zone.malloc = replace_malloc_zone_malloc;
  zone.calloc = replace_malloc_zone_calloc;
  zone.valloc = replace_malloc_zone_valloc;
  zone.free = replace_malloc_zone_free;
  zone.realloc = replace_malloc_zone_realloc;
  zone.destroy = replace_malloc_destroy_zone;
  zone.zone_name = theOneTrueZoneName;
  zone.batch_malloc = replace_malloc_zone_batch_malloc;
  zone.batch_free = replace_malloc_zone_batch_free;
  zone.introspect = nullptr;
  zone.version = 8;
  zone.memalign = replace_malloc_zone_memalign;

#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1060
  zone.free_definite_size = replace_malloc_zone_free_definite_size;
  zone.pressure_relief = nullptr;
#endif

  return true;
}
