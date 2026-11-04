#include <dolphin/os/OSMutex.h>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

extern "C" OSThread* OSGetCurrentThread() {
  static OSThread sShared;
  return &sShared;
}

extern "C" void OSInitThreadQueue(OSThreadQueue* queue) {
  queue->head = nullptr;
  queue->tail = nullptr;
}

namespace {

bool try_lock_from_other_thread(OSMutex* mutex) {
  bool locked = false;
  std::thread([&] {
    locked = OSTryLockMutex(mutex);
    if (locked) {
      OSUnlockMutex(mutex);
    }
  }).join();
  return locked;
}

} // namespace

TEST(OSMutex, Recursive) {
  OSMutex mutex;
  OSInitMutex(&mutex);

  OSLockMutex(&mutex);
  OSLockMutex(&mutex);
  EXPECT_TRUE(OSTryLockMutex(&mutex));
  EXPECT_EQ(mutex.count, 3);
  EXPECT_EQ(mutex.thread, OSGetCurrentThread());
  EXPECT_FALSE(try_lock_from_other_thread(&mutex));

  OSUnlockMutex(&mutex);
  OSUnlockMutex(&mutex);
  EXPECT_FALSE(try_lock_from_other_thread(&mutex));
  OSUnlockMutex(&mutex);
  EXPECT_EQ(mutex.count, 0);
  EXPECT_EQ(mutex.thread, nullptr);
  EXPECT_TRUE(try_lock_from_other_thread(&mutex));
}

TEST(OSMutex, UnlockByOtherThreadIsIgnored) {
  OSMutex mutex;
  OSInitMutex(&mutex);
  OSLockMutex(&mutex);
  std::thread([&] { OSUnlockMutex(&mutex); }).join();
  EXPECT_EQ(mutex.count, 1);
  EXPECT_FALSE(try_lock_from_other_thread(&mutex));
  OSUnlockMutex(&mutex);
  EXPECT_TRUE(try_lock_from_other_thread(&mutex));
}

TEST(OSMutex, ZeroInitializedIsUnlocked) {
  static OSMutex sMutex;
  OSLockMutex(&sMutex);
  EXPECT_FALSE(try_lock_from_other_thread(&sMutex));
  OSUnlockMutex(&sMutex);
  EXPECT_TRUE(try_lock_from_other_thread(&sMutex));
}

TEST(OSMutex, MutualExclusionAcrossThreadsSharingAnOSThread) {
  constexpr int kThreads = 8;
  constexpr int kIterations = 200000;
  OSMutex mutex;
  OSInitMutex(&mutex);
  long long counter = 0;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < kIterations; ++i) {
        OSLockMutex(&mutex);
        if (i % 3 == 0) {
          OSLockMutex(&mutex);
          counter = counter + 1;
          OSUnlockMutex(&mutex);
        } else {
          counter = counter + 1;
        }
        OSUnlockMutex(&mutex);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(counter, static_cast<long long>(kThreads) * kIterations);
}

TEST(OSCond, WaitReleasesAndRestoresRecursiveLock) {
  OSMutex mutex;
  OSCond cond;
  OSInitMutex(&mutex);
  OSInitCond(&cond);
  bool ready = false;
  std::atomic<bool> waiting = false;

  std::thread waiter([&] {
    OSLockMutex(&mutex);
    OSLockMutex(&mutex);
    waiting = true;
    while (!ready) {
      OSWaitCond(&cond, &mutex);
    }
    EXPECT_EQ(mutex.count, 2);
    OSUnlockMutex(&mutex);
    OSUnlockMutex(&mutex);
  });

  while (!waiting) {
    std::this_thread::yield();
  }
  // The waiter releases the mutex while it waits, so this doesn't deadlock
  OSLockMutex(&mutex);
  ready = true;
  OSSignalCond(&cond);
  OSUnlockMutex(&mutex);
  waiter.join();
  EXPECT_TRUE(try_lock_from_other_thread(&mutex));
}

TEST(OSCond, PingPongDoesNotLoseSignals) {
  constexpr int kRounds = 20000;
  OSMutex mutex;
  OSCond cond;
  OSInitMutex(&mutex);
  OSInitCond(&cond);
  int turn = 0;

  auto player = [&](int me) {
    for (int round = 0; round < kRounds; ++round) {
      OSLockMutex(&mutex);
      while (turn % 2 != me) {
        OSWaitCond(&cond, &mutex);
      }
      ++turn;
      OSSignalCond(&cond);
      OSUnlockMutex(&mutex);
    }
  };
  std::thread a(player, 0);
  std::thread b(player, 1);
  a.join();
  b.join();
  EXPECT_EQ(turn, 2 * kRounds);
}
