// fork() must lock libSystem's own malloc zones, not just the interposed
// allocator. alloc8 interposes libmalloc's _malloc_fork_prepare/parent/child
// to lock the user allocator at the right point in fork(); those
// replacements must chain to libmalloc's originals, or a thread inside a
// system zone (still used for pre-takeover pointers and by the allocator's
// own internal calls) at fork time leaves the child with that zone's lock
// held and no owner -- the child's next allocation from the zone hangs.
//
// A worker thread hammers a system zone through its function pointers
// (bypassing interposition); the main thread forks repeatedly and each child
// allocates from the same zone. A watchdog turns a hang into a failure.
#include <malloc/malloc.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static malloc_zone_t* sysz;
static atomic_int stop;

static void* hammer(void* arg) {
  (void)arg;
  void* keep[64] = {0};
  unsigned i = 0;
  while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
    unsigned k = i++ & 63;
    if (keep[k]) sysz->free(sysz, keep[k]);
    keep[k] = sysz->malloc(sysz, 16 + (i * 37) % 4000);
  }
  for (unsigned k = 0; k < 64; k++)
    if (keep[k]) sysz->free(sysz, keep[k]);
  return NULL;
}

static malloc_zone_t* find_system_zone(void) {
  vm_address_t* zones = NULL;
  unsigned n = 0;
  if (malloc_get_all_zones(mach_task_self(), NULL, &zones, &n) != KERN_SUCCESS)
    return NULL;
  // Prefer the scalable helper zone (has real locks); fall back to the
  // system default zone.
  malloc_zone_t* fallback = NULL;
  for (unsigned i = 0; i < n; i++) {
    malloc_zone_t* z = (malloc_zone_t*)zones[i];
    const char* name = malloc_get_zone_name(z);
    if (!name) continue;
    if (strcmp(name, "MallocHelperZone") == 0) return z;
    if (strcmp(name, "DefaultMallocZone") == 0) fallback = z;
  }
  return fallback;
}

// Deterministic case: a thread holds the zone's lock (as an in-flight
// allocation would) when another thread calls fork(). With libmalloc's fork
// hooks in the chain, fork() waits for the lock and the child gets a usable
// zone; without them the child inherits it locked.
static void* hold_lock(void* arg) {
  (void)arg;
  sysz->introspect->force_lock(sysz);
  usleep(200 * 1000);
  sysz->introspect->force_unlock(sysz);
  return NULL;
}

static int fork_while_locked(void) {
  pthread_t h;
  pthread_create(&h, NULL, hold_lock, NULL);
  usleep(50 * 1000);
  pid_t pid = fork();
  if (pid == 0) {
    alarm(5);
    void* p = sysz->malloc(sysz, 1000);
    sysz->free(sysz, p);
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  pthread_join(h, NULL);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(void) {
  sysz = find_system_zone();
  if (!sysz) {
    printf("SKIP: no system zone found\n");
    return 0;
  }
  printf("system zone: %s\n", malloc_get_zone_name(sysz));

  if (!fork_while_locked()) {
    printf("FAIL: child hung on a system zone locked across fork()\n");
    return 1;
  }

  pthread_t t[2];
  for (int i = 0; i < 2; i++) pthread_create(&t[i], NULL, hammer, NULL);

  int hangs = 0;
  const int kForks = 200;
  for (int f = 0; f < kForks; f++) {
    pid_t pid = fork();
    if (pid == 0) {
      alarm(5);  // a hang in the child becomes SIGALRM
      for (int j = 0; j < 100; j++) {
        void* p = sysz->malloc(sysz, 16 + j * 40);
        sysz->free(sysz, p);
      }
      void* q = malloc(100);  // and the interposed allocator
      free(q);
      _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) hangs++;
  }
  atomic_store(&stop, 1);
  for (int i = 0; i < 2; i++) pthread_join(t[i], NULL);

  if (hangs) {
    printf("FAIL: %d of %d forked children hung or crashed\n", hangs, kForks);
    return 1;
  }
  printf("PASS: %d forks\n", kForks);
  return 0;
}
