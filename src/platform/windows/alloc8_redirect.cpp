// alloc8-redirect: Early-load DLL for malloc interposition on Windows
//
// This DLL implements a mimalloc-redirect style mechanism that patches
// the CRT's malloc/free at load time, before any allocations occur.
// This avoids the "foreign pointer" problem and reduces hooking overhead.
//
// Supported platforms: Windows x64, Windows ARM64
//
// How it works:
// 1. This DLL is loaded as a dependency of the main allocator DLL
// 2. On DLL_PROCESS_ATTACH, it patches the IAT of all loaded modules
// 3. Patched functions call back into the main allocator's xxmalloc/xxfree
// 4. The main allocator calls alloc8_redirect_init() to register its functions
// 5. Then calls alloc8_redirect_enable() to activate redirection
//
// Build (ARM64): cl /LD /O2 /MD alloc8_redirect.cpp /Fe:alloc8-redirect-arm64.dll
// Build (x64):   cl /LD /O2 /MD alloc8_redirect.cpp /Fe:alloc8-redirect.dll
//
// The PE/COFF format and IAT structure are identical on x64 and ARM64,
// so this code works on both architectures without modification.

#ifndef _WIN32
#error "This file is Windows-only"
#endif

#include <windows.h>
#include <winternl.h>

// Undocumented ntdll types and functions
extern "C" {

typedef struct _LDR_DATA_TABLE_ENTRY_FULL {
  LIST_ENTRY InLoadOrderLinks;
  LIST_ENTRY InMemoryOrderLinks;
  LIST_ENTRY InInitializationOrderLinks;
  PVOID DllBase;
  PVOID EntryPoint;
  ULONG SizeOfImage;
  UNICODE_STRING FullDllName;
  UNICODE_STRING BaseDllName;
  ULONG Flags;
  USHORT LoadCount;
  USHORT TlsIndex;
  LIST_ENTRY HashLinks;
  ULONG TimeDateStamp;
} LDR_DATA_TABLE_ENTRY_FULL;

typedef NTSTATUS (NTAPI *PFN_NtProtectVirtualMemory)(
  HANDLE ProcessHandle,
  PVOID* BaseAddress,
  SIZE_T* RegionSize,
  ULONG NewProtect,
  PULONG OldProtect
);

typedef NTSTATUS (NTAPI *PFN_LdrGetDllHandle)(
  PWSTR DllPath,
  PULONG DllCharacteristics,
  PUNICODE_STRING DllName,
  PVOID* DllHandle
);

}  // extern "C"

// Function pointers to ntdll functions
static PFN_NtProtectVirtualMemory pNtProtectVirtualMemory = nullptr;
static PFN_LdrGetDllHandle pLdrGetDllHandle = nullptr;
static HMODULE hNtdll = nullptr;

// Callbacks to the main allocator
typedef void* (*pfn_xxmalloc)(size_t);
typedef void (*pfn_xxfree)(void*);
typedef void* (*pfn_xxcalloc)(size_t, size_t);
typedef void* (*pfn_xxrealloc)(void*, size_t);
typedef size_t (*pfn_xxmalloc_usable_size)(void*);

static pfn_xxmalloc p_xxmalloc = nullptr;
static pfn_xxfree p_xxfree = nullptr;
static pfn_xxcalloc p_xxcalloc = nullptr;
static pfn_xxrealloc p_xxrealloc = nullptr;
static pfn_xxmalloc_usable_size p_xxmalloc_usable_size = nullptr;

// Original CRT functions (for cleanup/fallback)
static void* (__cdecl* orig_malloc)(size_t) = nullptr;
static void (__cdecl* orig_free)(void*) = nullptr;
static void* (__cdecl* orig_calloc)(size_t, size_t) = nullptr;
static void* (__cdecl* orig_realloc)(void*, size_t) = nullptr;
static size_t (__cdecl* orig_msize)(void*) = nullptr;

