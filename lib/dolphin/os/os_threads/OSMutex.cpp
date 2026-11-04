#include "dolphin/os/OSMutex.h"

#include <atomic>
#include <climits>
#include <cstring>

#if defined(__APPLE__)
#include <os/lock.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

void* current_owner() {
  thread_local char sToken;
  return &sToken;
}

std::atomic_ref<void*> mutex_owner(OSMutex* mutex) { return std::atomic_ref(mutex->owner); }

#if defined(__linux__)
void wait_on_word(u32* word, u32 expected) {
  syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0);
}
void wake_word(u32* word, bool all) {
  syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, all ? INT_MAX : 1, nullptr, nullptr, 0);
}
#else
void wait_on_word(u32* word, u32 expected) { std::atomic_ref(*word).wait(expected, std::memory_order_relaxed); }
void wake_word(u32* word, bool all) {
  if (all) {
    std::atomic_ref(*word).notify_all();
  } else {
    std::atomic_ref(*word).notify_one();
  }
}
#endif

// ============================================================================
// Platform lock
// ============================================================================

#if defined(__APPLE__)
static_assert(sizeof(os_unfair_lock) == sizeof(u32));
os_unfair_lock* underlying_lock(OSMutex* mutex) { return reinterpret_cast<os_unfair_lock*>(&mutex->lock.word); }
void lock_native(OSMutex* mutex) { os_unfair_lock_lock(underlying_lock(mutex)); }
bool try_lock_native(OSMutex* mutex) { return os_unfair_lock_trylock(underlying_lock(mutex)); }
void unlock_native(OSMutex* mutex) { os_unfair_lock_unlock(underlying_lock(mutex)); }
#elif defined(_WIN32)
static_assert(sizeof(SRWLOCK) == sizeof(void*));
SRWLOCK* underlying_lock(OSMutex* mutex) { return reinterpret_cast<SRWLOCK*>(&mutex->lock.ptr); }
void lock_native(OSMutex* mutex) { AcquireSRWLockExclusive(underlying_lock(mutex)); }
bool try_lock_native(OSMutex* mutex) { return TryAcquireSRWLockExclusive(underlying_lock(mutex)) != 0; }
void unlock_native(OSMutex* mutex) { ReleaseSRWLockExclusive(underlying_lock(mutex)); }
#else
// A futex lock (Drepper, "Futexes Are Tricky"): 0 unlocked, 1 locked, 2 locked with waiters
void lock_native(OSMutex* mutex) {
  std::atomic_ref<u32> state(mutex->lock.word);
  u32 expected = 0;
  if (state.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
    return;
  }

  while (state.exchange(2, std::memory_order_acquire) != 0) {
    wait_on_word(&mutex->lock.word, 2);
  }
}

bool try_lock_native(OSMutex* mutex) {
  u32 expected = 0;
  return std::atomic_ref<u32>(mutex->lock.word)
      .compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed);
}

void unlock_native(OSMutex* mutex) {
  if (std::atomic_ref<u32>(mutex->lock.word).exchange(0, std::memory_order_release) == 2) {
    wake_word(&mutex->lock.word, false);
  }
}
#endif

void acquired(OSMutex* mutex, void* owner, s32 count) {
  mutex_owner(mutex).store(owner, std::memory_order_relaxed);
  mutex->thread = OSGetCurrentThread();
  mutex->count = count;
}

} // namespace

// ============================================================================
// C API functions
// ============================================================================

void OSInitMutex(OSMutex* mutex) {
  if (!mutex) {
    return;
  }
  OSInitThreadQueue(&mutex->queue);
  mutex->thread = nullptr;
  mutex->count = 0;
  mutex->link.next = nullptr;
  mutex->link.prev = nullptr;
  mutex->owner = nullptr;
  std::memset(&mutex->lock, 0, sizeof(mutex->lock));
}

void OSLockMutex(OSMutex* mutex) {
  if (!mutex) {
    return;
  }
  void* self = current_owner();
  if (mutex_owner(mutex).load(std::memory_order_relaxed) == self) {
    mutex->count++;
    return;
  }
  lock_native(mutex);
  acquired(mutex, self, 1);
}

void OSUnlockMutex(OSMutex* mutex) {
  if (!mutex || mutex_owner(mutex).load(std::memory_order_relaxed) != current_owner()) {
    return;
  }
  if (--mutex->count > 0) {
    return;
  }
  mutex->count = 0;
  mutex->thread = nullptr;
  mutex_owner(mutex).store(nullptr, std::memory_order_relaxed);
  unlock_native(mutex);
}

BOOL OSTryLockMutex(OSMutex* mutex) {
  if (!mutex) {
    return FALSE;
  }
  void* self = current_owner();
  if (mutex_owner(mutex).load(std::memory_order_relaxed) == self) {
    mutex->count++;
    return TRUE;
  }
  if (!try_lock_native(mutex)) {
    return FALSE;
  }
  acquired(mutex, self, 1);
  return TRUE;
}

// ============================================================================
// Internal: unlock all mutexes held by a thread (called on thread exit)
// ============================================================================

void __OSUnlockAllMutex(OSThread* thread) {
  // On GC this walks the thread's mutex queue, which isn't maintained on PC.
  if (!thread) {
    return;
  }
  thread->queueMutex.head = nullptr;
  thread->queueMutex.tail = nullptr;
}

int __OSCheckDeadLock(OSThread* thread) { return 0; }

int __OSCheckMutexes(OSThread* thread) { return 1; }

// ============================================================================
// Condition Variable API
// ============================================================================

void OSInitCond(OSCond* cond) {
  if (!cond) {
    return;
  }
  OSInitThreadQueue(&cond->queue);
  cond->sequence = 0;
}

void OSWaitCond(OSCond* cond, OSMutex* mutex) {
  if (!cond || !mutex) {
    return;
  }

  // Read before releasing the mutex, so a signal sent in between isn't missed
  std::atomic_ref sequence(cond->sequence);
  const u32 observed = sequence.load(std::memory_order_relaxed);

  void* self = current_owner();
  const bool held = mutex_owner(mutex).load(std::memory_order_relaxed) == self;
  const s32 count = mutex->count;
  if (held) {
    mutex->count = 0;
    mutex->thread = nullptr;
    mutex_owner(mutex).store(nullptr, std::memory_order_relaxed);
    unlock_native(mutex);
  }

  while (sequence.load(std::memory_order_acquire) == observed) {
    wait_on_word(&cond->sequence, observed);
  }

  if (held) {
    lock_native(mutex);
    acquired(mutex, self, count);
  }
}

void OSSignalCond(OSCond* cond) {
  if (!cond) {
    return;
  }
  std::atomic_ref(cond->sequence).fetch_add(1, std::memory_order_release);
  wake_word(&cond->sequence, true);
}
