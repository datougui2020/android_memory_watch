// MIT License
//
// Copyright (c) 2019 larrinluo
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Created by larrin luo on 2020-01-23.
//

#include "deadlock.h"
#include "../utils/utils.h"
#include <android/log.h>

#define TAG "DEAD_LOCK"

#define log_d(tag, format, ...) __android_log_print(ANDROID_LOG_DEBUG, tag, format, ##__VA_ARGS__)
#define log_w(tag, format, ...) __android_log_print(ANDROID_LOG_WARN, tag, format, ##__VA_ARGS__)
#define log_e(tag, format, ...) __android_log_print(ANDROID_LOG_ERROR, tag, format, ##__VA_ARGS__)

std::vector<BlockedMutex> DeadLock::sBlockedMutexes;
std::map<void *, LockInfo> DeadLock::sLockMap;
int DeadLock::sdkVersion = 0;
pthread_mutex_t DeadLock::sLock;
pthread_mutex_t DeadLock::sBlockedMutexLock;

#define _lock_(lock)       GotHook::origin_pthread_mutex_lock((lock))
#define _unlock_(lock)     GotHook::origin_pthread_mutex_unlock((lock))

#define _lock_map_()       GotHook::origin_pthread_mutex_lock(&sLock)
#define _unlock_map_()     GotHook::origin_pthread_mutex_unlock(&sLock)

int DeadLock::my_pthread_mutex_init(PthreadMutexInitContext &context) {

    int count;
    _lock_map_();
    LockInfo lock;
    lock.type = MUTEX_TYPE::MUTEX;
    lock.lock = context.mutex;

    int type;
    pthread_mutexattr_gettype(context.attr, &type);
    lock.recursive = type == PTHREAD_MUTEX_RECURSIVE;
    sLockMap[context.mutex] = lock;
    count = sLockMap.size();
    _unlock_map_();

    return 0;
}

int DeadLock::my_pthread_mutex_destroy(PthreadMutexDestroyContext &context) {

    int count;
    _lock_map_();
    sLockMap.erase(context.mutex);
    count = sLockMap.size();
    _unlock_map_();

    return 0;
}

int DeadLock::my_pthread_mutex_lock(PthreadMutexLockContext &context) {

    LockInfo *pLock = getLock(context.mutex);
    if (pLock) {
        context.retVal = try_lock(*pLock);
        return 1;
    } else {
        return 0;
    }

}

int DeadLock::my_pthread_mutex_unlock(PthreadMutexUnlockContext &context) {
    LockInfo *pLock = getLock(context.mutex);

    if (pLock) {
        unlock(*pLock);
    }

    return 0;
}

int DeadLock::my_pthread_rwlock_init(PthreadRwLockInitContext &context) {
    return 0;
}

int DeadLock::my_pthread_rwlock_destroy(PthreadRwLockDestroyContext &context) {
    return 0;
}

int DeadLock::my_pthread_rwlock_rdlock(PthreadRWLockRDLockContext &context) {
    return 0;
}


int DeadLock::my_pthread_rwlock_wrlock(PthreadRWLockWRLockContext &context) {
    return 0;
}

int DeadLock::my_pthread_rwlock_unlock(PthreadRWLockUnlockContext &context) {
    return 0;
}

void DeadLock::registerHooks(int sdkVersion, const char *targetSo, const char *path) {

    DeadLock::sdkVersion = sdkVersion;

    {
        pthread_mutex_t mutex;
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutex_init(&mutex, &attr);

        struct timespec tout;
        // Must be CLOCK_REALTIME
        clock_gettime(CLOCK_REALTIME, &tout);
        pthread_mutex_timedlock(&mutex, &tout);

        pthread_mutex_destroy(&mutex);
        pthread_mutexattr_destroy(&attr);
    }

    GotHook::setDeadLockTargetSo(targetSo);

    GotHook::add_pthread_mutex_lock_hook(my_pthread_mutex_lock);
    GotHook::add_pthread_mutex_unlock_hook(my_pthread_mutex_unlock);
    GotHook::add_pthread_mutex_init_hook(my_pthread_mutex_init);
    GotHook::add_pthread_mutex_destroy_hook(my_pthread_mutex_destroy);

    GotHook::add_pthread_rwlock_init_hook(my_pthread_rwlock_init);
    GotHook::add_pthread_rwlock_destory_hook(my_pthread_rwlock_destroy);
    GotHook::add_pthread_rwlock_rdlock_hook(my_pthread_rwlock_rdlock);
    GotHook::add_pthread_rwlock_wdlock_hook(my_pthread_rwlock_wrlock);
    GotHook::add_pthread_rwlock_unlock_hook(my_pthread_rwlock_unlock);
}