// Redirect state
static bool g_redirectEnabled = false;
static bool g_initialized = false;

// ─── PATCHING HELPERS ────────────────────────────────────────────────────────

static bool ChangeMemoryProtection(void* addr, size_t size, ULONG newProt, ULONG* oldProt) {
  if (!pNtProtectVirtualMemory) return false;

  PVOID base = addr;
  SIZE_T regionSize = size;
  NTSTATUS status = pNtProtectVirtualMemory(
    GetCurrentProcess(), &base, &regionSize, newProt, oldProt);
  return NT_SUCCESS(status);
}

// Find the address of an exported function in a module
static void* FindExport(HMODULE hMod, const char* name) {
  if (!hMod) return nullptr;
  return (void*)GetProcAddress(hMod, name);
}

// Patch an IAT entry in all loaded modules
static int PatchIATInAllModules(const char* targetDll, const char* funcName, void* newFunc, void** origFunc) {
  int patched = 0;

  // Get PEB to enumerate loaded modules
  PEB* peb = NtCurrentTeb()->ProcessEnvironmentBlock;
  if (!peb || !peb->Ldr) return 0;

  LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
  LIST_ENTRY* curr = head->Flink;

  while (curr != head) {
    LDR_DATA_TABLE_ENTRY_FULL* entry = CONTAINING_RECORD(
      curr, LDR_DATA_TABLE_ENTRY_FULL, InMemoryOrderLinks);

    HMODULE hMod = (HMODULE)entry->DllBase;
    if (hMod) {
      // Parse PE headers
      PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hMod;
      if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)hMod + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE) {
          DWORD impRVA = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
          if (impRVA != 0) {
            PIMAGE_IMPORT_DESCRIPTOR imp = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hMod + impRVA);

            for (; imp->Name != 0; imp++) {
              const char* dllName = (const char*)((BYTE*)hMod + imp->Name);
              if (_stricmp(dllName, targetDll) != 0) continue;

              PIMAGE_THUNK_DATA origThunk = (PIMAGE_THUNK_DATA)((BYTE*)hMod + imp->OriginalFirstThunk);
              PIMAGE_THUNK_DATA iatThunk = (PIMAGE_THUNK_DATA)((BYTE*)hMod + imp->FirstThunk);

              for (; origThunk->u1.AddressOfData != 0; origThunk++, iatThunk++) {
                if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) continue;

                PIMAGE_IMPORT_BY_NAME impName = (PIMAGE_IMPORT_BY_NAME)((BYTE*)hMod + origThunk->u1.AddressOfData);
                if (strcmp((const char*)impName->Name, funcName) == 0) {
                  // Save original if requested
                  if (origFunc && *origFunc == nullptr) {
                    *origFunc = (void*)iatThunk->u1.Function;
                  }

                  // Patch the IAT entry
                  ULONG oldProt;
                  if (ChangeMemoryProtection(&iatThunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProt)) {
                    iatThunk->u1.Function = (ULONG_PTR)newFunc;
                    ChangeMemoryProtection(&iatThunk->u1.Function, sizeof(void*), oldProt, &oldProt);
                    patched++;
                  }
                }
              }
            }
          }
        }
      }
    }

    curr = curr->Flink;
  }

  return patched;
}

// ─── HOOKED FUNCTIONS ────────────────────────────────────────────────────────

static void* __cdecl hooked_malloc(size_t size) {
  if (g_redirectEnabled && p_xxmalloc) {
    return p_xxmalloc(size);
  }
  return orig_malloc ? orig_malloc(size) : nullptr;
}

static void __cdecl hooked_free(void* ptr) {
  if (g_redirectEnabled && p_xxfree) {
    p_xxfree(ptr);
  } else if (orig_free) {
    orig_free(ptr);
  }
}

static void* __cdecl hooked_calloc(size_t count, size_t size) {
  if (g_redirectEnabled && p_xxcalloc) {
    return p_xxcalloc(count, size);
  }
  return orig_calloc ? orig_calloc(count, size) : nullptr;
}

