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

#include <detours.h>

#pragma comment(lib, "psapi.lib")

extern "C" {
  void* xxmalloc(size_t);
  void  xxfree(void*);
  size_t xxmalloc_usable_size(void*);
  void xxmalloc_lock();
  void xxmalloc_unlock();
  void* xxrealloc(void*, size_t);
  void* xxcalloc(size_t, size_t);
  void  xxfree_sized(void*, size_t);
  void  xxfree_aligned_sized(void*, size_t, size_t);
}

static volatile bool g_initComplete = false;

#if defined(ALLOC8_XXOWNS_INLINE_HEADER)
#include ALLOC8_XXOWNS_INLINE_HEADER
static inline bool IsOurPointer(void* ptr) {
  if (!ptr) return false;
  return alloc8_xxowns_inline(ptr);
}
static size_t SafeGetAllocSize(void* ptr) {
  if (!IsOurPointer(ptr)) return 0;
  return xxmalloc_usable_size(ptr);
}
#else
static size_t SafeGetAllocSize(void* ptr) {
  if (!ptr) return 0;
  __try { return xxmalloc_usable_size(ptr); }
  __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return 0; }
}
static inline bool IsOurPointer(void* ptr) {
  if (g_initComplete) return true;
  return SafeGetAllocSize(ptr) > 0;
}
#endif

static void* (__cdecl* Real_malloc)(size_t) = nullptr;
static void  (__cdecl* Real_free)(void*) = nullptr;
static void* (__cdecl* Real_calloc)(size_t, size_t) = nullptr;
static void* (__cdecl* Real_realloc)(void*, size_t) = nullptr;
static size_t(__cdecl* Real_msize)(void*) = nullptr;
static void* (__cdecl* Real_expand)(void*, size_t) = nullptr;
static void* (__cdecl* Real_recalloc)(void*, size_t, size_t) = nullptr;
static char* (__cdecl* Real_strdup)(const char*) = nullptr;
static void* (__cdecl* Real_malloc_base)(size_t) = nullptr;
static void* (__cdecl* Real_malloc_crt)(size_t) = nullptr;
static void  (__cdecl* Real_free_base)(void*) = nullptr;
static void  (__cdecl* Real_free_crt)(void*) = nullptr;
static void* (__cdecl* Real_realloc_base)(void*, size_t) = nullptr;
static void* (__cdecl* Real_realloc_crt)(void*, size_t) = nullptr;
static void* (__cdecl* Real_calloc_base)(size_t, size_t) = nullptr;
static void* (__cdecl* Real_calloc_crt)(size_t, size_t) = nullptr;
static void* (__cdecl* Real_malloc_dbg)(size_t, int, const char*, int) = nullptr;
static void  (__cdecl* Real_free_dbg)(void*, int) = nullptr;
static void* (__cdecl* Real_realloc_dbg)(void*, size_t, int, const char*, int) = nullptr;
static void* (__cdecl* Real_calloc_dbg)(size_t, size_t, int, const char*, int) = nullptr;
static size_t(__cdecl* Real_msize_dbg)(void*, int) = nullptr;
static void* (WINAPI* Real_new_64)(size_t) = nullptr;
static void* (WINAPI* Real_new_array_64)(size_t) = nullptr;
static void  (WINAPI* Real_delete_64)(void*) = nullptr;
static void  (WINAPI* Real_delete_array_64)(void*) = nullptr;
static void* (WINAPI* Real_new_32)(size_t) = nullptr;
static void* (WINAPI* Real_new_array_32)(size_t) = nullptr;
static void  (WINAPI* Real_delete_32)(void*) = nullptr;
static void  (WINAPI* Real_delete_array_32)(void*) = nullptr;
static void (WINAPI* Real_delete_sized_64)(void*, size_t) = nullptr;
static void (WINAPI* Real_delete_array_sized_64)(void*, size_t) = nullptr;
static void (WINAPI* Real_delete_sized_32)(void*, size_t) = nullptr;
static void (WINAPI* Real_delete_array_sized_32)(void*, size_t) = nullptr;