void DeadLock::checkHooks() {
    if (GotHook::origin_pthread_mutex_lock == NULL ||
        GotHook::origin_pthread_mutex_unlock == NULL ||
        GotHook::origin_pthread_mutex_init == NULL ||
        GotHook::origin_pthread_mutex_destroy == NULL) {
        log_w(TAG, "dead lock hooks failed ");
        return;
    }

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    GotHook::origin_pthread_mutex_init(&sLock, &attr);
    GotHook::origin_pthread_mutex_init(&sBlockedMutexLock, &attr);
    pthread_mutexattr_destroy(&attr);
}

// internal methods
int DeadLock::try_lock(LockInfo &lock) {

    long tm = get_relative_millisecond();
    int gate = 1000;
    int tid = get_tid();

    bool isMainThread = is_main_thread(tid);
    if (isMainThread) {
        gate = 500;
    }

    bool selfLockDetected = false;  // 线程被不可重入锁自锁
    bool isDeadLocked = false;      // 线程是否已经被死锁

    int err = 0;
    bool needRemoveFromDeadLock = false;

    while ((err = timed_lock(lock, gate)) != 0) {

        long enter_time = lock.enter_time;
        long owner = lock.owner;

        long dt = get_relative_millisecond() - tm;
        if (dt > gate) { // 超时警告

            if (!isDeadLocked && !selfLockDetected) { // 如果已经死锁， 不重复输出下面的信

                char thread_name[64] = {0};
                log_w(  "MUTEX:Block",
                        "[tid %d/%s] blocked by [tid %d] on lock(%p) %dms, holds: %dms, err: %d",
                        tid,
                        get_thread_name(thread_name, tid),
                        owner,
                        lock.lock,
                        (int) dt,
                        enter_time == 0? 0 : (int) (get_relative_millisecond() - enter_time),
                        err);

                if (isMainThread) {
                    gate = gate < 1000? 300: 1000;
                }
            }
        }

        if (lock.owner == tid && !lock.recursive) {
            // 线程在不可重入锁上把自锁了
            if (!selfLockDetected) {
                selfLockDetected = true;

                char tname[64];
                char buf[512];
                std::string log;

                // 输出死锁线程的callstak
                std::string callstack;
                get_full_callstack(callstack, 15);

                snprintf(buf,
                         256,
                         "[DEAD LOCK] Thread slef locked by none"
                         " recursive mutex"
                         " ------------------------------>\n\n");
                log += buf;

                if (isMainThread) {
                    log += "ARN warning: Main Thread dead locked!!!!\n";
                }

                snprintf(
                        buf,
                        256,
                        "Deadlock callstack, thread: %d (%s) :\n",
                        tid,
                        tname);

                log += buf;

                log += callstack;
                log += "[End] Dead Lock";

                log_e("MUTEX:Deadlock", "%s", log.c_str());
            }
        }

        if (!needRemoveFromDeadLock) { // 加入一次就够了
            collect_blocked_items(tid, lock);
        }

        std::vector<std::vector<BlockedMutex>> deadlock_links;
        find_dead_locks(tid, deadlock_links);

        std::string callstack;

        if (!deadlock_links.empty()) {
            get_full_callstack(callstack, 15);
            isDeadLocked = true;
            gate = -1; // 已经死锁， 不必在重复检测了
        }

        for (auto& link: deadlock_links) {
            // 为减少调用栈深度，backtrace获取更多有用调用栈信息，
            // 下面的代码不提取到单独方法中
            char tname[64];
            char buf[256];
            get_thread_name(tname, tid);

            // create log string for report
            std::string log;

            log = ("[Warning] Dead Lock found --------------------->\n");
            if (isMainThread) {
                log += "ARN warning: Main Thread dead locked!!!!\n";
            }

            for (auto & lock: link) {
                snprintf(
                        buf,
                        256,
                        "[thread %d] blocked by [thread %d] on lock(%p) >>>> \n",
                        lock.blocked_thread,
                        lock.owner_thread,
                        lock.mutex);

                log += buf;
            }

            snprintf(buf,
                     256,
                     "Deadlock callstack, thread: %ld (%s) :\n",
                     tid,
                     tname);

            log += buf;

            log += callstack;
            log += "[End] Dead Lock";

            log_e("MUTEX:Deadlock", "%s", log.c_str());
        }

        needRemoveFromDeadLock = true;
    }

    lock.deep++;
    lock.owner = tid;
    if (lock.deep == 1) {
        lock.enter_time = get_relative_millisecond();
    }

    if (needRemoveFromDeadLock) {
        remove_blocked_item(tid);
    }

    return 0;
}

int DeadLock::unlock(LockInfo &lock) {
    int tid = get_tid();
    if (lock.owner == tid) {
        lock.deep--;
        if (lock.deep == 0) {
            lock.owner = 0;
            lock.enter_time = 0;
        }
    } else {
        log_w("MUTEX:Unlock",
                 "[tid %d] unlock by none owner thread: %d", lock.owner);
    }

    return 0;
}

