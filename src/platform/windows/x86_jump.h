// alloc8/src/platform/windows/x86_jump.h
// x86/x64 jump instruction structures for manual code patching
//
// This is a fallback for when Microsoft Detours is not available.
// Reference: Heap-Layers x86jump.h by Emery Berger

#ifndef ALLOC8_X86_JUMP_H
#define ALLOC8_X86_JUMP_H

#ifndef _WIN32
#error "This file is for Windows only"
#endif

#include <cstdint>

// Ensure structures are packed with no padding
#pragma pack(push, 1)

namespace alloc8 {

/**
 * 32-bit relative jump instruction.
 * Used for patching function entry points on x86.
 *
 * Opcode: E9 xx xx xx xx
 * Size: 5 bytes
 */
struct X86Jump32 {
  uint8_t  jmp_opcode;   // 0xE9 = JMP rel32
  uint32_t jmp_offset;   // 32-bit relative offset

  X86Jump32(void* target, void* from) {
    jmp_opcode = 0xE9;
    // Calculate relative offset: target - (from + sizeof(this))
    intptr_t offset = reinterpret_cast<intptr_t>(target) -
                      (reinterpret_cast<intptr_t>(from) + sizeof(X86Jump32));
    jmp_offset = static_cast<uint32_t>(offset);
  }
};
static_assert(sizeof(X86Jump32) == 5, "X86Jump32 must be 5 bytes");

/**
 * 64-bit absolute jump instruction using RIP-relative addressing.
 * Used for patching function entry points on x64.
 *
 * Opcodes: FF 25 00 00 00 00 (JMP [RIP+0]) followed by 64-bit address
 * Size: 14 bytes
 */
struct X86Jump64 {
  uint16_t farjmp;    // 0x25FF = JMP [RIP+disp32]
  uint32_t offset;    // 0x00000000 = offset to address (immediately following)
  uint64_t addr;      // 64-bit absolute target address

  X86Jump64(void* target) {
    farjmp = 0x25FF;
    offset = 0x00000000;
    addr = reinterpret_cast<uint64_t>(target);
  }
};
static_assert(sizeof(X86Jump64) == 14, "X86Jump64 must be 14 bytes");

/**
 * Platform-appropriate jump structure.
 */
#if defined(_WIN64)
using X86Jump = X86Jump64;
#else
using X86Jump = X86Jump32;
#endif

/**
 * Patch entry for manual code patching.
 */
struct ManualPatch {
  const char* name;        // Function name (for GetProcAddress)
  void* replacement;       // Replacement function
  void* original;          // Original function address (after lookup)
  uint8_t savedBytes[sizeof(X86Jump)];  // Original bytes for restoration
  bool applied;
};

/**
 * Apply a manual patch by overwriting the function prologue.
 *
 * @param hModule Module containing the function
 * @param patch Patch entry to apply
 * @return true if patch was applied
 */
inline bool ApplyManualPatch(HMODULE hModule, ManualPatch* patch) {
  FARPROC proc = GetProcAddress(hModule, patch->name);
  if (!proc) return false;

  patch->original = reinterpret_cast<void*>(proc);

  // Get memory region info
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(patch->original, &mbi, sizeof(mbi))) {
    return false;
  }

  // Make memory writable
  DWORD oldProtect;
  if (!VirtualProtect(mbi.BaseAddress, mbi.RegionSize,
                      PAGE_EXECUTE_READWRITE, &oldProtect)) {
    return false;
  }

  // Save original bytes
  memcpy(patch->savedBytes, patch->original, sizeof(X86Jump));

  // Write jump instruction
#if defined(_WIN64)
  new (patch->original) X86Jump64(patch->replacement);
#else
  new (patch->original) X86Jump32(patch->replacement, patch->original);
#endif

  // Restore original protection
  VirtualProtect(mbi.BaseAddress, mbi.RegionSize, oldProtect, &oldProtect);

  // Flush instruction cache
  FlushInstructionCache(GetCurrentProcess(), patch->original, sizeof(X86Jump));

  patch->applied = true;
  return true;
}

/**
 * Remove a manual patch by restoring original bytes.
 *
 * @param patch Patch entry to remove
 * @return true if patch was removed
 */
inline bool RemoveManualPatch(ManualPatch* patch) {
  if (!patch->applied || !patch->original) return false;

  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(patch->original, &mbi, sizeof(mbi))) {
    return false;
  }

  DWORD oldProtect;
  if (!VirtualProtect(mbi.BaseAddress, mbi.RegionSize,
                      PAGE_EXECUTE_READWRITE, &oldProtect)) {
    return false;
  }

  // Restore original bytes
  memcpy(patch->original, patch->savedBytes, sizeof(X86Jump));

  VirtualProtect(mbi.BaseAddress, mbi.RegionSize, oldProtect, &oldProtect);
  FlushInstructionCache(GetCurrentProcess(), patch->original, sizeof(X86Jump));

  patch->applied = false;
  return true;
}

} // namespace alloc8

#pragma pack(pop)

#endif // ALLOC8_X86_JUMP_H
