#include <aurora/heap.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <thread>
#include <vector>

namespace {

int gOwnerA;
int gOwnerB;
int gStatic;

struct HeapGuard {
  AuroraHeap* heap;
  explicit HeapGuard(AuroraHeapKind kind, void* owner, uint32_t flags = 0) {
    const AuroraHeapDesc desc{kind, flags, owner, "test"};
    heap = aurora_heap_create(&desc);
  }
  ~HeapGuard() { aurora_heap_destroy(heap); }
  HeapGuard(const HeapGuard&) = delete;
  HeapGuard& operator=(const HeapGuard&) = delete;
};

void* lookupOwner(const void* p, AuroraHeap** heap = nullptr, int* lane = nullptr) {
  return aurora_heap_lookup(p, heap, lane);
}

void expectOwnedRange(const void* p, size_t size, void* owner, AuroraHeap* heap) {
  const auto* bytes = static_cast<const uint8_t*>(p);
  for (const size_t offset : {size_t{0}, size_t{1}, size / 2, size - 1}) {
    AuroraHeap* found = nullptr;
    EXPECT_EQ(lookupOwner(bytes + offset, &found), owner) << "offset " << offset << " of " << size;
    EXPECT_EQ(found, heap) << "offset " << offset << " of " << size;
  }
}

AuroraHeapStats stats(AuroraHeap* heap) {
  AuroraHeapStats out;
  aurora_heap_stats(heap, &out);
  return out;
}

} // namespace

TEST(AuroraHeap, ForeignPointersHaveNoOwner) {
  HeapGuard general(AURORA_HEAP_GENERAL, &gOwnerA);
  HeapGuard bump(AURORA_HEAP_BUMP, &gOwnerB);
  ASSERT_NE(aurora_heap_alloc(general.heap, 64, 16, AURORA_HEAP_LANE_HEAD, 0), nullptr);
  ASSERT_NE(aurora_heap_alloc(bump.heap, 64, 16, AURORA_HEAP_LANE_HEAD, 0), nullptr);

  int onStack = 0;
  void* fromMalloc = std::malloc(64);
  AuroraHeap* heap = reinterpret_cast<AuroraHeap*>(1);
  int lane = 7;
  EXPECT_EQ(lookupOwner(&onStack, &heap, &lane), nullptr);
  EXPECT_EQ(heap, nullptr);
  EXPECT_EQ(lane, -1);
  EXPECT_EQ(lookupOwner(&gStatic), nullptr);
  EXPECT_EQ(lookupOwner(fromMalloc), nullptr);
  EXPECT_EQ(lookupOwner(nullptr), nullptr);
  std::free(fromMalloc);
}

TEST(AuroraHeap, GlobalHeapHasNoOwner) {
  void* p = aurora_heap_alloc(aurora_heap_global(), 100, 16, AURORA_HEAP_LANE_HEAD, 0);
  ASSERT_NE(p, nullptr);
  AuroraHeap* heap = nullptr;
  EXPECT_EQ(lookupOwner(p, &heap), nullptr);
  EXPECT_EQ(heap, aurora_heap_global());
  EXPECT_EQ(aurora_heap_free(aurora_heap_global(), p), 100u);
}

TEST(AuroraHeap, GeneralInteriorLookup) {
  HeapGuard guard(AURORA_HEAP_GENERAL, &gOwnerA);
  for (const size_t size : {size_t{1}, size_t{100}, size_t{64 * 1024}, size_t{1024 * 1024}, size_t{2 * 1024 * 1024},
                            size_t{4 * 1024 * 1024}, size_t{8 * 1024 * 1024}}) {
    void* p = aurora_heap_alloc(guard.heap, size, 16, AURORA_HEAP_LANE_HEAD, 0);
    ASSERT_NE(p, nullptr);
    expectOwnedRange(p, size, &gOwnerA, guard.heap);
  }
}

