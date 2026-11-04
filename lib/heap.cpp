#include <aurora/heap.h>

#include "logging.hpp"

#include <mimalloc.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace {
aurora::Module Log("aurora::heap");

constexpr uint32_t kLiveMagic = 0x4C424841;  // "AHBL"
constexpr uint32_t kFreedMagic = 0x46424841; // "AHBF"
constexpr uint16_t kBlockRanged = 1u << 0;
constexpr size_t kMinAlign = 16;
// mimalloc's page map only resolves interior pointers within the first 4 MiB (minus one arena slice) of a page,
// so general blocks at least this large are also tracked in the range map.
constexpr size_t kRangedSize = 2 * 1024 * 1024;
// Block sizes are stored in 32 bits
constexpr size_t kMaxGeneralSize = UINT32_MAX - 64;
// First chunk of a lane without a size hint
constexpr size_t kDefaultChunkSize = 64 * 1024;
constexpr size_t kMinChunkSize = 256;
// Kept below mimalloc's interior pointer coverage; requests over half of it get a dedicated chunk.
constexpr size_t kChunkMaxSize = 2 * 1024 * 1024;
constexpr uint8_t kAllocFill = 0xCD;
constexpr uint8_t kFreeFill = 0xDD;

struct alignas(16) Block {
  uint32_t magic;
  uint8_t lane;
  uint8_t tag;
  uint16_t flags;
  uint32_t size;
  // mimalloc size class of the whole allocation, for reservedBytes
  uint32_t reserved;
  Block* prev;
  Block* next;
};
static_assert(sizeof(Block) == 32, "Block header must stay 32 bytes");

struct alignas(16) Chunk {
  Chunk* next;
  uint8_t* cur;
  uint8_t* end;
  size_t size;

  uint8_t* data() { return reinterpret_cast<uint8_t*>(this + 1); }
};
static_assert(sizeof(Chunk) % kMinAlign == 0, "Chunk data must stay aligned");

struct Lane {
  Chunk* first = nullptr;
  Chunk* last = nullptr;
  Chunk* current = nullptr;
  size_t nextChunkSize = kDefaultChunkSize;
};

struct RangeEntry {
  uintptr_t end;
  AuroraHeap* heap;
  int lane;
};

struct Registry {
  std::shared_mutex mutex;
  std::map<uintptr_t, RangeEntry> ranges;
  std::unordered_map<const mi_heap_t*, AuroraHeap*> owners;
  mi_heap_t* chunkHeap = nullptr;
};

void on_mi_error(int err, void*) {
  Log.error("mimalloc reported error {}", err);
  assert((err == ENOMEM || err == EOVERFLOW) && "mimalloc detected heap corruption");
}

Registry& registry() {
  static Registry* sRegistry = [] {
    mi_register_error(on_mi_error, nullptr);
    auto* reg = new Registry;
    reg->chunkHeap = mi_heap_new();
    if (reg->chunkHeap == nullptr) {
      Log.fatal("Failed to create the bump chunk heap");
    }
    return reg;
  }();
  return *sRegistry;
}
} // namespace

struct AuroraHeap {
  AuroraHeapKind kind;
  uint32_t flags;
  void* owner;
  char name[32];
  mutable std::mutex mutex;

  // General heaps
  mi_heap_t* mi;
  Block* head;
  Block* tail;
  size_t rangedBlocks;

  // Bump heaps
  Lane lanes[2];

  size_t laneUsed[2];
  size_t laneBlocks[2];
  size_t peakBytes;
  size_t reservedBytes;
};

