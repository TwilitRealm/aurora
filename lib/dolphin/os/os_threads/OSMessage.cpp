#include "dolphin/os/OSMessage.h"

#include "OSThreadShutdown.hpp"

#include <mutex>
#include <unordered_map>

// Side-table for native synchronization per OSMessageQueue
struct PCMessageQueueData {
    std::mutex mtx;
    std::condition_variable cvSend;     // Notified when space becomes available
    std::condition_variable cvReceive;  // Notified when a message arrives
};

// Lazy-initialized to avoid DLL static init crashes
static std::mutex& GetMsgQueueMapMutex() {
    static std::mutex mtx;
    return mtx;
}
static std::unordered_map<OSMessageQueue*, std::unique_ptr<PCMessageQueueData>>& GetMsgQueueMap() {
    static std::unordered_map<OSMessageQueue*, std::unique_ptr<PCMessageQueueData>> map;
    return map;
}

static PCMessageQueueData& GetMsgQueueData(OSMessageQueue* mq) {
    std::lock_guard<std::mutex> lock(GetMsgQueueMapMutex());
    auto& map = GetMsgQueueMap();
    auto it = map.find(mq);
    if (it == map.end()) {
        auto result = map.emplace(mq, std::make_unique<PCMessageQueueData>());
        return *result.first->second;
    }
    return *it->second;
}

static void ClearMsgQueueMap() {
    std::lock_guard<std::mutex> lock(GetMsgQueueMapMutex());
    auto& map = GetMsgQueueMap();
    for (auto & [_, value] : map) {
        value->cvReceive.notify_all();
        value->cvSend.notify_all();
    }
    map.clear();
}

void OSInitMessageQueue(OSMessageQueue* mq, OSMessage* msgArray, s32 msgCount) {
    if (!mq) return;
    mq->queueSend.head = mq->queueSend.tail = nullptr;
    mq->queueReceive.head = mq->queueReceive.tail = nullptr;
    mq->msgArray   = msgArray;
    mq->msgCount   = msgCount;
    mq->firstIndex = 0;
    mq->usedCount  = 0;
    GetMsgQueueData(mq);  // Ensure side-table entry exists
}

int OSSendMessage(OSMessageQueue* mq, void* msg, s32 flags) {
    if (!mq) return 0;

    PCMessageQueueData& data = GetMsgQueueData(mq);
    std::unique_lock<std::mutex> lock(data.mtx);

    if (mq->usedCount >= mq->msgCount) {
        if (flags == OS_MESSAGE_NOBLOCK) return 0;
        // BLOCK: wait until space is available
        data.cvSend.wait(lock, [mq] { return mq->usedCount < mq->msgCount || aurora::os::IsShuttingDown; });
    }
    if (aurora::os::IsShuttingDown) {
        return 0;
    }

    s32 idx = (mq->firstIndex + mq->usedCount) % mq->msgCount;
    ((OSMessage*)mq->msgArray)[idx] = msg;
    mq->usedCount++;

    data.cvReceive.notify_one();
    return 1;
}

BOOL OSReceiveMessage(OSMessageQueue* mq, OSMessage* msg, s32 flags) {
    if (!mq) return 0;

    PCMessageQueueData& data = GetMsgQueueData(mq);
    std::unique_lock<std::mutex> lock(data.mtx);

    if (mq->usedCount == 0) {
        if (flags == OS_MESSAGE_NOBLOCK) return 0;
        // BLOCK: wait until a message arrives
        data.cvReceive.wait(lock, [mq] { return mq->usedCount > 0 || aurora::os::IsShuttingDown; });
    }
    if (aurora::os::IsShuttingDown) {
        return 0;
    }

    if (msg) {
        *(OSMessage*)msg = ((OSMessage*)mq->msgArray)[mq->firstIndex];
    }
    mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
    mq->usedCount--;

    data.cvSend.notify_one();
    return 1;
}

int OSJamMessage(OSMessageQueue* mq, void* msg, s32 flags) {
    if (!mq) return 0;

    PCMessageQueueData& data = GetMsgQueueData(mq);
    std::unique_lock<std::mutex> lock(data.mtx);

    if (mq->usedCount >= mq->msgCount) {
        if (flags == OS_MESSAGE_NOBLOCK) return 0;
        // BLOCK: wait until space is available
        data.cvSend.wait(lock, [mq] { return mq->usedCount < mq->msgCount || aurora::os::IsShuttingDown; });
    }
    if (aurora::os::IsShuttingDown) {
        return 0;
    }

    // Jam inserts at the front of the queue
    mq->firstIndex = (mq->firstIndex - 1 + mq->msgCount) % mq->msgCount;
    ((OSMessage*)mq->msgArray)[mq->firstIndex] = msg;
    mq->usedCount++;

    data.cvReceive.notify_one();
    return 1;
}
