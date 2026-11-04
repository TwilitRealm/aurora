#ifndef AURORA_HEAP_H
#define AURORA_HEAP_H

/**
 * Tracked heaps for game allocators (JKRHeap, OSAlloc), built on mimalloc.
 *
 * Heaps are unbounded and can be released in bulk. Any pointer can be mapped back to the heap that owns it.
 * Thread-safe except for reset and destroy.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AuroraHeap AuroraHeap;

typedef enum AuroraHeapKind {
  /**
   * Individual frees. Each heap is backed by its own mimalloc heap, and every block has a 32-byte header.
   */
  AURORA_HEAP_GENERAL,
  /**
   * No individual frees. Allocations are bumped from chunks in two lanes (head and tail) that are reset as a whole.
   * Chunks for all bump heaps come from one shared mimalloc heap, so many small bump heaps stay cheap.
   */
  AURORA_HEAP_BUMP,
} AuroraHeapKind;

enum {
  /** Zero every allocation. */
  AURORA_HEAP_ZERO = 1u << 0,
  /** Fill allocations with 0xCD and freed or reset memory with 0xDD. */
  AURORA_HEAP_DEBUG_FILL = 1u << 1,
};

enum {
  AURORA_HEAP_LANE_HEAD = 0,
  AURORA_HEAP_LANE_TAIL = 1,
  AURORA_HEAP_LANE_ALL = -1,
};

typedef struct AuroraHeapDesc {
  AuroraHeapKind kind;
  /** AURORA_HEAP_* flags. */
  uint32_t flags;
  /** Opaque owner returned by aurora_heap_lookup. */
  void* owner;
  /** Optional name for debugging. */
  const char* name;
  /**
   * Bump heaps: the expected size of each lane, which sizes its first chunk (clamped to 2 MiB). Allocating past it
   * adds chunks as usual. 0 selects a 64 KiB first chunk.
   */
  size_t sizeHint;
} AuroraHeapDesc;

typedef struct AuroraHeapStats {
  /** Payload bytes live. */
  size_t usedBytes;
  /** Live allocations. */
  size_t blockCount;
  /** Payload bytes live per lane. */
  size_t laneUsedBytes[2];
  /** Live allocations per lane. */
  size_t laneBlockCount[2];
  /** Highest usedBytes since creation. */
  size_t peakBytes;
  /** Chunk bytes (bump heaps), or mimalloc size classes of live blocks, headers included (general heaps). */
  size_t reservedBytes;
} AuroraHeapStats;

/**
 * Called by aurora_heap_visit for each block (general heaps) or chunk (bump heaps). Return false to stop.
 */
typedef bool (*AuroraHeapVisitor)(void* arg, void* p, size_t size, int lane, uint8_t tag);

AuroraHeap* aurora_heap_create(const AuroraHeapDesc* desc);
void aurora_heap_destroy(AuroraHeap* heap);

/**
 * Releases every allocation in a lane, or in both lanes with AURORA_HEAP_LANE_ALL.
 */
void aurora_heap_reset(AuroraHeap* heap, int lane);

/**
 * Allocates size bytes aligned to align (a power of two; at least 16 is used) from a lane.
 * The tag is a free-form byte reported by visits and aurora_heap_tag_used.
 * Returns NULL only when the system is out of memory.
 */
void* aurora_heap_alloc(AuroraHeap* heap, size_t size, size_t align, int lane, uint8_t tag);

/**
 * Frees a block of a general heap and returns its size. NULL, unknown pointers and pointers into other heaps are
 * ignored and return 0. Bump heaps ignore individual frees.
 */
size_t aurora_heap_free(AuroraHeap* heap, void* p);

/**
 * Resizes a live block of a general heap in place. Shrinking always succeeds; growing succeeds only if the
 * underlying allocation already has room.
 */
bool aurora_heap_expand(AuroraHeap* heap, void* p, size_t newSize);

/**
 * Size of a live block of a general heap, or 0 if p is not a live block of this heap.
 */
size_t aurora_heap_block_size(AuroraHeap* heap, const void* p);

/**
 * Finds the heap that owns p. Returns that heap's owner and optionally the heap and lane
 * (-1 if unknown, which is always the case for general heaps).
 * Returns NULL for memory outside of any known heap.
 */
void* aurora_heap_lookup(const void* p, AuroraHeap** heap, int* lane);

void aurora_heap_stats(const AuroraHeap* heap, AuroraHeapStats* out);

/**
 * Payload bytes live with the given tag. General heaps only.
 */
size_t aurora_heap_tag_used(const AuroraHeap* heap, uint8_t tag);

/**
 * Visits blocks (general heaps) or chunks with their used size (bump heaps) of lane in allocation order.
 * The visitor may allocate or free memory from the heap, but must not destroy it.
 */
void aurora_heap_visit(AuroraHeap* heap, int lane, AuroraHeapVisitor visitor, void* arg);

/**
 * Returns the next live block and its size after prev (NULL for the first) in allocation order.
 * prev must be live; if it is not, the walk restarts from the first block.
 * General heaps only.
 */
void* aurora_heap_next(AuroraHeap* heap, int lane, const void* prev, size_t* size);

void aurora_heap_set_name(AuroraHeap* heap, const char* name);
const char* aurora_heap_get_name(const AuroraHeap* heap);

/**
 * A process-lifetime general heap with no owner, for arena and orphan allocations.
 */
AuroraHeap* aurora_heap_global(void);

#ifdef __cplusplus
}
#endif

#endif