namespace {
void add_range(const void* start, size_t size, AuroraHeap* heap, int lane) {
  Registry& reg = registry();
  const auto begin = reinterpret_cast<uintptr_t>(start);
  std::unique_lock lock(reg.mutex);
  reg.ranges[begin] = RangeEntry{begin + size, heap, lane};
}

void remove_ranges(const std::vector<void*>& starts) {
  if (starts.empty()) {
    return;
  }
  Registry& reg = registry();
  std::unique_lock lock(reg.mutex);
  for (void* start : starts) {
    reg.ranges.erase(reinterpret_cast<uintptr_t>(start));
  }
}

void account_alloc(AuroraHeap* heap, int lane, size_t size) {
  heap->laneUsed[lane] += size;
  heap->laneBlocks[lane]++;
  heap->peakBytes = std::max(heap->peakBytes, heap->laneUsed[0] + heap->laneUsed[1]);
}

void account_free(AuroraHeap* heap, int lane, size_t size) {
  heap->laneUsed[lane] -= size;
  heap->laneBlocks[lane]--;
}

void unlink_block(AuroraHeap* heap, Block* block) {
  if (block->prev != nullptr) {
    block->prev->next = block->next;
  } else {
    heap->head = block->next;
  }
  if (block->next != nullptr) {
    block->next->prev = block->prev;
  } else {
    heap->tail = block->prev;
  }
}

// Requires heap->mutex. Block starts are always within mimalloc's page map coverage, so this needs no registry
// lookup.
bool owns_block(const AuroraHeap* heap, const void* p) {
  if (p == nullptr || (reinterpret_cast<uintptr_t>(p) & (kMinAlign - 1)) != 0) {
    return false;
  }
  return mi_heap_of(p) == heap->mi;
}

size_t first_chunk_size(size_t sizeHint) {
  if (sizeHint == 0) {
    return kDefaultChunkSize;
  }
  if (sizeHint >= kChunkMaxSize - sizeof(Chunk)) {
    return kChunkMaxSize;
  }
  const size_t size = (sizeof(Chunk) + sizeHint + kMinAlign - 1) & ~(kMinAlign - 1);
  return std::max(size, kMinChunkSize);
}

void report_bad_free(const AuroraHeap* heap, const void* p, bool freedMagic) {
  static std::atomic_bool sLogged;
  if (!sLogged.exchange(true)) {
    Log.error("Ignoring free of {} in heap '{}': {}", p, heap->name,
              freedMagic ? "double free" : "double free, or not the start of a block");
  }
  assert(false && "aurora_heap: double free or invalid free");
}

void* general_alloc(AuroraHeap* heap, size_t size, size_t align, int lane, uint8_t tag) {
  if (size > kMaxGeneralSize) {
    return nullptr;
  }
  const size_t total = sizeof(Block) + size;
  const bool zero = (heap->flags & AURORA_HEAP_ZERO) != 0;
  void* raw = zero ? mi_heap_zalloc_aligned_at(heap->mi, total, align, sizeof(Block))
                   : mi_heap_malloc_aligned_at(heap->mi, total, align, sizeof(Block));
  if (raw == nullptr) {
    return nullptr;
  }

  auto* block = static_cast<Block*>(raw);
  block->magic = kLiveMagic;
  block->lane = static_cast<uint8_t>(lane);
  block->tag = tag;
  block->flags = total >= kRangedSize ? kBlockRanged : 0;
  block->size = static_cast<uint32_t>(size);
  block->reserved = static_cast<uint32_t>(std::min<size_t>(mi_good_size(total), UINT32_MAX));
  block->next = nullptr;
  void* p = block + 1;
  if (!zero && (heap->flags & AURORA_HEAP_DEBUG_FILL) != 0) {
    std::memset(p, kAllocFill, size);
  }

  std::lock_guard lock(heap->mutex);
  block->prev = heap->tail;
  if (heap->tail != nullptr) {
    heap->tail->next = block;
  } else {
    heap->head = block;
  }
  heap->tail = block;
  if ((block->flags & kBlockRanged) != 0) {
    heap->rangedBlocks++;
    add_range(block, total, heap, -1);
  }
  account_alloc(heap, lane, size);
  heap->reservedBytes += block->reserved;
  return p;
}

size_t general_free(AuroraHeap* heap, void* p) {
  auto* block = static_cast<Block*>(p) - 1;
  size_t size;
  bool ranged;
  {
    std::unique_lock lock(heap->mutex);
    if (!owns_block(heap, p)) {
      lock.unlock();
      Log.debug("Ignoring free of {} in heap '{}': not owned by this heap", p, heap->name);
      return 0;
    }
    if (block->magic != kLiveMagic) {
      const bool doubleFree = block->magic == kFreedMagic;
      lock.unlock();
      report_bad_free(heap, p, doubleFree);
      return 0;
    }
    unlink_block(heap, block);
    size = block->size;
    ranged = (block->flags & kBlockRanged) != 0;
    if (ranged) {
      heap->rangedBlocks--;
    }
    account_free(heap, block->lane, size);
    heap->reservedBytes -= block->reserved;
    block->magic = kFreedMagic;
  }

  if ((heap->flags & AURORA_HEAP_DEBUG_FILL) != 0) {
    std::memset(p, kFreeFill, size);
  }
  if (ranged) {
    remove_ranges({block});
  }
  mi_free(block);
  return size;
}

// Requires heap->mutex. Ranges of released blocks are appended to ranged.
void general_release_lane(AuroraHeap* heap, int lane, std::vector<Block*>& released, std::vector<void*>& ranged) {
  for (Block* block = heap->head; block != nullptr;) {
    Block* next = block->next;
    if (block->lane == lane) {
      unlink_block(heap, block);
      if ((block->flags & kBlockRanged) != 0) {
        heap->rangedBlocks--;
        ranged.push_back(block);
      }
      heap->reservedBytes -= block->reserved;
      block->magic = kFreedMagic;
      released.push_back(block);
    }
    block = next;
  }
  heap->laneUsed[lane] = 0;
  heap->laneBlocks[lane] = 0;
}

void general_reset(AuroraHeap* heap, int lane) {
  std::vector<void*> ranged;
  if (lane != AURORA_HEAP_LANE_ALL) {
    std::vector<Block*> released;
    {
      std::lock_guard lock(heap->mutex);
      general_release_lane(heap, lane, released, ranged);
    }
    remove_ranges(ranged);
    for (Block* block : released) {
      if ((heap->flags & AURORA_HEAP_DEBUG_FILL) != 0) {
        std::memset(block + 1, kFreeFill, block->size);
      }
      mi_free(block);
    }
    return;
  }

  mi_heap_t* fresh = mi_heap_new();
  if (fresh == nullptr) {
    Log.fatal("Failed to create a mimalloc heap while resetting heap '{}'", heap->name);
  }
  mi_heap_t* old;
  {
    std::lock_guard lock(heap->mutex);
    const bool fill = (heap->flags & AURORA_HEAP_DEBUG_FILL) != 0;
    if (fill || heap->rangedBlocks != 0) {
      for (Block* block = heap->head; block != nullptr; block = block->next) {
        if ((block->flags & kBlockRanged) != 0) {
          ranged.push_back(block);
        }
        if (fill) {
          std::memset(block + 1, kFreeFill, block->size);
        }
      }
    }
    old = heap->mi;
    heap->mi = fresh;
    heap->head = nullptr;
    heap->tail = nullptr;
    heap->rangedBlocks = 0;
    heap->laneUsed[0] = heap->laneUsed[1] = 0;
    heap->laneBlocks[0] = heap->laneBlocks[1] = 0;
    heap->reservedBytes = 0;
  }

  Registry& reg = registry();
  {
    std::unique_lock lock(reg.mutex);
    for (void* start : ranged) {
      reg.ranges.erase(reinterpret_cast<uintptr_t>(start));
    }
    reg.owners.erase(old);
    reg.owners[fresh] = heap;
  }
  mi_heap_destroy(old);
}

uint8_t* chunk_fit(Chunk* chunk, size_t size, size_t align) {
  const auto cur = reinterpret_cast<uintptr_t>(chunk->cur);
  const auto end = reinterpret_cast<uintptr_t>(chunk->end);
  const uintptr_t start = (cur + align - 1) & ~(static_cast<uintptr_t>(align) - 1);
  if (start > end || size > end - start) {
    return nullptr;
  }
  chunk->cur = reinterpret_cast<uint8_t*>(start + size);
  return reinterpret_cast<uint8_t*>(start);
}

// Requires heap->mutex.
Chunk* new_chunk(AuroraHeap* heap, int lane, size_t size) {
  void* raw = mi_heap_malloc_aligned(registry().chunkHeap, size, kMinAlign);
  if (raw == nullptr) {
    return nullptr;
  }
  auto* chunk = static_cast<Chunk*>(raw);
  chunk->next = nullptr;
  chunk->cur = chunk->data();
  chunk->end = static_cast<uint8_t*>(raw) + size;
  chunk->size = size;

  Lane& l = heap->lanes[lane];
  if (l.last != nullptr) {
    l.last->next = chunk;
  } else {
    l.first = chunk;
  }
  l.last = chunk;
  heap->reservedBytes += size;
  add_range(raw, size, heap, lane);
  return chunk;
}

void* bump_alloc(AuroraHeap* heap, size_t size, size_t align, int lane) {
  uint8_t* p;
  {
    std::lock_guard lock(heap->mutex);
    Lane& l = heap->lanes[lane];
    p = l.current != nullptr ? chunk_fit(l.current, size, align) : nullptr;
    if (p == nullptr) {
      // Chunk data starts 16-byte aligned, so this covers the worst-case padding
      if (size > SIZE_MAX - align - sizeof(Chunk)) {
        return nullptr;
      }
      const size_t need = size + align - kMinAlign;
      Chunk* chunk;
      if (need > kChunkMaxSize / 2) {
        chunk = new_chunk(heap, lane, sizeof(Chunk) + need);
      } else {
        size_t chunkSize = l.nextChunkSize;
        while (chunkSize - sizeof(Chunk) < need) {
          chunkSize *= 2;
        }
        l.nextChunkSize = std::min(chunkSize * 2, kChunkMaxSize);
        chunk = new_chunk(heap, lane, chunkSize);
        if (chunk != nullptr) {
          l.current = chunk;
        }
      }
      if (chunk == nullptr) {
        return nullptr;
      }
      p = chunk_fit(chunk, size, align);
      assert(p != nullptr);
    }
    account_alloc(heap, lane, size);
  }

  if ((heap->flags & AURORA_HEAP_ZERO) != 0) {
    std::memset(p, 0, size);
  } else if ((heap->flags & AURORA_HEAP_DEBUG_FILL) != 0) {
    std::memset(p, kAllocFill, size);
  }
  return p;
}

// Requires heap->mutex. Keeps the first head chunk for reuse.
void bump_release_lane(AuroraHeap* heap, int lane, std::vector<void*>& released) {
  Lane& l = heap->lanes[lane];
  Chunk* keep = lane == AURORA_HEAP_LANE_HEAD ? l.first : nullptr;
  const bool fill = (heap->flags & AURORA_HEAP_DEBUG_FILL) != 0;
  for (Chunk* chunk = l.first; chunk != nullptr;) {
    Chunk* next = chunk->next;
    if (fill) {
      std::memset(chunk->data(), kFreeFill, chunk->cur - chunk->data());
    }
    if (chunk != keep) {
      heap->reservedBytes -= chunk->size;
      released.push_back(chunk);
    }
    chunk = next;
  }
  if (keep != nullptr) {
    keep->next = nullptr;
    keep->cur = keep->data();
  }
  l.first = keep;
  l.last = keep;
  l.current = keep;
  heap->laneUsed[lane] = 0;
  heap->laneBlocks[lane] = 0;
}

void free_chunks(const std::vector<void*>& chunks) {
  remove_ranges(chunks);
  for (void* chunk : chunks) {
    mi_free(chunk);
  }
}

void bump_reset(AuroraHeap* heap, int lane) {
  std::vector<void*> released;
  {
    std::lock_guard lock(heap->mutex);
    if (lane == AURORA_HEAP_LANE_ALL || lane == AURORA_HEAP_LANE_HEAD) {
      bump_release_lane(heap, AURORA_HEAP_LANE_HEAD, released);
    }
    if (lane == AURORA_HEAP_LANE_ALL || lane == AURORA_HEAP_LANE_TAIL) {
      bump_release_lane(heap, AURORA_HEAP_LANE_TAIL, released);
    }
  }
  free_chunks(released);
}

struct VisitEntry {
  void* p;
  size_t size;
  int lane;
  uint8_t tag;
};
} // namespace

