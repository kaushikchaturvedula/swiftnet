#pragma once
#include <cstddef>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace swiftnet::detail
{
  inline void pin_thread_to_core(std::size_t core) noexcept
  {
#if defined(_WIN32)
    DWORD_PTR mask = (static_cast<DWORD_PTR>(1) << core);
    SetThreadAffinityMask(GetCurrentThread(), mask);
#elif defined(__linux__)
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(core, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
#elif defined(__APPLE__)
    thread_affinity_policy_data_t policy{static_cast<int>(core + 1)};
    thread_policy_set(mach_thread_self(),
                      THREAD_AFFINITY_POLICY,
                      reinterpret_cast<int *>(&policy),
                      THREAD_AFFINITY_POLICY_COUNT);
#endif
  }

}
