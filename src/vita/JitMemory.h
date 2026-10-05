#pragma once

#include <cstddef>

// Reserves the executable memory pool used by Play!'s recompilers. Must be
// called once, early (before the VM is created). Returns false if the kernel
// refused the allocation (the app must be built as "unsafe" homebrew).
bool VitaJit_Init(size_t poolSize);
size_t VitaJit_GetUsedBytes();
size_t VitaJit_GetCapacity();

// Implementations of the hooks declared in CodeGen's MemoryFunction.cpp.
extern "C" void* vitaps2_jit_alloc(size_t);
extern "C" void vitaps2_jit_free(void*);
extern "C" void vitaps2_jit_begin_write();
extern "C" void vitaps2_jit_end_write(void*, size_t);
