// alloc8/src/platform/windows/win_wrapper_detours.cpp
// Windows allocator interposition using Microsoft Detours
//
// Reference: Hoard winwrapper-detours.cpp by Emery Berger

#ifndef _WIN32
#error "This file is for Windows only"
#endif

#include <alloc8/alloc8.h>

#include <windows.h>
#include <errno.h>
#include <psapi.h>
#include <stdio.h>
#include <tchar.h>
#include <new>

// Microsoft Detours header
#include <detours.h>

#pragma comment(lib, "psapi.lib")

// ─── FORWARD DECLARATIONS ─────────────────────────────────────────────────────

extern "C" {
  void* xxmalloc(size_t);
  void  xxfree(void*);
  size_t xxmalloc_usable_size(void*);
  void xxmalloc_lock();
  void xxmalloc_unlock();
  void* xxrealloc(void*, size_t);
  void* xxcalloc(size_t, size_t);
}

// ─── FOREIGN POINTER HANDLING ─────────────────────────────────────────────────
//
// When alloc8 is injected via Detours, the target program may have allocated
// memory BEFORE our hooks were installed. These "foreign" pointers must be
// handled gracefully to avoid crashes.
//
// We use Windows SEH to safely check if a pointer belongs to our allocator.

static size_t SafeGetAllocSize(void* ptr) {
  if (!ptr) return 0;
  __try {
    return xxmalloc_usable_size(ptr);
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
              EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    return 0;  // Foreign pointer
  }
}

static inline bool IsOurPointer(void* ptr) {
  return SafeGetAllocSize(ptr) > 0;
}

// ─── ORIGINAL FUNCTION POINTERS (TRAMPOLINES) ─────────────────────────────────

// Standard C allocation functions
static void* (__cdecl* Real_malloc)(size_t) = nullptr;
static void  (__cdecl* Real_free)(void*) = nullptr;
static void* (__cdecl* Real_calloc)(size_t, size_t) = nullptr;
static void* (__cdecl* Real_realloc)(void*, size_t) = nullptr;
static size_t(__cdecl* Real_msize)(void*) = nullptr;
static void* (__cdecl* Real_expand)(void*, size_t) = nullptr;
static void* (__cdecl* Real_recalloc)(void*, size_t, size_t) = nullptr;
static char* (__cdecl* Real_strdup)(const char*) = nullptr;

// CRT internal variants
static void* (__cdecl* Real_malloc_base)(size_t) = nullptr;
static void* (__cdecl* Real_malloc_crt)(size_t) = nullptr;
static void  (__cdecl* Real_free_base)(void*) = nullptr;
static void  (__cdecl* Real_free_crt)(void*) = nullptr;
static void* (__cdecl* Real_realloc_base)(void*, size_t) = nullptr;
static void* (__cdecl* Real_realloc_crt)(void*, size_t) = nullptr;
static void* (__cdecl* Real_calloc_base)(size_t, size_t) = nullptr;
static void* (__cdecl* Real_calloc_crt)(size_t, size_t) = nullptr;

// Debug CRT functions
static void* (__cdecl* Real_malloc_dbg)(size_t, int, const char*, int) = nullptr;
static void  (__cdecl* Real_free_dbg)(void*, int) = nullptr;
static void* (__cdecl* Real_realloc_dbg)(void*, size_t, int, const char*, int) = nullptr;
static void* (__cdecl* Real_calloc_dbg)(size_t, size_t, int, const char*, int) = nullptr;
static size_t(__cdecl* Real_msize_dbg)(void*, int) = nullptr;

// C++ operators - 64-bit mangled names
static void* (WINAPI* Real_new_64)(size_t) = nullptr;
static void* (WINAPI* Real_new_array_64)(size_t) = nullptr;
static void  (WINAPI* Real_delete_64)(void*) = nullptr;
static void  (WINAPI* Real_delete_array_64)(void*) = nullptr;

// C++ operators - 32-bit mangled names
static void* (WINAPI* Real_new_32)(size_t) = nullptr;
static void* (WINAPI* Real_new_array_32)(size_t) = nullptr;
static void  (WINAPI* Real_delete_32)(void*) = nullptr;
static void  (WINAPI* Real_delete_array_32)(void*) = nullptr;

// ─── DETOUR REPLACEMENT FUNCTIONS ─────────────────────────────────────────────

static void* __cdecl Detour_malloc(size_t sz) {
  return xxmalloc(sz);
}

static void __cdecl Detour_free(void* ptr) {
  if (!ptr) return;
  // Only free our pointers - silently drop foreign pointers
  if (IsOurPointer(ptr)) {
    xxfree(ptr);
  }
}

static void* __cdecl Detour_calloc(size_t num, size_t size) {
  return xxcalloc(num, size);
}

