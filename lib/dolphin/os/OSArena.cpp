#include "internal.hpp"

#include <dolphin/os.h>

#if AURORA_ENABLE_HEAP
#include <aurora/heap.h>
#else
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#endif

void* OSGetArenaHi() { return nullptr; }

void* OSGetArenaLo() { return nullptr; }

void OSSetArenaHi(void* newHi) { Log.warn("OSSetArenaHi({}) ignored: there is no arena", newHi); }

void OSSetArenaLo(void* newLo) { Log.warn("OSSetArenaLo({}) ignored: there is no arena", newLo); }

static void* AllocFromArena(u32 size, u32 align) {
#if AURORA_ENABLE_HEAP
  return aurora_heap_alloc(aurora_heap_global(), size, align, AURORA_HEAP_LANE_HEAD, 0);
#else
  const size_t alignment = std::bit_ceil(std::max<size_t>(align, alignof(std::max_align_t)));
  const size_t alignedSize = (std::max<size_t>(size, 1) + alignment - 1) & ~(alignment - 1);
#ifdef _WIN32
  void* ptr = _aligned_malloc(alignedSize, alignment);
#else
  void* ptr = std::aligned_alloc(alignment, alignedSize);
#endif
  if (ptr != nullptr) {
    std::memset(ptr, 0, alignedSize);
  }
  return ptr;
#endif
}

void* OSAllocFromArenaLo(const u32 size, const u32 align) { return AllocFromArena(size, align); }

void* OSAllocFromArenaHi(const u32 size, const u32 align) { return AllocFromArena(size, align); }
