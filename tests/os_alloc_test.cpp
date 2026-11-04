#include <dolphin/os.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace {

constexpr std::size_t kArenaBytes = 64 * 1024;

std::array<std::uint8_t, kArenaBytes> gArena{};

void resetAllocator(int maxHeaps = 8) {
  void* start = gArena.data();
  void* end = gArena.data() + gArena.size();
  ASSERT_EQ(OSInitAlloc(start, end, maxHeaps), start);
  EXPECT_EQ(OSSetCurrentHeap(-1), -1);
}

int gVisitCount;
u32 gVisitBytes;

void countVisit(void*, u32 size) {
  ++gVisitCount;
  gVisitBytes += size;
}

} // namespace

TEST(OSAlloc, InitCreateAllocFreeCheck) {
  resetAllocator();

  OSHeapHandle heap = OSCreateHeap(gArena.data() + 512, gArena.data() + 4096);
  ASSERT_GE(heap, 0);

  EXPECT_EQ(OSSetCurrentHeap(heap), -1);

  void* p = OSAllocFromHeap(heap, 96);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) & 31u, 0u);
  EXPECT_EQ(OSReferentSize(p), 96u);

  // Heaps are not carved out of the region they were created with
  EXPECT_TRUE(p < static_cast<void*>(gArena.data()) || p >= static_cast<void*>(gArena.data() + gArena.size()));

  EXPECT_GE(OSCheckHeap(heap), 0);
  OSFreeToHeap(heap, p);
  EXPECT_GE(OSCheckHeap(heap), 0);
}

TEST(OSAlloc, HeapsAreUnbounded) {
  resetAllocator();

  OSHeapHandle heap = OSCreateHeap(gArena.data() + 0x1000, gArena.data() + 0x2000);
  ASSERT_GE(heap, 0);

  void* p1 = OSAllocFromHeap(heap, 0xD00);
  ASSERT_NE(p1, nullptr);
  void* p2 = OSAllocFromHeap(heap, 0x100000);
  ASSERT_NE(p2, nullptr);
  std::memset(p2, 0xAB, 0x100000);

  OSAddToHeap(heap, gArena.data() + 0x3000, gArena.data() + 0x3800);
  EXPECT_NE(OSAllocFromHeap(heap, 0x400), nullptr);
}

TEST(OSAlloc, AllocationsAreZeroed) {
  resetAllocator();

  OSHeapHandle heap = OSCreateHeap(nullptr, nullptr);
  ASSERT_GE(heap, 0);

  auto* p = static_cast<std::uint8_t*>(OSAllocFromHeap(heap, 256));
  ASSERT_NE(p, nullptr);
  for (int i = 0; i < 256; ++i) {
    ASSERT_EQ(p[i], 0);
  }
}

TEST(OSAlloc, AllocFixedIsUnsupported) {
  resetAllocator();

  OSHeapHandle heap = OSCreateHeap(gArena.data() + 0x2000, gArena.data() + 0x5000);
  ASSERT_GE(heap, 0);
  EXPECT_EQ(OSAllocFixed(gArena.data() + 0x3000, gArena.data() + 0x3400), nullptr);
}

TEST(OSAlloc, SetCurrentHeapReturnsPrevious) {
  resetAllocator();

  OSHeapHandle heapA = OSCreateHeap(gArena.data() + 0x1000, gArena.data() + 0x2000);
  OSHeapHandle heapB = OSCreateHeap(gArena.data() + 0x3000, gArena.data() + 0x4000);
  ASSERT_GE(heapA, 0);
  ASSERT_GE(heapB, 0);
  EXPECT_NE(heapA, heapB);

  EXPECT_EQ(OSSetCurrentHeap(heapA), -1);
  EXPECT_EQ(OSSetCurrentHeap(heapB), heapA);

  void* p = OSAlloc(128);
  ASSERT_NE(p, nullptr);
  OSFree(p);

  EXPECT_GE(OSCheckHeap(heapB), 0);
}

TEST(OSAlloc, FreeIgnoresForeignPointers) {
  resetAllocator();

  OSHeapHandle heapA = OSCreateHeap(nullptr, nullptr);
  OSHeapHandle heapB = OSCreateHeap(nullptr, nullptr);
  ASSERT_GE(heapA, 0);
  ASSERT_GE(heapB, 0);

  void* p = OSAllocFromHeap(heapB, 64);
  ASSERT_NE(p, nullptr);

  int onStack = 0;
  OSFreeToHeap(heapA, &onStack);
  OSFreeToHeap(heapA, gArena.data());
  OSFreeToHeap(heapA, p);
  OSFreeToHeap(heapA, nullptr);
  EXPECT_EQ(OSReferentSize(p), 64u);

  OSFreeToHeap(heapB, p);
  EXPECT_EQ(OSReferentSize(p), 0u);
}

TEST(OSAlloc, HandleLimitAndReuse) {
  resetAllocator(2);

  OSHeapHandle heapA = OSCreateHeap(nullptr, nullptr);
  OSHeapHandle heapB = OSCreateHeap(nullptr, nullptr);
  ASSERT_GE(heapA, 0);
  ASSERT_GE(heapB, 0);
  EXPECT_LT(OSCreateHeap(nullptr, nullptr), 0);

  OSDestroyHeap(heapA);
  EXPECT_EQ(OSCreateHeap(nullptr, nullptr), heapA);
}

TEST(OSAlloc, DestroyHeapInvalidatesHandle) {
  resetAllocator();

  OSHeapHandle heap = OSCreateHeap(gArena.data() + 0x1000, gArena.data() + 0x2000);
  ASSERT_GE(heap, 0);
  ASSERT_NE(OSAllocFromHeap(heap, 64), nullptr);
  EXPECT_EQ(OSSetCurrentHeap(heap), -1);

  OSDestroyHeap(heap);
  EXPECT_LT(OSCheckHeap(heap), 0);
  EXPECT_EQ(OSAllocFromHeap(heap, 64), nullptr);
  EXPECT_EQ(OSSetCurrentHeap(-1), -1);
}

TEST(OSAlloc, InitAllocDestroysExistingHeaps) {
  resetAllocator();

  OSHeapHandle heap = OSCreateHeap(nullptr, nullptr);
  ASSERT_GE(heap, 0);
  ASSERT_NE(OSAllocFromHeap(heap, 64), nullptr);

  resetAllocator();
  EXPECT_LT(OSCheckHeap(heap), 0);
}

TEST(OSAlloc, VisitAllocated) {
  resetAllocator();

  OSHeapHandle heapA = OSCreateHeap(nullptr, nullptr);
  OSHeapHandle heapB = OSCreateHeap(nullptr, nullptr);
  ASSERT_NE(OSAllocFromHeap(heapA, 32), nullptr);
  ASSERT_NE(OSAllocFromHeap(heapA, 64), nullptr);
  ASSERT_NE(OSAllocFromHeap(heapB, 128), nullptr);

  gVisitCount = 0;
  gVisitBytes = 0;
  OSVisitAllocated(countVisit);
  EXPECT_EQ(gVisitCount, 3);
  EXPECT_EQ(gVisitBytes, 32u + 64u + 128u);
}
