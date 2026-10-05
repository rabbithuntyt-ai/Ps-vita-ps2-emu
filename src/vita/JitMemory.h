#pragma once

#include <cstddef>

#include <string>

// Reserves the executable memory pool used by Play!'s recompilers. Must be
// called once, early (before the VM is created). Tries sizes from maxSize
// down to minSize (1MB granularity). Returns false if nothing could be
// allocated; VitaJit_GetDiagnostics() then explains why.
bool VitaJit_Init(size_t minSize, size_t maxSize);
std::string VitaJit_GetDiagnostics();
size_t VitaJit_GetUsedBytes();
size_t VitaJit_GetCapacity();

// Implementations of the hooks declared in CodeGen's MemoryFunction.cpp.
extern "C" void* vitaps2_jit_alloc(size_t);
extern "C" void vitaps2_jit_free(void*);
extern "C" void vitaps2_jit_begin_write();
extern "C" void vitaps2_jit_end_write(void*, size_t);