static void* (__cdecl* Real_aligned_malloc)(size_t, size_t) = nullptr;
static void  (__cdecl* Real_aligned_free)(void*) = nullptr;
static void* (__cdecl* Real_aligned_realloc)(void*, size_t, size_t) = nullptr;
static void* (__cdecl* Real_aligned_recalloc)(void*, size_t, size_t, size_t) = nullptr;
static size_t(__cdecl* Real_aligned_msize)(void*, size_t, size_t) = nullptr;
static void* (__cdecl* Real_aligned_offset_malloc)(size_t, size_t, size_t) = nullptr;
static void* (__cdecl* Real_aligned_offset_realloc)(void*, size_t, size_t, size_t) = nullptr;
static void* (__cdecl* Real_aligned_offset_recalloc)(void*, size_t, size_t, size_t, size_t) = nullptr;
static void* (__cdecl* Real_aligned_malloc_dbg)(size_t, size_t, const char*, int) = nullptr;
static void  (__cdecl* Real_aligned_free_dbg)(void*) = nullptr;
static void* (__cdecl* Real_aligned_realloc_dbg)(void*, size_t, size_t, const char*, int) = nullptr;
static void* (__cdecl* Real_aligned_recalloc_dbg)(void*, size_t, size_t, size_t, const char*, int) = nullptr;
static size_t(__cdecl* Real_aligned_msize_dbg)(void*, size_t, size_t) = nullptr;
static void* (__cdecl* Real_aligned_offset_malloc_dbg)(size_t, size_t, size_t, const char*, int) = nullptr;
static void* (__cdecl* Real_aligned_offset_realloc_dbg)(void*, size_t, size_t, size_t, const char*, int) = nullptr;
static void* (__cdecl* Real_aligned_offset_recalloc_dbg)(void*, size_t, size_t, size_t, size_t, const char*, int) = nullptr;

static void* __cdecl Detour_malloc(size_t sz) { return xxmalloc(sz); }
static void __cdecl Detour_free(void* ptr) { if (ptr && IsOurPointer(ptr)) xxfree(ptr); }
static void* __cdecl Detour_calloc(size_t num, size_t size) { return xxcalloc(num, size); }
static void* __cdecl Detour_realloc(void* ptr, size_t sz) {
  if (!ptr) return xxmalloc(sz);
  if (sz == 0) { if (IsOurPointer(ptr)) xxfree(ptr); return xxmalloc(1); }
  size_t originalSize = SafeGetAllocSize(ptr);
  if (originalSize == 0) { void* buf = xxmalloc(sz); if (buf) memcpy(buf, ptr, sz); return buf; }
  if ((originalSize / 2 < sz) && (sz <= originalSize)) return ptr;
  void* buf = xxmalloc(sz);
  if (buf) { memcpy(buf, ptr, (originalSize < sz) ? originalSize : sz); xxfree(ptr); }
  return buf;
}
static size_t __cdecl Detour_msize(void* ptr) { return SafeGetAllocSize(ptr); }
static void* __cdecl Detour_expand(void*, size_t) { return nullptr; }
static void* __cdecl Detour_recalloc(void* memblock, size_t num, size_t size) {
  size_t requestedSize = num * size;
  void* ptr = Detour_realloc(memblock, requestedSize);
  if (ptr) { size_t actualSize = SafeGetAllocSize(ptr); if (actualSize > requestedSize) memset(static_cast<char*>(ptr) + requestedSize, 0, actualSize - requestedSize); }
  return ptr;
}
static char* __cdecl Detour_strdup(const char* s) { if (!s) return nullptr; size_t len = strlen(s) + 1; char* newStr = (char*)xxmalloc(len); if (newStr) memcpy(newStr, s, len); return newStr; }
static void __cdecl Detour_free_sized(void* ptr, size_t sz) { if (ptr && IsOurPointer(ptr)) xxfree_sized(ptr, sz); }
static void* __cdecl Detour_malloc_dbg(size_t size, int, const char*, int) { return xxmalloc(size); }
static void __cdecl Detour_free_dbg(void* ptr, int) { Detour_free(ptr); }
static void* __cdecl Detour_realloc_dbg(void* ptr, size_t sz, int, const char*, int) { return Detour_realloc(ptr, sz); }
static void* __cdecl Detour_calloc_dbg(size_t num, size_t size, int, const char*, int) { return Detour_calloc(num, size); }
static size_t __cdecl Detour_msize_dbg(void* ptr, int) { return Detour_msize(ptr); }