static void* __cdecl Detour_realloc(void* ptr, size_t sz) {
  if (!ptr) {
    return xxmalloc(sz);
  }
  if (sz == 0) {
    if (IsOurPointer(ptr)) {
      xxfree(ptr);
    }
    return xxmalloc(1);
  }

  size_t originalSize = SafeGetAllocSize(ptr);

  // Foreign pointer: allocate new memory and copy
  if (originalSize == 0) {
    void* buf = xxmalloc(sz);
    if (buf) {
      memcpy(buf, ptr, sz);  // Best effort copy
      // Don't free foreign pointer
    }
    return buf;
  }

  // Don't reallocate if shrinking by less than half
  if ((originalSize / 2 < sz) && (sz <= originalSize)) {
    return ptr;
  }

  void* buf = xxmalloc(sz);
  if (buf) {
    size_t minSize = (originalSize < sz) ? originalSize : sz;
    memcpy(buf, ptr, minSize);
    xxfree(ptr);
  }
  return buf;
}

static size_t __cdecl Detour_msize(void* ptr) {
  return SafeGetAllocSize(ptr);
}

static void* __cdecl Detour_expand(void*, size_t) {
  // _expand cannot be supported - requires in-place expansion
  return nullptr;
}

static void* __cdecl Detour_recalloc(void* memblock, size_t num, size_t size) {
  size_t requestedSize = num * size;
  void* ptr = Detour_realloc(memblock, requestedSize);
  if (ptr) {
    size_t actualSize = SafeGetAllocSize(ptr);
    if (actualSize > requestedSize) {
      memset(static_cast<char*>(ptr) + requestedSize, 0, actualSize - requestedSize);
    }
  }
  return ptr;
}

static char* __cdecl Detour_strdup(const char* s) {
  if (!s) return nullptr;
  size_t len = strlen(s) + 1;
  char* newStr = (char*)xxmalloc(len);
  if (newStr) {
    memcpy(newStr, s, len);
  }
  return newStr;
}

// Debug variants
static void* __cdecl Detour_malloc_dbg(size_t size, int, const char*, int) {
  return xxmalloc(size);
}

static void __cdecl Detour_free_dbg(void* ptr, int) {
  Detour_free(ptr);
}

static void* __cdecl Detour_realloc_dbg(void* ptr, size_t sz, int, const char*, int) {
  return Detour_realloc(ptr, sz);
}

static void* __cdecl Detour_calloc_dbg(size_t num, size_t size, int, const char*, int) {
  return Detour_calloc(num, size);
}

static size_t __cdecl Detour_msize_dbg(void* ptr, int) {
  return Detour_msize(ptr);
}

// ─── DETOUR ENTRY STRUCTURE ───────────────────────────────────────────────────

struct DetourEntry {
  const char* name;
  void** ppOriginal;
  void* pDetour;
  bool attached;
};

static bool AttachDetour(HMODULE hModule, DetourEntry* entry) {
  FARPROC proc = GetProcAddress(hModule, entry->name);
  if (!proc) return false;

  *entry->ppOriginal = (void*)proc;

  LONG error = DetourAttach(entry->ppOriginal, entry->pDetour);
  if (error == NO_ERROR) {
    entry->attached = true;
    return true;
  }
  return false;
}

static void DetachDetour(DetourEntry* entry) {
  if (entry->attached && *entry->ppOriginal) {
    DetourDetach(entry->ppOriginal, entry->pDetour);
    entry->attached = false;
  }
}

// ─── DETOUR ENTRIES ───────────────────────────────────────────────────────────

#define DETOUR_ENTRY(name, detour) { #name, (void**)&Real_##name, (void*)detour, false }
#define DETOUR_ENTRY_MANGLED(mangledName, realPtr, detour) { mangledName, (void**)&realPtr, (void*)detour, false }