int DeadLock::timed_lock(LockInfo& lock, int timeout) {

    int ret = 0;
    if (lock.type == MUTEX_TYPE::MUTEX) {

        if (timeout < 0) {
            return GotHook::origin_pthread_mutex_lock((pthread_mutex_t *)lock.lock);
        }

        #if !(__ANDROID__) || __ANDROID_API__ >= 21
            struct timespec tout;
            // Must be CLOCK_REALTIME
            clock_gettime(CLOCK_REALTIME, &tout);
            tout.tv_nsec += timeout * 1000000;
            if (tout.tv_nsec > 1000000000L) {
                tout.tv_sec += tout.tv_nsec / 1000000000L;
                tout.tv_nsec %= 1000000000L;
            }
            ret = pthread_mutex_timedlock((pthread_mutex_t *)lock.lock, &tout);
        #else
            ret = pthread_mutex_lock_timeout_np((pthread_mutex_t *)lock.lock, timeout);
        #endif
    }

    return ret;
}

void DeadLock::collect_blocked_items(int tid, LockInfo &lock) {
    _lock_(&sBlockedMutexLock);

    BlockedMutex blockItem;
    blockItem.mutex = lock.lock;
    blockItem.type = lock.type;
    blockItem.owner_thread = lock.owner;
    blockItem.blocked_thread = tid;
    sBlockedMutexes.push_back(blockItem);

    _unlock_(&sBlockedMutexLock);
}

void DeadLock::remove_blocked_item(int blocked_thread) {
    _lock_(&sBlockedMutexLock);

    for (auto it = sBlockedMutexes.begin(); it != sBlockedMutexes.end(); it++) {
        if (it->blocked_thread == blocked_thread) {
            sBlockedMutexes.erase(it);
            break;
        }
    }

    _unlock_(&sBlockedMutexLock);
}

void DeadLock::find_dead_locks(int blocked_thread, std::vector<std::vector<BlockedMutex>> &deadlock_links, bool force) {

    _lock_(&sBlockedMutexLock);

    if (sBlockedMutexes.size() < 2) {
        _unlock_(&sBlockedMutexLock);
        return;
    }

    // 如果blocked_thread发生了死锁，它的block点一定在死锁环路上
    // 因此问题演变为从blocked_thread的block点开始查找有无有向图环路，
    // 这样大大简化查找逻辑，效率更好
    // 下面开始收集blocked_thread线程的block点。
    // 单个线程的block点在detect之前一定已经收集完毕，不会出现遗漏
    std::vector<BlockedMutex *> blockPoints;
    for (auto &item: sBlockedMutexes) {
        if (item.blocked_thread == blocked_thread) {
            if (!force && item.dumped) { // 已经dump过了
                _unlock_(&sBlockedMutexLock);
                return;
            } else {
                blockPoints.push_back(&item);
            }
        }
    }

    // 由blocked_thread的block点开始查找死锁链
    for (auto &item : blockPoints) {
        std::vector<BlockedMutex *> items;
        BlockedMutex *pCurrentItem = item;
        items.push_back(pCurrentItem);

        BlockedMutex * pNext = NULL;
        while ((pNext = find_next_jump(pCurrentItem)) != NULL) {
            items.push_back(pNext);

            if (pNext->owner_thread == blocked_thread) { // 发现死锁回路
                deadlock_links.push_back(std::vector<BlockedMutex>());
                std::vector<BlockedMutex> &mutexes =
                        deadlock_links[deadlock_links.size() - 1];

                // copy死锁链路
                for (auto &blockItem: items) {
                    mutexes.push_back(*blockItem);
                }

                // 一个block点最多一个死锁线路
                break;
            }

            pCurrentItem = pNext;
        }
    }

    if (deadlock_links.size() > 0) {
        if (deadlock_links.size() >= blockPoints.size()) {
            // blocked_thread找到了死锁链路，标记输出状态，以后不再检测了
            for (auto &item : sBlockedMutexes) {
                if (item.blocked_thread == blocked_thread) {
                    item.dumped = true;
                }
            }
        } else {
            // blocked_thread存在多个block点，说明这是一个读写锁的写锁，
            // 或其他类似性质的锁走到这里说明由于线程调度，
            // 还有其他阻塞blocked_thread线程的mutex信息还没收集到
            // 因此直接清空已经收集到的死锁链路，
            // 待所有信息收集完整时再收集所有的死锁链路
            deadlock_links.clear();
        }
    }

    _unlock_(&sBlockedMutexLock);
}

BlockedMutex *DeadLock::find_next_jump(BlockedMutex *from_mutex)
{
    if (from_mutex == NULL) {
        return NULL;
    }

    for (auto &item : sBlockedMutexes) {
        if (from_mutex == &item) {
            continue;
        }

        if (from_mutex->owner_thread == item.blocked_thread) {
            return &item;
        }
    }

    return NULL;
}