static inline bool IsPowerOfTwo(size_t x) { return x && !(x & (x - 1)); }
static void* AlignedOffsetAlloc(size_t size, size_t alignment, size_t offset) {
  if (!IsPowerOfTwo(alignment) || alignment < sizeof(void*)) return nullptr;
  if (size == 0) size = 1;
  size_t extra = alignment + sizeof(void*);
  void* base = xxmalloc(size + extra);
  if (!base) return nullptr;
  uintptr_t baseAddr = reinterpret_cast<uintptr_t>(base);
  uintptr_t alignedAddr = (baseAddr + sizeof(void*) + alignment - 1 + offset) & ~(alignment - 1);
  alignedAddr -= offset;
  if (alignedAddr < baseAddr + sizeof(void*)) alignedAddr += alignment;
  void* aligned = reinterpret_cast<void*>(alignedAddr);
  reinterpret_cast<void**>(aligned)[-1] = base;
  return aligned;
}
static inline void* AlignedBase(void* ptr) { return ptr ? reinterpret_cast<void**>(ptr)[-1] : nullptr; }
static size_t AlignedUsableSize(void* ptr, size_t, size_t) {
  if (!ptr) return 0;
  void* base = AlignedBase(ptr);
  if (!base) return 0;
  size_t baseSize = xxmalloc_usable_size(base);
  if (baseSize == 0) return 0;
  size_t overhead = static_cast<size_t>(reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(base));
  return (baseSize > overhead) ? (baseSize - overhead) : 0;
}
static inline bool IsOurAlignedPointer(void* ptr) { if (!ptr) return false; void* base = AlignedBase(ptr); return base && IsOurPointer(base); }