static DetourEntry g_CRTDetours[] = {
  // Standard C allocation
  DETOUR_ENTRY(malloc, Detour_malloc),
  DETOUR_ENTRY(free, Detour_free),
  DETOUR_ENTRY(calloc, Detour_calloc),
  DETOUR_ENTRY(realloc, Detour_realloc),
  DETOUR_ENTRY_MANGLED("_msize", Real_msize, Detour_msize),
  DETOUR_ENTRY_MANGLED("_expand", Real_expand, Detour_expand),
  DETOUR_ENTRY_MANGLED("_recalloc", Real_recalloc, Detour_recalloc),
  DETOUR_ENTRY(strdup, Detour_strdup),

  // CRT internal variants
  DETOUR_ENTRY_MANGLED("_malloc_base", Real_malloc_base, Detour_malloc),
  DETOUR_ENTRY_MANGLED("_malloc_crt", Real_malloc_crt, Detour_malloc),
  DETOUR_ENTRY_MANGLED("_free_base", Real_free_base, Detour_free),
  DETOUR_ENTRY_MANGLED("_free_crt", Real_free_crt, Detour_free),
  DETOUR_ENTRY_MANGLED("_realloc_base", Real_realloc_base, Detour_realloc),
  DETOUR_ENTRY_MANGLED("_realloc_crt", Real_realloc_crt, Detour_realloc),
  DETOUR_ENTRY_MANGLED("_calloc_base", Real_calloc_base, Detour_calloc),
  DETOUR_ENTRY_MANGLED("_calloc_crt", Real_calloc_crt, Detour_calloc),

  // Debug CRT
  DETOUR_ENTRY_MANGLED("_malloc_dbg", Real_malloc_dbg, Detour_malloc_dbg),
  DETOUR_ENTRY_MANGLED("_free_dbg", Real_free_dbg, Detour_free_dbg),
  DETOUR_ENTRY_MANGLED("_realloc_dbg", Real_realloc_dbg, Detour_realloc_dbg),
  DETOUR_ENTRY_MANGLED("_calloc_dbg", Real_calloc_dbg, Detour_calloc_dbg),
  DETOUR_ENTRY_MANGLED("_msize_dbg", Real_msize_dbg, Detour_msize_dbg),

  // C++ operators - 64-bit
  DETOUR_ENTRY_MANGLED("??2@YAPEAX_K@Z", Real_new_64, Detour_malloc),
  DETOUR_ENTRY_MANGLED("??_U@YAPEAX_K@Z", Real_new_array_64, Detour_malloc),
  DETOUR_ENTRY_MANGLED("??3@YAXPEAX@Z", Real_delete_64, Detour_free),
  DETOUR_ENTRY_MANGLED("??_V@YAXPEAX@Z", Real_delete_array_64, Detour_free),

  // C++ operators - 32-bit
  DETOUR_ENTRY_MANGLED("??2@YAPAXI@Z", Real_new_32, Detour_malloc),
  DETOUR_ENTRY_MANGLED("??_U@YAPAXI@Z", Real_new_array_32, Detour_malloc),
  DETOUR_ENTRY_MANGLED("??3@YAXPAX@Z", Real_delete_32, Detour_free),
  DETOUR_ENTRY_MANGLED("??_V@YAXPAX@Z", Real_delete_array_32, Detour_free),
};

// ─── INSTALL/REMOVE DETOURS ───────────────────────────────────────────────────

static bool InstallDetours() {
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());

  bool anyAttached = false;

  // Enumerate all loaded modules and attach CRT detours
  HANDLE hProcess = GetCurrentProcess();
  DWORD cbNeeded;
  const DWORD MaxModules = 8192;
  HMODULE hMods[MaxModules];

  if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
    for (DWORD i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
      TCHAR szModName[MAX_PATH] = { 0 };
      if (GetModuleFileName(hMods[i], szModName, MAX_PATH)) {
        // Patch CRT and C++ runtime libraries
        if (!(_tcsstr(szModName, _T("CRT")) || _tcsstr(szModName, _T("crt")) ||
              _tcsstr(szModName, _T("ucrt")) || _tcsstr(szModName, _T("UCRT")) ||
              _tcsstr(szModName, _T("msvcr")) || _tcsstr(szModName, _T("MSVCR")) ||
              _tcsstr(szModName, _T("msvcp")) || _tcsstr(szModName, _T("MSVCP")) ||
              _tcsstr(szModName, _T("vcruntime")) || _tcsstr(szModName, _T("VCRUNTIME")))) {
          continue;
        }

        HMODULE hCRT = hMods[i];
        for (size_t j = 0; j < sizeof(g_CRTDetours) / sizeof(g_CRTDetours[0]); j++) {
          if (AttachDetour(hCRT, &g_CRTDetours[j])) {
            anyAttached = true;
          }
        }
      }
    }
  }

  LONG error = DetourTransactionCommit();
  if (error != NO_ERROR) {
    DetourTransactionAbort();
    return false;
  }

  return anyAttached;
}

static void RemoveDetours() {
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());

  for (size_t i = 0; i < sizeof(g_CRTDetours) / sizeof(g_CRTDetours[0]); i++) {
    DetachDetour(&g_CRTDetours[i]);
  }

  DetourTransactionCommit();
}

// ─── PUBLIC API ───────────────────────────────────────────────────────────────

extern "C" __declspec(dllexport) void InitializeAlloc8() {
  // Must call for withdll.exe injection
  DetourRestoreAfterWith();

  // Ensure Windows heap is initialized first
  HeapAlloc(GetProcessHeap(), 0, 1);

  // Install detours
  InstallDetours();
}

extern "C" __declspec(dllexport) void FinalizeAlloc8() {
  // Don't remove detours during exit - causes issues
}

// ─── DLL ENTRY POINT ──────────────────────────────────────────────────────────
// Define ALLOC8_NO_DLLMAIN if you provide your own DllMain that calls InitializeAlloc8()

#ifndef ALLOC8_NO_DLLMAIN
BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
      DisableThreadLibraryCalls(hinstDLL);
      InitializeAlloc8();
      break;

    case DLL_PROCESS_DETACH:
      // Don't call FinalizeAlloc8 - let process exit naturally
      break;
  }
  return TRUE;
}
#endif // ALLOC8_NO_DLLMAIN

