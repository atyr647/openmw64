// Single-threaded stand-ins for std::mutex / std::condition_variable.
//
// libdragon's libstdc++ is built without gthreads, so <mutex> and
// <condition_variable> declare no mutex types. The game runs on one CPU
// thread, so a lock that never contends is the correct implementation, and
// this lets OpenMW's thread-safe code compile unchanged.
#ifndef OPENMW_N64_COMPAT_THREADS_HPP
#define OPENMW_N64_COMPAT_THREADS_HPP

#include <bits/c++config.h>

#ifndef _GLIBCXX_HAS_GTHREADS
#include <cassert>

namespace std
{
    class mutex
    {
    public:
        constexpr mutex() noexcept = default;
        mutex(const mutex&) = delete;
        mutex& operator=(const mutex&) = delete;
        void lock() {}
        bool try_lock() { return true; }
        void unlock() {}
    };

    class recursive_mutex : public mutex
    {
    };

    template <class Lock>
    class unique_lock;

    class condition_variable
    {
    public:
        void notify_one() noexcept {}
        void notify_all() noexcept {}
        template <class Lock, class Predicate>
        void wait(Lock&, Predicate pred)
        {
            // With one thread nobody else can make the predicate true.
            assert(pred());
            (void)pred;
        }
    };
}
#endif

#endif
