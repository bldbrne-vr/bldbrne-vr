// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>
#include "common/alignment.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/threads/sleepq.h"
#include "core/libraries/kernel/threads/thread_state.h"
#include "core/memory.h"
#include "core/tls.h"

namespace Libraries::Kernel {

thread_local Pthread* g_curthread{};

Core::Tcb* TcbCtor(Pthread* thread, int initial);
void TcbDtor(Core::Tcb* oldtls);

ThreadState::ThreadState() {
    // Reserve memory for maximum amount of threads allowed.
    auto* memory = Core::Memory::Instance();
    auto& impl = memory->GetAddressSpace();
    static constexpr u32 ThrHeapSize = Common::AlignUp(sizeof(Pthread) * MaxThreads, 16_KB);
    void* heap_addr{};
    const int ret = memory->MapMemory(&heap_addr, impl.SystemReservedVirtualBase(), ThrHeapSize,
                                      Core::MemoryProt::CpuReadWrite, Core::MemoryMapFlags::NoFlags,
                                      Core::VMAType::File, "ThrHeap");
    ASSERT_MSG(ret == 0, "Unable to allocate thread heap memory {}", ret);
    thread_heap.Initialize(heap_addr, sizeof(Pthread) * MaxThreads);
}

void ThreadState::Collect(Pthread* curthread) {
    boost::container::small_vector<Pthread*, 8> work_list;
    {
        std::scoped_lock lk{thread_list_lock};
        if (gc_list.size() < GcThreshold) {
            return;
        }
        for (auto it = gc_list.begin(); it != gc_list.end();) {
            Pthread* td = *it;
            if (td->tid != TidTerminated ||
                td->pending_deferred_wakes.load(std::memory_order_acquire) != 0) {
                ++it;
                continue;
            }
            work_list.push_back(td);
            it = gc_list.erase(it);
        }
    }
    for (Pthread* td : work_list) {
        // TidTerminated wakes joiners before the host finishes using the record and guest stack.
        td->native_thr->WaitForExit();
        {
            std::scoped_lock lk{thread_list_lock};
            FreeStack(&td->attr);
        }
        Free(curthread, td);
    }
}

void ThreadState::TryCollect(Pthread* thread) {
    if (!thread->ShouldCollect()) {
        thread->lock->unlock();
        return;
    }

    thread->refcount++;
    thread->lock->unlock();
    std::scoped_lock lk{thread_list_lock};
    std::scoped_lock thread_guard{*thread->lock};
    thread->refcount--;
    if (thread->ShouldCollect()) {
        threads.erase(thread);
        gc_list.push_back(thread);
    }
}

Pthread* ThreadState::Alloc(Pthread* curthread) {
    if (curthread != nullptr) {
        Collect(curthread);
    }
    Pthread* thread = thread_heap.Allocate();
    if (thread == nullptr) {
        return nullptr;
    }
    Core::Tcb* tcb = nullptr;
    if (curthread != nullptr) {
        std::scoped_lock lk{tcb_lock};
        tcb = TcbCtor(thread, 0 /* not initial tls */);
    } else {
        tcb = TcbCtor(thread, 1 /* initial tls */);
    }
    if (tcb != nullptr) {
        thread->tcb = tcb;
        thread->sleepqueue = new SleepQueue{};
    } else {
        thread_heap.Free(thread);
        thread = nullptr;
    }
    return thread;
}

void ThreadState::Free(Pthread* curthread, Pthread* thread) {
    if (curthread != nullptr) {
        std::scoped_lock lk{tcb_lock};
        TcbDtor(thread->tcb);
    } else {
        TcbDtor(thread->tcb);
    }
    delete thread->sleepqueue;
    thread_heap.Free(thread);
}

int ThreadState::FindThread(Pthread* thread, const bool include_dead) {
    if (thread == nullptr) {
        return POSIX_EINVAL;
    }
    std::scoped_lock lk{thread_list_lock};
    const auto it = threads.find(thread);
    if (it == threads.end()) {
        return POSIX_ESRCH;
    }
    thread->lock->lock();
    if (!include_dead && thread->state == PthreadState::Dead) {
        thread->lock->unlock();
        return POSIX_ESRCH;
    }
    return 0;
}

int ThreadState::RefAdd(Pthread* thread, bool include_dead) {
    if (thread == nullptr) {
        /* Invalid thread: */
        return POSIX_EINVAL;
    }

    if (int ret = FindThread(thread, include_dead); ret != 0) {
        return ret;
    }

    thread->refcount++;
    thread->lock->unlock();
    return 0;
}

void ThreadState::RefDelete(Pthread* thread) {
    thread->lock->lock();
    thread->refcount--;
    TryCollect(thread);
}

} // namespace Libraries::Kernel