TEST(AuroraHeap, BumpInteriorLookupAcrossChunks) {
  HeapGuard guard(AURORA_HEAP_BUMP, &gOwnerB);
  std::vector<std::pair<void*, size_t>> blocks;
  // Crosses several regular chunks, then a dedicated chunk
  for (int i = 0; i < 2000; ++i) {
    const size_t size = 100 + (i % 7) * 300;
    const int lane = i % 3 == 0 ? AURORA_HEAP_LANE_TAIL : AURORA_HEAP_LANE_HEAD;
    void* p = aurora_heap_alloc(guard.heap, size, 16, lane, 0);
    ASSERT_NE(p, nullptr);
    blocks.emplace_back(p, size);
  }
  for (const size_t size : {size_t{3 * 1024 * 1024}, size_t{8 * 1024 * 1024}}) {
    void* p = aurora_heap_alloc(guard.heap, size, 16, AURORA_HEAP_LANE_HEAD, 0);
    ASSERT_NE(p, nullptr);
    blocks.emplace_back(p, size);
  }
  for (const auto& [p, size] : blocks) {
    expectOwnedRange(p, size, &gOwnerB, guard.heap);
  }

  int lane = -1;
  lookupOwner(blocks[0].first, nullptr, &lane);
  EXPECT_EQ(lane, AURORA_HEAP_LANE_TAIL);
  lookupOwner(blocks[1].first, nullptr, &lane);
  EXPECT_EQ(lane, AURORA_HEAP_LANE_HEAD);
}

TEST(AuroraHeap, BumpSizeHintSizesFirstChunk) {
  const AuroraHeapDesc hinted{AURORA_HEAP_BUMP, 0, &gOwnerA, "hinted", 2048};
  AuroraHeap* heap = aurora_heap_create(&hinted);
  ASSERT_NE(heap, nullptr);
  for (int i = 0; i < 32; ++i) {
    ASSERT_NE(aurora_heap_alloc(heap, 64, 16, AURORA_HEAP_LANE_HEAD, 0), nullptr);
  }
  const size_t oneChunk = stats(heap).reservedBytes;
  EXPECT_GE(oneChunk, 2048u);
  EXPECT_LT(oneChunk, 2048u + 64u);

  // Past the hint, chunks are added as usual
  ASSERT_NE(aurora_heap_alloc(heap, 64, 16, AURORA_HEAP_LANE_HEAD, 0), nullptr);
  EXPECT_GT(stats(heap).reservedBytes, oneChunk);

  // Resetting keeps the hinted first chunk
  aurora_heap_reset(heap, AURORA_HEAP_LANE_ALL);
  EXPECT_EQ(stats(heap).reservedBytes, oneChunk);
  aurora_heap_destroy(heap);

  HeapGuard unhinted(AURORA_HEAP_BUMP, &gOwnerA);
  ASSERT_NE(aurora_heap_alloc(unhinted.heap, 64, 16, AURORA_HEAP_LANE_HEAD, 0), nullptr);
  EXPECT_EQ(stats(unhinted.heap).reservedBytes, 64u * 1024u);

  const AuroraHeapDesc huge{AURORA_HEAP_BUMP, 0, &gOwnerA, "huge", size_t{1} << 30};
  heap = aurora_heap_create(&huge);
  ASSERT_NE(aurora_heap_alloc(heap, 64, 16, AURORA_HEAP_LANE_HEAD, 0), nullptr);
  EXPECT_EQ(stats(heap).reservedBytes, 2u * 1024u * 1024u);
  aurora_heap_destroy(heap);
}

TEST(AuroraHeap, GeneralBlocksAreLimitedTo32Bits) {
  HeapGuard guard(AURORA_HEAP_GENERAL, &gOwnerA);
  EXPECT_EQ(aurora_heap_alloc(guard.heap, size_t{UINT32_MAX} + 1, 16, AURORA_HEAP_LANE_HEAD, 0), nullptr);
  void* p = aurora_heap_alloc(guard.heap, 64, 16, AURORA_HEAP_LANE_HEAD, 0);
  ASSERT_NE(p, nullptr);
  EXPECT_FALSE(aurora_heap_expand(guard.heap, p, size_t{UINT32_MAX} + 1));
  EXPECT_EQ(aurora_heap_block_size(guard.heap, p), 64u);
}