static void* __cdecl Detour_aligned_malloc(size_t size, size_t alignment) { return AlignedOffsetAlloc(size, alignment, 0); }
static void* __cdecl Detour_aligned_offset_malloc(size_t size, size_t alignment, size_t offset) { return AlignedOffsetAlloc(size, alignment, offset); }
static void __cdecl Detour_aligned_free(void* ptr) { if (ptr && IsOurAlignedPointer(ptr)) xxfree(AlignedBase(ptr)); }
static size_t __cdecl Detour_aligned_msize(void* ptr, size_t alignment, size_t offset) { return (ptr && IsOurAlignedPointer(ptr)) ? AlignedUsableSize(ptr, alignment, offset) : 0; }
static void* __cdecl Detour_aligned_realloc(void* ptr, size_t size, size_t alignment) {
  if (!ptr) return Detour_aligned_malloc(size, alignment);
  if (size == 0) { Detour_aligned_free(ptr); return Detour_aligned_malloc(1, alignment); }
  size_t oldSize = Detour_aligned_msize(ptr, alignment, 0);
  if (oldSize == 0) { void* newPtr = Detour_aligned_malloc(size, alignment); if (newPtr) memcpy(newPtr, ptr, size); return newPtr; }
  if ((oldSize / 2 < size) && (size <= oldSize)) return ptr;
  void* newPtr = Detour_aligned_malloc(size, alignment);
  if (newPtr) { memcpy(newPtr, ptr, (oldSize < size) ? oldSize : size); Detour_aligned_free(ptr); }
  return newPtr;
}
static void* __cdecl Detour_aligned_offset_realloc(void* ptr, size_t size, size_t alignment, size_t offset) {
  if (!ptr) return Detour_aligned_offset_malloc(size, alignment, offset);
  if (size == 0) { Detour_aligned_free(ptr); return Detour_aligned_offset_malloc(1, alignment, offset); }
  size_t oldSize = Detour_aligned_msize(ptr, alignment, offset);
  if (oldSize == 0) { void* newPtr = Detour_aligned_offset_malloc(size, alignment, offset); if (newPtr) memcpy(newPtr, ptr, size); return newPtr; }
  if ((oldSize / 2 < size) && (size <= oldSize)) return ptr;
  void* newPtr = Detour_aligned_offset_malloc(size, alignment, offset);
  if (newPtr) { memcpy(newPtr, ptr, (oldSize < size) ? oldSize : size); Detour_aligned_free(ptr); }
  return newPtr;
}
static void* __cdecl Detour_aligned_recalloc(void* ptr, size_t num, size_t size, size_t alignment) {
  size_t newSize = num * size, oldSize = ptr ? Detour_aligned_msize(ptr, alignment, 0) : 0;
  void* newPtr = Detour_aligned_realloc(ptr, newSize, alignment);
  if (newPtr && newSize > oldSize) memset(static_cast<char*>(newPtr) + oldSize, 0, newSize - oldSize);
  return newPtr;
}
static void* __cdecl Detour_aligned_offset_recalloc(void* ptr, size_t num, size_t size, size_t alignment, size_t offset) {
  size_t newSize = num * size, oldSize = ptr ? Detour_aligned_msize(ptr, alignment, offset) : 0;
  void* newPtr = Detour_aligned_offset_realloc(ptr, newSize, alignment, offset);
  if (newPtr && newSize > oldSize) memset(static_cast<char*>(newPtr) + oldSize, 0, newSize - oldSize);
  return newPtr;
}
static void* __cdecl Detour_aligned_malloc_dbg(size_t size, size_t alignment, const char*, int) { return Detour_aligned_malloc(size, alignment); }
static void __cdecl Detour_aligned_free_dbg(void* ptr) { Detour_aligned_free(ptr); }
static void* __cdecl Detour_aligned_realloc_dbg(void* ptr, size_t size, size_t alignment, const char*, int) { return Detour_aligned_realloc(ptr, size, alignment); }
static void* __cdecl Detour_aligned_recalloc_dbg(void* ptr, size_t num, size_t size, size_t alignment, const char*, int) { return Detour_aligned_recalloc(ptr, num, size, alignment); }
static size_t __cdecl Detour_aligned_msize_dbg(void* ptr, size_t alignment, size_t offset) { return Detour_aligned_msize(ptr, alignment, offset); }
static void* __cdecl Detour_aligned_offset_malloc_dbg(size_t size, size_t alignment, size_t offset, const char*, int) { return Detour_aligned_offset_malloc(size, alignment, offset); }
static void* __cdecl Detour_aligned_offset_realloc_dbg(void* ptr, size_t size, size_t alignment, size_t offset, const char*, int) { return Detour_aligned_offset_realloc(ptr, size, alignment, offset); }
static void* __cdecl Detour_aligned_offset_recalloc_dbg(void* ptr, size_t num, size_t size, size_t alignment, size_t offset, const char*, int) { return Detour_aligned_offset_recalloc(ptr, num, size, alignment, offset); }

struct DetourEntry { const char* name; void** ppOriginal; void* pDetour; bool attached; };
static bool AttachDetour(HMODULE hModule, DetourEntry* entry) {
  if (entry->attached) return false;
  FARPROC proc = GetProcAddress(hModule, entry->name);
  if (!proc) return false;
  *entry->ppOriginal = (void*)proc;
  LONG error = DetourAttach(entry->ppOriginal, entry->pDetour);
  if (error == NO_ERROR) { entry->attached = true; return true; }
  return false;
}
static void DetachDetour(DetourEntry* entry) { if (entry->attached && *entry->ppOriginal) { DetourDetach(entry->ppOriginal, entry->pDetour); entry->attached = false; } }

#define DETOUR_ENTRY(name, detour) { #name, (void**)&Real_##name, (void*)detour, false }
#define DETOUR_ENTRY_MANGLED(mangledName, realPtr, detour) { mangledName, (void**)&realPtr, (void*)detour, false }