static void* __cdecl hooked_realloc(void* ptr, size_t size) {
  if (g_redirectEnabled && p_xxrealloc) {
    return p_xxrealloc(ptr, size);
  }
  return orig_realloc ? orig_realloc(ptr, size) : nullptr;
}

static size_t __cdecl hooked_msize(void* ptr) {
  if (g_redirectEnabled && p_xxmalloc_usable_size) {
    return p_xxmalloc_usable_size(ptr);
  }
  return orig_msize ? orig_msize(ptr) : 0;
}

// ─── INITIALIZATION ──────────────────────────────────────────────────────────

static bool InitNtdll() {
  hNtdll = GetModuleHandleW(L"ntdll.dll");
  if (!hNtdll) return false;

  pNtProtectVirtualMemory = (PFN_NtProtectVirtualMemory)GetProcAddress(hNtdll, "NtProtectVirtualMemory");
  pLdrGetDllHandle = (PFN_LdrGetDllHandle)GetProcAddress(hNtdll, "LdrGetDllHandle");

  return pNtProtectVirtualMemory != nullptr;
}

static void InstallPatches() {
  // CRT DLLs to patch
  const char* crtDlls[] = {
    "ucrtbase.dll",
    "ucrtbased.dll",
    "api-ms-win-crt-heap-l1-1-0.dll",
    "msvcrt.dll",
    nullptr
  };

  // Functions to patch
  struct PatchEntry {
    const char* name;
    void* hook;
    void** orig;
  };

  PatchEntry patches[] = {
    { "malloc",  (void*)hooked_malloc,  (void**)&orig_malloc },
    { "free",    (void*)hooked_free,    (void**)&orig_free },
    { "calloc",  (void*)hooked_calloc,  (void**)&orig_calloc },
    { "realloc", (void*)hooked_realloc, (void**)&orig_realloc },
    { "_msize",  (void*)hooked_msize,   (void**)&orig_msize },
  };

  for (const char** dll = crtDlls; *dll; dll++) {
    for (auto& p : patches) {
      PatchIATInAllModules(*dll, p.name, p.hook, p.orig);
    }
  }
}

// ─── PUBLIC API ──────────────────────────────────────────────────────────────

extern "C" {

// Called by the main allocator DLL to register its functions
__declspec(dllexport) void alloc8_redirect_init(
  void* (*xxmalloc)(size_t),
  void (*xxfree)(void*),
  void* (*xxcalloc)(size_t, size_t),
  void* (*xxrealloc)(void*, size_t),
  size_t (*xxmalloc_usable_size)(void*)
) {
  p_xxmalloc = xxmalloc;
  p_xxfree = xxfree;
  p_xxcalloc = xxcalloc;
  p_xxrealloc = xxrealloc;
  p_xxmalloc_usable_size = xxmalloc_usable_size;
}

// Enable/disable redirection
__declspec(dllexport) void alloc8_redirect_enable() {
  g_redirectEnabled = true;
}

__declspec(dllexport) void alloc8_redirect_disable() {
  g_redirectEnabled = false;
}

// Query state
__declspec(dllexport) bool alloc8_redirect_is_enabled() {
  return g_redirectEnabled;
}

__declspec(dllexport) bool alloc8_redirect_is_initialized() {
  return g_initialized;
}

}  // extern "C"

// ─── DLL ENTRY POINT ─────────────────────────────────────────────────────────

BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  (void)lpvReserved;

  switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
      DisableThreadLibraryCalls(hinstDLL);

      // Initialize ntdll function pointers
      if (!InitNtdll()) {
        return FALSE;
      }

      // Install IAT patches early - before any allocations
      InstallPatches();
      g_initialized = true;
      break;

    case DLL_PROCESS_DETACH:
      g_redirectEnabled = false;
      break;
  }

  return TRUE;
}
