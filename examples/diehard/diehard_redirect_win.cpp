// alloc8/examples/diehard/diehard_redirect_win.cpp
// DieHard allocator using alloc8-redirect (IAT patching) instead of Detours
//
// This provides a simpler redirect wrapper for DieHard since it doesn't need
// per-thread TLS management like Hoard does.
//
// Performance: ~2.4x faster hooking overhead vs Detours

#ifndef _WIN32
#error "This file is for Windows only"
#endif

#include <windows.h>
#include <iostream>

// Export ordinal #1 for withdll.exe compatibility
// withdll.exe uses DetourCreateProcessWithDllEx which requires this export
// We provide a stub since we're not using Detours
extern "C" __declspec(dllexport) void WINAPI DetourFinishHelperProcess(HWND, HINSTANCE, LPSTR, INT) {}
#pragma comment(linker, "/EXPORT:DetourFinishHelperProcess,@1,NONAME")

// Forward declarations for xxmalloc functions (from diehard_alloc8.cpp)
extern "C" {
  void* xxmalloc(size_t sz);
  void xxfree(void* ptr);
  void* xxcalloc(size_t count, size_t size);
  void* xxrealloc(void* ptr, size_t sz);
  size_t xxmalloc_usable_size(void* ptr);
}

// alloc8-redirect API
extern "C" {
  __declspec(dllimport) void alloc8_redirect_init(
    void* (*xxmalloc)(size_t),
    void (*xxfree)(void*),
    void* (*xxcalloc)(size_t, size_t),
    void* (*xxrealloc)(void*, size_t),
    size_t (*xxmalloc_usable_size)(void*)
  );
  __declspec(dllimport) void alloc8_redirect_enable();
  __declspec(dllimport) void alloc8_redirect_disable();
}

// ─── DLL ENTRY POINT ─────────────────────────────────────────────────────────

BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
      {
        DisableThreadLibraryCalls(hinstDLL);

        // Pin this DLL in memory to prevent unloading
        HMODULE hSelf = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCWSTR)DllMain, &hSelf);

        // Force initialization of the C++ library before anything else
        std::cout << "";

        // Initialize and enable alloc8-redirect (IAT patching)
        alloc8_redirect_init(xxmalloc, xxfree, xxcalloc, xxrealloc, xxmalloc_usable_size);
        alloc8_redirect_enable();

        // Verification message
        fprintf(stderr, "[DieHard redirect] Memory allocator active (IAT patching)\n");
      }
      break;

    case DLL_PROCESS_DETACH:
      {
        if (lpvReserved == nullptr) {
          // Dynamic unload (FreeLibrary)
          alloc8_redirect_disable();
        } else {
          // Process exit - force immediate termination to avoid crashes
          TerminateProcess(GetCurrentProcess(), 0);
        }
      }
      break;
  }

  return TRUE;
}