AuroraHeap* aurora_heap_create(const AuroraHeapDesc* desc) {
  if (desc == nullptr) {
    return nullptr;
  }
  Registry& reg = registry();
  auto* heap = new AuroraHeap{};
  heap->kind = desc->kind;
  heap->flags = desc->flags;
  heap->owner = desc->owner;
  aurora_heap_set_name(heap, desc->name);
  for (Lane& lane : heap->lanes) {
    lane.nextChunkSize = first_chunk_size(desc->sizeHint);
  }
  if (heap->kind == AURORA_HEAP_GENERAL) {
    heap->mi = mi_heap_new();
    if (heap->mi == nullptr) {
      Log.error("Failed to create a mimalloc heap for heap '{}'", heap->name);
      delete heap;
      return nullptr;
    }
    std::unique_lock lock(reg.mutex);
    reg.owners[heap->mi] = heap;
  }
  return heap;
}

void aurora_heap_destroy(AuroraHeap* heap) {
  if (heap == nullptr) {
    return;
  }
  if (heap == aurora_heap_global()) {
    Log.error("The global heap cannot be destroyed");
    return;
  }

  if (heap->kind == AURORA_HEAP_GENERAL) {
    std::vector<void*> ranged;
    {
      std::lock_guard lock(heap->mutex);
      if (heap->rangedBlocks != 0) {
        for (Block* block = heap->head; block != nullptr; block = block->next) {
          if ((block->flags & kBlockRanged) != 0) {
            ranged.push_back(block);
          }
        }
      }
    }
    Registry& reg = registry();
    {
      std::unique_lock lock(reg.mutex);
      for (void* start : ranged) {
        reg.ranges.erase(reinterpret_cast<uintptr_t>(start));
      }
      reg.owners.erase(heap->mi);
    }
    mi_heap_destroy(heap->mi);
  } else {
    std::vector<void*> chunks;
    {
      std::lock_guard lock(heap->mutex);
      for (const Lane& l : heap->lanes) {
        for (Chunk* chunk = l.first; chunk != nullptr; chunk = chunk->next) {
          chunks.push_back(chunk);
        }
      }
    }
    free_chunks(chunks);
  }
  delete heap;
}

