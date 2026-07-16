#pragma once
#include <atomic>
#include <cassert>
#include <cstdlib>
#include "RenderJob.hpp"
#include "../../macro_util.hpp"

namespace engine::job {

/**
 * @brief: RenderJobStack: fixed size non-blocking SPMC stack data structure
 * * This class implements a fixed size stack like data structure for the MandelbrotEngine
 * frame generation schedule. The structure exposes methods which allow a single concurrent
 * thread to add a new job to the stack or multiple concurrent threads to get the last added
 * job.
 * * When adding the new job, the previous one gets aborted, which would allow worker threads
 * to always detect the latest added job to skip not completed old frames in favor of newer
 * ones. The stack exposes a try_push method which enqueues the provided job only if the selected
 * slot contains a job which is done. A job is considered done when it is completed or it was
 * previously aborted and all threads which could be working on the job observed (at least one time)
 * that said job was aborted. This results in a very simple backpressure effect in case the frame-rate
 * of the enqueuer thread greatly outmatches the rate of completion of a job given a static number
 * of workers.
 * * The structure allows threads to passively wait (via native C++20 std::atomic::wait) using a
 * dedicated versioning state bell, dropping CPU usage to exactly 0% when no jobs are active.
 */
class RenderJobStack {
    // Tracks the absolute chronological generation of the most recently pushed job
    CACHE_ALIGN std::atomic_uint64_t tail{0};
    CACHE_PAD(std::atomic_uint64_t)

    const size_t size_;
    RenderJob* const buffer;

public:
    /**
     * @brief constructor for RenderJobStack
     * @warning: may throw std::bad_alloc
     */
    RenderJobStack(size_t buf_size):
        size_{buf_size},
        buffer{new RenderJob[buf_size]}
    {
        assert(buf_size != 0 && "RenderJobStack: buffer size must be non-negative");
    }

    /**
     * @brief: destructor for RenderJobStack
     * simply deallocates the inner buffer
     * @warning: doesn't check if any thread holds a reference to any object of the pool
     */
    ~RenderJobStack() {
        delete[] buffer;
    }

    RenderJobStack(const RenderJobStack&) = delete;
    RenderJobStack& operator=(const RenderJobStack&) = delete;

    /**
     * @brief: tries to add a new job to the stack
     * @tparam: type of the JobState: either RenderJob::ETAJob or RenderJob::PTBJob
     * @param: RenderJob::JobSpecs specifics of the job
     * @returns true if the job was stacked false otherwise
     * @note: if the method returns true, the previous current job is to be considered aborted
     * @note: this method is to be considered wait-free and is not MT-Safe
     */
    template<typename T, typename... Args>
    bool try_push(RenderJob::JobSpecs specs, Args&&... args) {
        const uint64_t curr_tail = tail.load(std::memory_order_acquire);
        RenderJob& next_slot_job = buffer[(curr_tail + 1) % size_];
        //the next slot cannot be written because its still accessed
        if(!next_slot_job.safeToRecycle()) { 
            return false;
        }
        //write the new data in isolation
        next_slot_job.emplace<T>(specs, curr_tail + 1,std::forward<Args>(args)...);
        //abort the current job
        RenderJob& curr_job = buffer[curr_tail % size_];
        curr_job.abort();
        //publish the job
        tail.store(curr_tail + 1, std::memory_order_release);
        //unblock workers that we waiting for a new job
        tail.notify_all();
        return true;
    }

    /**
     * @brief: get the last added job
     * @returns: a modifiable RenderJob reference
     * @note: this method is MT-Safe
     */
    inline RenderJob& get_latest_job() const {
        return buffer[tail.load(std::memory_order_acquire) % size_];
    }

    /**
     * @brief: get the status of the last added job
     * @returns: true if the last job added is done (see RenderJob::done())
     * @note: this method is wait-free and MT-Safe
     */
    inline bool is_latest_done() const {
        return buffer[tail.load(std::memory_order_acquire) % size_].done();
    }

    /**
     * @brief: checks if a job is the latest one
     * @param: immutable RenderJob reference
     * @returns true: if the pointer of the argument matches the pointer of the last added job
     * @note: the pointer comparison is guaranteed because the underlying buffer never resizes
     */
    inline bool is_latest(const RenderJob& j) const {
        const RenderJob* const latest_ptr = buffer + (tail.load(std::memory_order_acquire) % size_);
        return (&j) == latest_ptr;
    }

    /**
     * @brief: check if a stamp is the latest one
     * @param: uint64_t stamp
     * @returns: true if the argument stamp is bitwise equal to the latest stamp
     */
    inline bool is_latest_stamp(uint64_t j_stamp) const {
        return tail.load(std::memory_order_acquire) == j_stamp;
    }

    /**
     * @brief: wait until the last job has the one provided
     * or you're notified
     */
    inline void wait_for_job(uint64_t curr_stamp) const {
        do {
            if(tail.load(std::memory_order_acquire) != curr_stamp) break;
            tail.wait(curr_stamp, std::memory_order_acquire);
        } while(true);
    }

    /**
     * @brief: unblock all waiting thread
     */
    inline void notify_all() {
        tail.notify_all();
    }

    /**
     * @brief: aborts the current job and wakes up all threads
     */
    inline void abort_latest() {
        buffer[tail.load(std::memory_order_acquire) % size_].abort();
        tail.notify_all();
    }
};

} // namespace engine::job