static DetourEntry g_CRTDetours[] = {
  DETOUR_ENTRY(malloc, Detour_malloc), DETOUR_ENTRY(free, Detour_free), DETOUR_ENTRY(calloc, Detour_calloc), DETOUR_ENTRY(realloc, Detour_realloc),
  DETOUR_ENTRY_MANGLED("_msize", Real_msize, Detour_msize), DETOUR_ENTRY_MANGLED("_expand", Real_expand, Detour_expand),
  DETOUR_ENTRY_MANGLED("_recalloc", Real_recalloc, Detour_recalloc), DETOUR_ENTRY(strdup, Detour_strdup),
  DETOUR_ENTRY_MANGLED("_malloc_base", Real_malloc_base, Detour_malloc), DETOUR_ENTRY_MANGLED("_malloc_crt", Real_malloc_crt, Detour_malloc),
  DETOUR_ENTRY_MANGLED("_free_base", Real_free_base, Detour_free), DETOUR_ENTRY_MANGLED("_free_crt", Real_free_crt, Detour_free),
  DETOUR_ENTRY_MANGLED("_realloc_base", Real_realloc_base, Detour_realloc), DETOUR_ENTRY_MANGLED("_realloc_crt", Real_realloc_crt, Detour_realloc),
  DETOUR_ENTRY_MANGLED("_calloc_base", Real_calloc_base, Detour_calloc), DETOUR_ENTRY_MANGLED("_calloc_crt", Real_calloc_crt, Detour_calloc),
  DETOUR_ENTRY_MANGLED("_malloc_dbg", Real_malloc_dbg, Detour_malloc_dbg), DETOUR_ENTRY_MANGLED("_free_dbg", Real_free_dbg, Detour_free_dbg),
  DETOUR_ENTRY_MANGLED("_realloc_dbg", Real_realloc_dbg, Detour_realloc_dbg), DETOUR_ENTRY_MANGLED("_calloc_dbg", Real_calloc_dbg, Detour_calloc_dbg),
  DETOUR_ENTRY_MANGLED("_msize_dbg", Real_msize_dbg, Detour_msize_dbg),
  DETOUR_ENTRY_MANGLED("??2@YAPEAX_K@Z", Real_new_64, Detour_malloc), DETOUR_ENTRY_MANGLED("??_U@YAPEAX_K@Z", Real_new_array_64, Detour_malloc),
  DETOUR_ENTRY_MANGLED("??3@YAXPEAX@Z", Real_delete_64, Detour_free), DETOUR_ENTRY_MANGLED("??_V@YAXPEAX@Z", Real_delete_array_64, Detour_free),
  DETOUR_ENTRY_MANGLED("??2@YAPAXI@Z", Real_new_32, Detour_malloc), DETOUR_ENTRY_MANGLED("??_U@YAPAXI@Z", Real_new_array_32, Detour_malloc),
  DETOUR_ENTRY_MANGLED("??3@YAXPAX@Z", Real_delete_32, Detour_free), DETOUR_ENTRY_MANGLED("??_V@YAXPAX@Z", Real_delete_array_32, Detour_free),
  DETOUR_ENTRY_MANGLED("??3@YAXPEAX_K@Z", Real_delete_sized_64, Detour_free_sized), DETOUR_ENTRY_MANGLED("??_V@YAXPEAX_K@Z", Real_delete_array_sized_64, Detour_free_sized),
  DETOUR_ENTRY_MANGLED("??3@YAXPAXI@Z", Real_delete_sized_32, Detour_free_sized), DETOUR_ENTRY_MANGLED("??_V@YAXPAXI@Z", Real_delete_array_sized_32, Detour_free_sized),
  DETOUR_ENTRY_MANGLED("_aligned_malloc", Real_aligned_malloc, Detour_aligned_malloc),
  DETOUR_ENTRY_MANGLED("_aligned_free", Real_aligned_free, Detour_aligned_free),
  DETOUR_ENTRY_MANGLED("_aligned_realloc", Real_aligned_realloc, Detour_aligned_realloc),
  DETOUR_ENTRY_MANGLED("_aligned_recalloc", Real_aligned_recalloc, Detour_aligned_recalloc),
  DETOUR_ENTRY_MANGLED("_aligned_msize", Real_aligned_msize, Detour_aligned_msize),
  DETOUR_ENTRY_MANGLED("_aligned_offset_malloc", Real_aligned_offset_malloc, Detour_aligned_offset_malloc),
  DETOUR_ENTRY_MANGLED("_aligned_offset_realloc", Real_aligned_offset_realloc, Detour_aligned_offset_realloc),
  DETOUR_ENTRY_MANGLED("_aligned_offset_recalloc", Real_aligned_offset_recalloc, Detour_aligned_offset_recalloc),
  DETOUR_ENTRY_MANGLED("_aligned_malloc_dbg", Real_aligned_malloc_dbg, Detour_aligned_malloc_dbg),
  DETOUR_ENTRY_MANGLED("_aligned_free_dbg", Real_aligned_free_dbg, Detour_aligned_free_dbg),
  DETOUR_ENTRY_MANGLED("_aligned_realloc_dbg", Real_aligned_realloc_dbg, Detour_aligned_realloc_dbg),
  DETOUR_ENTRY_MANGLED("_aligned_recalloc_dbg", Real_aligned_recalloc_dbg, Detour_aligned_recalloc_dbg),
  DETOUR_ENTRY_MANGLED("_aligned_msize_dbg", Real_aligned_msize_dbg, Detour_aligned_msize_dbg),
  DETOUR_ENTRY_MANGLED("_aligned_offset_malloc_dbg", Real_aligned_offset_malloc_dbg, Detour_aligned_offset_malloc_dbg),
  DETOUR_ENTRY_MANGLED("_aligned_offset_realloc_dbg", Real_aligned_offset_realloc_dbg, Detour_aligned_offset_realloc_dbg),
  DETOUR_ENTRY_MANGLED("_aligned_offset_recalloc_dbg", Real_aligned_offset_recalloc_dbg, Detour_aligned_offset_recalloc_dbg),
};