void aurora_heap_reset(AuroraHeap* heap, int lane) {
  if (heap == nullptr) {
    return;
  }
  if (lane != AURORA_HEAP_LANE_HEAD && lane != AURORA_HEAP_LANE_TAIL) {
    lane = AURORA_HEAP_LANE_ALL;
  }
  if (heap->kind == AURORA_HEAP_GENERAL) {
    general_reset(heap, lane);
  } else {
    bump_reset(heap, lane);
  }
}

void* aurora_heap_alloc(AuroraHeap* heap, size_t size, size_t align, int lane, uint8_t tag) {
  if (heap == nullptr) {
    return nullptr;
  }
  if (lane != AURORA_HEAP_LANE_TAIL) {
    lane = AURORA_HEAP_LANE_HEAD;
  }
  align = std::bit_ceil(std::max(align, kMinAlign));
  if (size == 0) {
    size = 1;
  }
  void* p = heap->kind == AURORA_HEAP_GENERAL ? general_alloc(heap, size, align, lane, tag)
                                              : bump_alloc(heap, size, align, lane);
  if (p == nullptr) {
    Log.error("Out of memory allocating {} bytes (alignment {}) in heap '{}'", size, align, heap->name);
  }
  return p;
}

size_t aurora_heap_free(AuroraHeap* heap, void* p) {
  if (heap == nullptr || p == nullptr || heap->kind != AURORA_HEAP_GENERAL) {
    return 0;
  }
  return general_free(heap, p);
}