TEST(AuroraHeap, Alignment) {
  HeapGuard general(AURORA_HEAP_GENERAL, &gOwnerA);
  HeapGuard bump(AURORA_HEAP_BUMP, &gOwnerB);
  for (size_t align = 1; align <= 4096; align *= 2) {
    for (AuroraHeap* heap : {general.heap, bump.heap}) {
      for (const int lane : {AURORA_HEAP_LANE_HEAD, AURORA_HEAP_LANE_TAIL}) {
        void* p = aurora_heap_alloc(heap, 24, align, lane, 0);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % std::max<size_t>(align, 16), 0u) << "align " << align;
        std::memset(p, 0xAB, 24);
      }
    }
    void* p = aurora_heap_alloc(general.heap, 3000, align, AURORA_HEAP_LANE_HEAD, 0);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(aurora_heap_block_size(general.heap, p), 3000u);
    EXPECT_EQ(aurora_heap_free(general.heap, p), 3000u);
  }
  // Non power of two alignments round up
  void* p = aurora_heap_alloc(general.heap, 8, 48, AURORA_HEAP_LANE_HEAD, 0);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 64, 0u);
}

TEST(AuroraHeap, ZeroFill) {
  for (const AuroraHeapKind kind : {AURORA_HEAP_GENERAL, AURORA_HEAP_BUMP}) {
    HeapGuard guard(kind, &gOwnerA, AURORA_HEAP_ZERO);
    for (int round = 0; round < 3; ++round) {
      auto* p = static_cast<uint8_t*>(aurora_heap_alloc(guard.heap, 4096, 16, AURORA_HEAP_LANE_HEAD, 0));
      ASSERT_NE(p, nullptr);
      EXPECT_TRUE(std::all_of(p, p + 4096, [](uint8_t b) { return b == 0; }));
      std::memset(p, 0xFF, 4096);
      aurora_heap_reset(guard.heap, AURORA_HEAP_LANE_ALL);
    }
  }
}

TEST(AuroraHeap, DebugFill) {
  HeapGuard guard(AURORA_HEAP_BUMP, &gOwnerA, AURORA_HEAP_DEBUG_FILL);
  auto* p = static_cast<uint8_t*>(aurora_heap_alloc(guard.heap, 64, 16, AURORA_HEAP_LANE_HEAD, 0));
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(std::all_of(p, p + 64, [](uint8_t b) { return b == 0xCD; }));
  aurora_heap_reset(guard.heap, AURORA_HEAP_LANE_ALL);
  // The first head chunk is kept for reuse, so its old contents are still mapped
  EXPECT_TRUE(std::all_of(p, p + 64, [](uint8_t b) { return b == 0xDD; }));
}

TEST(AuroraHeap, StatsAndTags) {
  HeapGuard guard(AURORA_HEAP_GENERAL, &gOwnerA);
  void* a = aurora_heap_alloc(guard.heap, 100, 16, AURORA_HEAP_LANE_HEAD, 1);
  void* b = aurora_heap_alloc(guard.heap, 200, 16, AURORA_HEAP_LANE_TAIL, 2);
  void* c = aurora_heap_alloc(guard.heap, 300, 16, AURORA_HEAP_LANE_HEAD, 1);
  ASSERT_TRUE(a && b && c);

  AuroraHeapStats s = stats(guard.heap);
  EXPECT_EQ(s.usedBytes, 600u);
  EXPECT_EQ(s.blockCount, 3u);
  EXPECT_EQ(s.laneUsedBytes[AURORA_HEAP_LANE_HEAD], 400u);
  EXPECT_EQ(s.laneBlockCount[AURORA_HEAP_LANE_TAIL], 1u);
  EXPECT_GE(s.reservedBytes, 600u);
  EXPECT_EQ(aurora_heap_tag_used(guard.heap, 1), 400u);
  EXPECT_EQ(aurora_heap_tag_used(guard.heap, 2), 200u);

  EXPECT_EQ(aurora_heap_free(guard.heap, a), 100u);
  s = stats(guard.heap);
  EXPECT_EQ(s.usedBytes, 500u);
  EXPECT_EQ(s.peakBytes, 600u);

  EXPECT_TRUE(aurora_heap_expand(guard.heap, c, 50));
  EXPECT_EQ(aurora_heap_block_size(guard.heap, c), 50u);
  EXPECT_EQ(stats(guard.heap).usedBytes, 250u);
  if (aurora_heap_expand(guard.heap, c, 60)) {
    EXPECT_EQ(aurora_heap_block_size(guard.heap, c), 60u);
  }

  aurora_heap_reset(guard.heap, AURORA_HEAP_LANE_ALL);
  s = stats(guard.heap);
  EXPECT_EQ(s.usedBytes, 0u);
  EXPECT_EQ(s.blockCount, 0u);
  EXPECT_EQ(lookupOwner(b), nullptr);
}

