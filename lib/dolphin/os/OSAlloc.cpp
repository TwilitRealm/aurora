#include <dolphin/os.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "../../logging.hpp"

#if AURORA_ENABLE_HEAP
#include <aurora/heap.h>
#else
#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#endif

extern "C" volatile OSHeapHandle __OSCurrHeap = -1;

namespace {

constexpr u32 kAlignment = 32;
// Reported by OSCheckHeap as the free size of every valid heap
constexpr s32 kHeadroom = 0x10000000;

aurora::Module AllocLog("aurora::os::alloc");

#if AURORA_ENABLE_HEAP
using Heap = AuroraHeap;

Heap* createHeap(OSHeapHandle handle) {
  char name[32];
  snprintf(name, sizeof(name), "OSHeap %d", handle);
  const AuroraHeapDesc desc{AURORA_HEAP_GENERAL, AURORA_HEAP_ZERO, nullptr, name};
  return aurora_heap_create(&desc);
}

void destroyHeap(Heap* heap) { aurora_heap_destroy(heap); }

void* allocFromHeap(Heap* heap, u32 size) {
  return aurora_heap_alloc(heap, size, kAlignment, AURORA_HEAP_LANE_HEAD, 0);
}

void freeToHeap(Heap* heap, void* ptr) { aurora_heap_free(heap, ptr); }

u32 blockSize(Heap* heap, void* ptr) { return static_cast<u32>(aurora_heap_block_size(heap, ptr)); }

void heapStats(Heap* heap, size_t& usedBytes, size_t& blockCount) {
  AuroraHeapStats stats;
  aurora_heap_stats(heap, &stats);
  usedBytes = stats.usedBytes;
  blockCount = stats.blockCount;
}

void visitHeap(Heap* heap, void (*visitor)(void*, u32)) {
  aurora_heap_visit(
      heap, AURORA_HEAP_LANE_ALL,
      [](void* arg, void* p, size_t size, int, uint8_t) {
        reinterpret_cast<void (*)(void*, u32)>(arg)(p, static_cast<u32>(size));
        return true;
      },
      reinterpret_cast<void*>(visitor));
}
#else
struct Heap {
  std::mutex mutex;
  std::unordered_map<void*, u32> blocks;
  size_t usedBytes = 0;
};

void releaseMemory(void* ptr) {
#ifdef _WIN32
  _aligned_free(ptr);
#else
  std::free(ptr);
#endif
}

Heap* createHeap(OSHeapHandle) { return new Heap; }

void destroyHeap(Heap* heap) {
  for (const auto& [ptr, size] : heap->blocks) {
    releaseMemory(ptr);
  }
  delete heap;
}

void* allocFromHeap(Heap* heap, u32 size) {
  const size_t total = (std::max<size_t>(size, 1) + kAlignment - 1) & ~size_t(kAlignment - 1);
#ifdef _WIN32
  void* ptr = _aligned_malloc(total, kAlignment);
#else
  void* ptr = std::aligned_alloc(kAlignment, total);
#endif
  if (ptr == nullptr) {
    return nullptr;
  }
  std::memset(ptr, 0, total);
  std::lock_guard lock(heap->mutex);
  heap->blocks.emplace(ptr, size);
  heap->usedBytes += size;
  return ptr;
}

void freeToHeap(Heap* heap, void* ptr) {
  {
    std::lock_guard lock(heap->mutex);
    const auto it = heap->blocks.find(ptr);
    if (it == heap->blocks.end()) {
      return;
    }
    heap->usedBytes -= it->second;
    heap->blocks.erase(it);
  }
  releaseMemory(ptr);
}

u32 blockSize(Heap* heap, void* ptr) {
  std::lock_guard lock(heap->mutex);
  const auto it = heap->blocks.find(ptr);
  return it != heap->blocks.end() ? it->second : 0;
}

void heapStats(Heap* heap, size_t& usedBytes, size_t& blockCount) {
  std::lock_guard lock(heap->mutex);
  usedBytes = heap->usedBytes;
  blockCount = heap->blocks.size();
}

void visitHeap(Heap* heap, void (*visitor)(void*, u32)) {
  std::vector<std::pair<void*, u32>> blocks;
  {
    std::lock_guard lock(heap->mutex);
    blocks.assign(heap->blocks.begin(), heap->blocks.end());
  }
  for (const auto& [ptr, size] : blocks) {
    visitor(ptr, size);
  }
}
#endif

std::mutex sHeapsMutex;
std::vector<Heap*> sHeaps;
bool sInitialized = false;

Heap* getHeap(OSHeapHandle handle) {
  std::lock_guard lock(sHeapsMutex);
  if (handle < 0 || static_cast<size_t>(handle) >= sHeaps.size()) {
    return nullptr;
  }
  return sHeaps[handle];
}

} // namespace