bool aurora_heap_expand(AuroraHeap* heap, void* p, size_t newSize) {
  if (heap == nullptr || heap->kind != AURORA_HEAP_GENERAL || newSize > kMaxGeneralSize) {
    return false;
  }
  if (newSize == 0) {
    newSize = 1;
  }
  auto* block = static_cast<Block*>(p) - 1;
  std::lock_guard lock(heap->mutex);
  if (!owns_block(heap, p) || block->magic != kLiveMagic) {
    return false;
  }
  const size_t oldSize = block->size;
  if (newSize > oldSize && mi_expand(block, sizeof(Block) + newSize) == nullptr) {
    return false;
  }
  block->size = static_cast<uint32_t>(newSize);
  heap->laneUsed[block->lane] = heap->laneUsed[block->lane] - oldSize + newSize;
  heap->peakBytes = std::max(heap->peakBytes, heap->laneUsed[0] + heap->laneUsed[1]);

  const size_t total = sizeof(Block) + newSize;
  if ((block->flags & kBlockRanged) != 0 || total >= kRangedSize) {
    if ((block->flags & kBlockRanged) == 0) {
      block->flags |= kBlockRanged;
      heap->rangedBlocks++;
    }
    add_range(block, total, heap, -1);
  }
  return true;
}

size_t aurora_heap_block_size(AuroraHeap* heap, const void* p) {
  if (heap == nullptr || heap->kind != AURORA_HEAP_GENERAL) {
    return 0;
  }
  const auto* block = static_cast<const Block*>(p) - 1;
  std::lock_guard lock(heap->mutex);
  return owns_block(heap, p) && block->magic == kLiveMagic ? block->size : 0;
}

void* aurora_heap_lookup(const void* p, AuroraHeap** outHeap, int* outLane) {
  AuroraHeap* heap = nullptr;
  int lane = -1;
  if (p != nullptr) {
    Registry& reg = registry();
    const mi_heap_t* mh = mi_heap_of(p);
    std::shared_lock lock(reg.mutex);
    if (mh != nullptr && mh != reg.chunkHeap) {
      if (const auto it = reg.owners.find(mh); it != reg.owners.end()) {
        heap = it->second;
      }
    } else {
      const auto addr = reinterpret_cast<uintptr_t>(p);
      auto it = reg.ranges.upper_bound(addr);
      if (it != reg.ranges.begin()) {
        --it;
        if (addr < it->second.end) {
          heap = it->second.heap;
          lane = it->second.lane;
        }
      }
    }
  }
  if (outHeap != nullptr) {
    *outHeap = heap;
  }
  if (outLane != nullptr) {
    *outLane = lane;
  }
  return heap != nullptr ? heap->owner : nullptr;
}

