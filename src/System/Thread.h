#pragma once

#include <Core/Core.h>
#include <thread>
#include <atomic>
#include <algorithm>
#include <execution>

#if !defined(_MSC_VER) && !defined(USE_TBB)
#warning \
    "Vortex::ParallelFor on GCC/Clang requires TBB. Define USE_TBB to silence this warning."
#endif

namespace Vortex {

/// Parallel for using ConcRT on MSVC and TBB on GCC/Clang.
template <typename Func>
void ParallelFor(int begin, int end, Func&& func) {
    struct Iter {
        int i;
        using iterator_category = std::forward_iterator_tag;
        using value_type = int;
        using difference_type = int;
        using pointer = const int*;
        using reference = int;
        int operator*() const { return i; }
        Iter& operator++() {
            ++i;
            return *this;
        }
        Iter operator++(int) {
            auto t = *this;
            ++i;
            return t;
        }
        bool operator==(const Iter& o) const { return i == o.i; }
        bool operator!=(const Iter& o) const { return i != o.i; }
    };
    std::for_each(std::execution::par_unseq, Iter{begin}, Iter{end},
                  std::forward<Func>(func));
}

/// A thread that performs a task, running in the background.
class BackgroundThread {
   public:
    virtual ~BackgroundThread();

    BackgroundThread();

    /// Creates a thread, which calls "exec" once, and then terminates. The
    /// function returns when the thread is created; use "waitUntilDone" to wait
    /// until the thread has terminated.
    void start();

    /// Sets the terminate flag and waits until the thread is terminated. The
    /// terminate flag is only a request; The "exec" function is responsible for
    /// testing the flag and returning.
    void terminate();

    /// Waits until the thread has terminated, after which the function returns.
    void waitUntilDone();

    std::stop_token getStopToken();

    /// Returns true if the thread has terminated, false if the thread is still
    /// running.
    bool isDone() const;

    /// The worker function called by the thread created in "start".
    virtual void exec() = 0;

   private:
    std::jthread thread;
    std::atomic_bool done;
};

};  // namespace Vortex