extern "C" {

void* OSInitAlloc(void* arenaStart, void* arenaEnd, int maxHeaps) {
  if (maxHeaps <= 0) {
    return nullptr;
  }

  std::vector<Heap*> old;
  {
    std::lock_guard lock(sHeapsMutex);
    old.swap(sHeaps);
    sHeaps.assign(maxHeaps, nullptr);
    sInitialized = true;
  }
  for (Heap* heap : old) {
    if (heap != nullptr) {
      destroyHeap(heap);
    }
  }

  __OSCurrHeap = -1;
  return arenaStart;
}

OSHeapHandle OSCreateHeap(void* start, void* end) {
  std::lock_guard lock(sHeapsMutex);
  if (!sInitialized) {
    return -1;
  }
  for (size_t i = 0; i < sHeaps.size(); ++i) {
    if (sHeaps[i] != nullptr) {
      continue;
    }
    const auto handle = static_cast<OSHeapHandle>(i);
    sHeaps[i] = createHeap(handle);
    return sHeaps[i] != nullptr ? handle : -1;
  }
  return -1;
}

void OSDestroyHeap(OSHeapHandle heap) {
  Heap* destroyed = nullptr;
  {
    std::lock_guard lock(sHeapsMutex);
    if (heap < 0 || static_cast<size_t>(heap) >= sHeaps.size()) {
      return;
    }
    destroyed = sHeaps[heap];
    sHeaps[heap] = nullptr;
  }
  if (destroyed != nullptr) {
    destroyHeap(destroyed);
  }
  if (__OSCurrHeap == heap) {
    __OSCurrHeap = -1;
  }
}

void OSAddToHeap(OSHeapHandle heap, void* start, void* end) {}

void* OSAllocFromHeap(OSHeapHandle heap, u32 size) {
  Heap* h = getHeap(heap);
  if (h == nullptr) {
    return nullptr;
  }
  return allocFromHeap(h, size);
}

void OSFreeToHeap(OSHeapHandle heap, void* ptr) {
  Heap* h = getHeap(heap);
  if (h == nullptr) {
    return;
  }
  freeToHeap(h, ptr);
}

OSHeapHandle OSSetCurrentHeap(OSHeapHandle heap) {
  const auto prev = __OSCurrHeap;
  if (heap == -1 || getHeap(heap) != nullptr) {
    __OSCurrHeap = heap;
  }
  return prev;
}

void* OSAllocFixed(void* rstart, void* rend) {
  AllocLog.warn("OSAllocFixed({}, {}) is not supported: heaps are not carved out of the arena", rstart, rend);
  return nullptr;
}

s32 OSCheckHeap(OSHeapHandle heap) { return getHeap(heap) != nullptr ? kHeadroom : -1; }

u32 OSReferentSize(void* ptr) {
  if (ptr == nullptr) {
    return 0;
  }
  std::vector<Heap*> heaps;
  {
    std::lock_guard lock(sHeapsMutex);
    heaps = sHeaps;
  }
  for (Heap* heap : heaps) {
    if (heap == nullptr) {
      continue;
    }
    if (const u32 size = blockSize(heap, ptr); size != 0) {
      return size;
    }
  }
  return 0;
}

void OSDumpHeap(OSHeapHandle heap) {
  AllocLog.info("OSDumpHeap({})", heap);
  Heap* h = getHeap(heap);
  if (h == nullptr) {
    AllocLog.info("--------Invalid");
    return;
  }
  size_t usedBytes = 0;
  size_t blockCount = 0;
  heapStats(h, usedBytes, blockCount);
  AllocLog.info("{} bytes in {} blocks", usedBytes, blockCount);
}

void OSVisitAllocated(void (*visitor)(void*, u32)) {
  if (visitor == nullptr) {
    return;
  }
  std::vector<Heap*> heaps;
  {
    std::lock_guard lock(sHeapsMutex);
    heaps = sHeaps;
  }
  for (Heap* heap : heaps) {
    if (heap != nullptr) {
      visitHeap(heap, visitor);
    }
  }
}

} // extern "C"