void aurora_heap_stats(const AuroraHeap* heap, AuroraHeapStats* out) {
  if (out == nullptr) {
    return;
  }
  *out = {};
  if (heap == nullptr) {
    return;
  }
  std::lock_guard lock(heap->mutex);
  for (int lane = 0; lane < 2; ++lane) {
    out->laneUsedBytes[lane] = heap->laneUsed[lane];
    out->laneBlockCount[lane] = heap->laneBlocks[lane];
  }
  out->usedBytes = heap->laneUsed[0] + heap->laneUsed[1];
  out->blockCount = heap->laneBlocks[0] + heap->laneBlocks[1];
  out->peakBytes = heap->peakBytes;
  out->reservedBytes = heap->reservedBytes;
}

size_t aurora_heap_tag_used(const AuroraHeap* heap, uint8_t tag) {
  if (heap == nullptr || heap->kind != AURORA_HEAP_GENERAL) {
    return 0;
  }
  size_t used = 0;
  std::lock_guard lock(heap->mutex);
  for (const Block* block = heap->head; block != nullptr; block = block->next) {
    if (block->tag == tag) {
      used += block->size;
    }
  }
  return used;
}

void aurora_heap_visit(AuroraHeap* heap, int lane, AuroraHeapVisitor visitor, void* arg) {
  if (heap == nullptr || visitor == nullptr) {
    return;
  }
  std::vector<VisitEntry> entries;
  {
    std::lock_guard lock(heap->mutex);
    if (heap->kind == AURORA_HEAP_GENERAL) {
      entries.reserve(heap->laneBlocks[0] + heap->laneBlocks[1]);
      for (Block* block = heap->head; block != nullptr; block = block->next) {
        if (lane < 0 || block->lane == lane) {
          entries.push_back({block + 1, block->size, block->lane, block->tag});
        }
      }
    } else {
      for (int l = 0; l < 2; ++l) {
        if (lane >= 0 && l != lane) {
          continue;
        }
        for (Chunk* chunk = heap->lanes[l].first; chunk != nullptr; chunk = chunk->next) {
          entries.push_back({chunk->data(), static_cast<size_t>(chunk->cur - chunk->data()), l, 0});
        }
      }
    }
  }
  for (const VisitEntry& entry : entries) {
    if (!visitor(arg, entry.p, entry.size, entry.lane, entry.tag)) {
      break;
    }
  }
}

void* aurora_heap_next(AuroraHeap* heap, int lane, const void* prev, size_t* size) {
  if (heap == nullptr || heap->kind != AURORA_HEAP_GENERAL) {
    return nullptr;
  }
  std::lock_guard lock(heap->mutex);
  const Block* block = nullptr;
  if (prev != nullptr) {
    block = static_cast<const Block*>(prev) - 1;
    if (block->magic != kLiveMagic) {
      Log.warn("aurora_heap_next: {} is not live in heap '{}', restarting from the first block", prev, heap->name);
      block = nullptr;
    }
  }
  Block* next = block != nullptr ? block->next : heap->head;
  while (next != nullptr && lane >= 0 && next->lane != lane) {
    next = next->next;
  }
  if (next == nullptr) {
    return nullptr;
  }
  if (size != nullptr) {
    *size = next->size;
  }
  return next + 1;
}

void aurora_heap_set_name(AuroraHeap* heap, const char* name) {
  if (heap == nullptr) {
    return;
  }
  std::lock_guard lock(heap->mutex);
  std::memset(heap->name, 0, sizeof(heap->name));
  if (name != nullptr) {
    std::strncpy(heap->name, name, sizeof(heap->name) - 1);
  }
}

const char* aurora_heap_get_name(const AuroraHeap* heap) { return heap != nullptr ? heap->name : ""; }

AuroraHeap* aurora_heap_global() {
  static AuroraHeap* sHeap = [] {
    constexpr AuroraHeapDesc desc{AURORA_HEAP_GENERAL, AURORA_HEAP_ZERO, nullptr, "Global"};
    AuroraHeap* heap = aurora_heap_create(&desc);
    if (heap == nullptr) {
      Log.fatal("Failed to create the global heap");
    }
    return heap;
  }();
  return sHeap;
}