TEST(AuroraHeap, ForeignAndWrongHeapFreesAreIgnored) {
  HeapGuard heapA(AURORA_HEAP_GENERAL, &gOwnerA);
  HeapGuard heapB(AURORA_HEAP_GENERAL, &gOwnerB);
  void* p = aurora_heap_alloc(heapB.heap, 64, 16, AURORA_HEAP_LANE_HEAD, 0);
  ASSERT_NE(p, nullptr);

  int onStack = 0;
  void* fromMalloc = std::malloc(64);
  EXPECT_EQ(aurora_heap_free(heapA.heap, nullptr), 0u);
  EXPECT_EQ(aurora_heap_free(heapA.heap, &onStack), 0u);
  EXPECT_EQ(aurora_heap_free(heapA.heap, &gStatic), 0u);
  EXPECT_EQ(aurora_heap_free(heapA.heap, fromMalloc), 0u);
  EXPECT_EQ(aurora_heap_free(heapA.heap, p), 0u);
  EXPECT_EQ(aurora_heap_block_size(heapB.heap, p), 64u);
  std::free(fromMalloc);

  HeapGuard bump(AURORA_HEAP_BUMP, &gOwnerA);
  void* q = aurora_heap_alloc(bump.heap, 64, 16, AURORA_HEAP_LANE_HEAD, 0);
  EXPECT_EQ(aurora_heap_free(bump.heap, q), 0u);
  EXPECT_EQ(stats(bump.heap).blockCount, 1u);
}

TEST(AuroraHeapDeathTest, DoubleFree) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  HeapGuard guard(AURORA_HEAP_GENERAL, &gOwnerA);
  // Keeps the page alive so the second free still resolves to this heap
  void* keep = aurora_heap_alloc(guard.heap, 64, 16, AURORA_HEAP_LANE_HEAD, 0);
  void* p = aurora_heap_alloc(guard.heap, 64, 16, AURORA_HEAP_LANE_HEAD, 0);
  ASSERT_TRUE(keep && p);
  EXPECT_EQ(aurora_heap_free(guard.heap, p), 64u);
#ifdef NDEBUG
  EXPECT_EQ(aurora_heap_free(guard.heap, p), 0u);
#else
  EXPECT_DEATH(aurora_heap_free(guard.heap, p), "");
#endif
  EXPECT_EQ(stats(guard.heap).blockCount, 1u);
}