static bool InstallDetours() {
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  bool anyAttached = false;
  HANDLE hProcess = GetCurrentProcess();
  DWORD cbNeeded;
  const DWORD MaxModules = 8192;
  HMODULE hMods[MaxModules];
  if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
    for (DWORD i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
      TCHAR szModName[MAX_PATH] = { 0 };
      if (GetModuleFileName(hMods[i], szModName, MAX_PATH)) {
        if (!(_tcsstr(szModName, _T("CRT")) || _tcsstr(szModName, _T("crt")) || _tcsstr(szModName, _T("ucrt")) || _tcsstr(szModName, _T("UCRT")) ||
              _tcsstr(szModName, _T("msvcr")) || _tcsstr(szModName, _T("MSVCR")) || _tcsstr(szModName, _T("msvcp")) || _tcsstr(szModName, _T("MSVCP")) ||
              _tcsstr(szModName, _T("vcruntime")) || _tcsstr(szModName, _T("VCRUNTIME")))) continue;
        HMODULE hCRT = hMods[i];
        for (size_t j = 0; j < sizeof(g_CRTDetours) / sizeof(g_CRTDetours[0]); j++)
          if (AttachDetour(hCRT, &g_CRTDetours[j])) anyAttached = true;
      }
    }
  }
  LONG error = DetourTransactionCommit();
  if (error != NO_ERROR) { DetourTransactionAbort(); return false; }
  return anyAttached;
}

static void RemoveDetours() {
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  for (size_t i = 0; i < sizeof(g_CRTDetours) / sizeof(g_CRTDetours[0]); i++) DetachDetour(&g_CRTDetours[i]);
  DetourTransactionCommit();
}

extern "C" __declspec(dllexport) void InitializeAlloc8() {
  DetourRestoreAfterWith();
  HeapAlloc(GetProcessHeap(), 0, 1);
  InstallDetours();
  g_initComplete = true;
}

extern "C" __declspec(dllexport) void FinalizeAlloc8() {}

#ifndef ALLOC8_NO_DLLMAIN
BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
  switch (fdwReason) {
    case DLL_PROCESS_ATTACH: DisableThreadLibraryCalls(hinstDLL); InitializeAlloc8(); break;
    case DLL_PROCESS_DETACH: break;
  }
  return TRUE;
}
#endif