// The JKRExpHeap::freeTail pattern: dispose the current block, then read the next one, then free the current one.
// Disposing frees another tail block and allocates a new one; both must be handled, and nothing freed is read.
TEST(AuroraHeap, GeneralTailWalkWithReentrantDisposal) {
  HeapGuard guard(AURORA_HEAP_GENERAL, &gOwnerA);
  std::vector<void*> tail;
  for (int i = 0; i < 6; ++i) {
    ASSERT_NE(aurora_heap_alloc(guard.heap, 32, 16, AURORA_HEAP_LANE_HEAD, 0), nullptr);
    tail.push_back(aurora_heap_alloc(guard.heap, 32, 16, AURORA_HEAP_LANE_TAIL, 0));
    ASSERT_NE(tail.back(), nullptr);
  }

  std::set<void*> disposed;
  void* spawned = nullptr;
  size_t size = 0;
  void* block = aurora_heap_next(guard.heap, AURORA_HEAP_LANE_TAIL, nullptr, &size);
  while (block != nullptr) {
    EXPECT_TRUE(disposed.insert(block).second);
    EXPECT_EQ(size, 32u);
    if (block == tail[1]) {
      // A destructor that frees a later tail block, and one that allocates a new tail block
      EXPECT_EQ(aurora_heap_free(guard.heap, tail[3]), 32u);
      spawned = aurora_heap_alloc(guard.heap, 32, 16, AURORA_HEAP_LANE_TAIL, 0);
      ASSERT_NE(spawned, nullptr);
    }
    size_t nextSize = 0;
    void* next = aurora_heap_next(guard.heap, AURORA_HEAP_LANE_TAIL, block, &nextSize);
    EXPECT_EQ(aurora_heap_free(guard.heap, block), 32u);
    block = next;
    size = nextSize;
  }

  EXPECT_EQ(disposed.count(tail[3]), 0u);
  EXPECT_EQ(disposed.count(spawned), 1u);
  EXPECT_EQ(disposed.size(), tail.size());
  const AuroraHeapStats s = stats(guard.heap);
  EXPECT_EQ(s.laneBlockCount[AURORA_HEAP_LANE_TAIL], 0u);
  EXPECT_EQ(s.laneBlockCount[AURORA_HEAP_LANE_HEAD], 6u);
}

TEST(AuroraHeap, LaneReset) {
  for (const AuroraHeapKind kind : {AURORA_HEAP_GENERAL, AURORA_HEAP_BUMP}) {
    HeapGuard guard(kind, &gOwnerA);
    std::vector<void*> head;
    std::vector<void*> tail;
    for (int i = 0; i < 200; ++i) {
      head.push_back(aurora_heap_alloc(guard.heap, 1000, 16, AURORA_HEAP_LANE_HEAD, 0));
      tail.push_back(aurora_heap_alloc(guard.heap, 1000, 16, AURORA_HEAP_LANE_TAIL, 0));
    }

    // Visiting the tail lane covers exactly the tail allocations
    struct Range {
      uintptr_t begin;
      uintptr_t end;
    };
    std::vector<Range> ranges;
    aurora_heap_visit(
        guard.heap, AURORA_HEAP_LANE_TAIL,
        [](void* arg, void* p, size_t size, int lane, uint8_t) {
          EXPECT_EQ(lane, AURORA_HEAP_LANE_TAIL);
          static_cast<std::vector<Range>*>(arg)->push_back(
              {reinterpret_cast<uintptr_t>(p), reinterpret_cast<uintptr_t>(p) + size});
          return true;
        },
        &ranges);
    for (void* p : tail) {
      const auto addr = reinterpret_cast<uintptr_t>(p);
      EXPECT_TRUE(std::any_of(ranges.begin(), ranges.end(), [&](const Range& r) { return addr >= r.begin && addr < r.end; }));
    }
    for (void* p : head) {
      const auto addr = reinterpret_cast<uintptr_t>(p);
      EXPECT_TRUE(std::none_of(ranges.begin(), ranges.end(), [&](const Range& r) { return addr >= r.begin && addr < r.end; }));
    }

    aurora_heap_reset(guard.heap, AURORA_HEAP_LANE_TAIL);
    AuroraHeapStats s = stats(guard.heap);
    EXPECT_EQ(s.laneBlockCount[AURORA_HEAP_LANE_TAIL], 0u);
    EXPECT_EQ(s.laneUsedBytes[AURORA_HEAP_LANE_TAIL], 0u);
    EXPECT_EQ(s.laneBlockCount[AURORA_HEAP_LANE_HEAD], 200u);
    for (void* p : head) {
      EXPECT_EQ(lookupOwner(p), &gOwnerA);
    }

    aurora_heap_reset(guard.heap, AURORA_HEAP_LANE_HEAD);
    s = stats(guard.heap);
    EXPECT_EQ(s.blockCount, 0u);
    EXPECT_EQ(s.usedBytes, 0u);
  }
}

TEST(AuroraHeap, VisitorMayAllocate) {
  HeapGuard guard(AURORA_HEAP_GENERAL, &gOwnerA);
  for (int i = 0; i < 4; ++i) {
    ASSERT_NE(aurora_heap_alloc(guard.heap, 16, 16, AURORA_HEAP_LANE_HEAD, 0), nullptr);
  }
  int visited = 0;
  struct Ctx {
    AuroraHeap* heap;
    int* visited;
  } ctx{guard.heap, &visited};
  aurora_heap_visit(
      guard.heap, AURORA_HEAP_LANE_ALL,
      [](void* arg, void* p, size_t, int, uint8_t) {
        auto* c = static_cast<Ctx*>(arg);
        ++*c->visited;
        aurora_heap_free(c->heap, p);
        return aurora_heap_alloc(c->heap, 16, 16, AURORA_HEAP_LANE_HEAD, 0) != nullptr;
      },
      &ctx);
  EXPECT_EQ(visited, 4);
  EXPECT_EQ(stats(guard.heap).blockCount, 4u);
}

TEST(AuroraHeap, MultiThreadedStress) {
  HeapGuard general(AURORA_HEAP_GENERAL, &gOwnerA);
  HeapGuard bump(AURORA_HEAP_BUMP, &gOwnerB);
  constexpr int kThreads = 8;
  constexpr int kIterations = 20000;
  std::atomic<size_t> live{0};
  std::atomic<size_t> bumpAllocs{0};
  std::atomic<bool> failed{false};

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(t);
      std::vector<void*> mine;
      for (int i = 0; i < kIterations; ++i) {
        if (mine.empty() || rng() % 3 != 0) {
          const size_t size = 1 + rng() % 2048;
          void* p = aurora_heap_alloc(general.heap, size, 16, static_cast<int>(rng() % 2), 0);
          void* q = aurora_heap_alloc(bump.heap, 1 + rng() % 256, 16, static_cast<int>(rng() % 2), 0);
          if (p == nullptr || q == nullptr || aurora_heap_lookup(p, nullptr, nullptr) != &gOwnerA ||
              aurora_heap_lookup(q, nullptr, nullptr) != &gOwnerB) {
            failed = true;
            return;
          }
          std::memset(p, t, size);
          mine.push_back(p);
          ++bumpAllocs;
        } else {
          const size_t index = rng() % mine.size();
          if (aurora_heap_free(general.heap, mine[index]) == 0) {
            failed = true;
            return;
          }
          mine[index] = mine.back();
          mine.pop_back();
        }
      }
      live += mine.size();
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_FALSE(failed);
  EXPECT_EQ(stats(general.heap).blockCount, live.load());
  EXPECT_EQ(stats(bump.heap).blockCount, bumpAllocs.load());
}

TEST(AuroraHeap, DestroyAndResetReleaseEverything) {
  for (int round = 0; round < 20; ++round) {
    HeapGuard general(AURORA_HEAP_GENERAL, &gOwnerA);
    HeapGuard bump(AURORA_HEAP_BUMP, &gOwnerB);
    std::vector<void*> blocks;
    for (int i = 0; i < 100; ++i) {
      blocks.push_back(aurora_heap_alloc(general.heap, 100 + i * 37, 16, AURORA_HEAP_LANE_HEAD, 0));
      blocks.push_back(aurora_heap_alloc(bump.heap, 100 + i * 37, 16, AURORA_HEAP_LANE_TAIL, 0));
    }
    blocks.push_back(aurora_heap_alloc(general.heap, 5 * 1024 * 1024, 16, AURORA_HEAP_LANE_HEAD, 0));
    aurora_heap_reset(general.heap, AURORA_HEAP_LANE_ALL);
    aurora_heap_reset(bump.heap, AURORA_HEAP_LANE_ALL);
    EXPECT_EQ(stats(general.heap).blockCount, 0u);
    EXPECT_EQ(stats(bump.heap).blockCount, 0u);
    // The general heap is usable after a reset
    void* p = aurora_heap_alloc(general.heap, 64, 16, AURORA_HEAP_LANE_HEAD, 0);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(lookupOwner(p), &gOwnerA);
  }
}
