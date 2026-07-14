#pragma once
#include <atomic>
#include <cassert>
#include "../../macro_util.hpp"

/**
 * @brief: Tagged Reference Counter struct specification
 * 
 * this struct implements a tagged reference counter. The counter consists in 
 * 3 parts:
 * 1. reference counter (up to 2^32 -1 concurrent threads)
 * 2. version (monotonically increasing with 2^31 wraparound)
 * 3. tag (bit-flag)
 * 
 * The tag flags allow a thread to lock-out other threads which try to acquire a 
 * resource until explicitly cleared. The version is needed internally to mitigate
 * the ABA problem. The version can also be used to track generations of the same 
 * resource
 */
struct TaggedReferenceCount {
    private:
    using u64 = uint64_t;
    public:
    // Bit Layout: [ 1-bit TAG | 31-bit VERSION | 32-bit REFCOUNT ]
    CACHE_ALIGN std::atomic<u64> word{0ull};  
    CACHE_PAD(std::atomic<u64>)
    static inline thread_local bool is_active{false};

    static constexpr u64 REFCOUNT_MASK = 0x00000000FFFFFFFFull; // Lower 32 bits
    static constexpr u64 TAG_MASK      = 1ull << 63;            // Most Significant Bit (MSB)
    static constexpr u64 VERSION_MASK  = 0x7FFFFFFF00000000ull; // Middle 31 bits
    static constexpr unsigned int VERSION_SHIFT = 32;

    /**
     * @brief: returns the current version of the reference counter
     * @rerturns: u64 meant for `::has_version()` compare
     */
    u64 get_version() const noexcept {
        return (word.load(std::memory_order_acquire) & VERSION_MASK) >> VERSION_SHIFT;
    }

    /**
     * @brief: checks if the current version of the counter is the one provided
     * @param: version to check agains
     * @returns: true if the version of the reference counter matches the
     * one provided
     */
    bool has_version(u64 v) const noexcept {
        return get_version() == v;
    }

    /**
     * @brief: checks if no entity is holding the reference counter
     */
    bool zero() const noexcept {
        return (word.load(std::memory_order_acquire) & REFCOUNT_MASK) == 0;
    }
    
    /**
     * @brief: speculative increments the reference counter
     * @returns: true if the counter was acquired, false if the tag was setted
     * 
     * @warning: calling this method on a previously acquired reference counter
     * likely disrupt the internal counter. 
     */
    bool acquire() noexcept {
        const u64 old = word.fetch_add(1ull, std::memory_order_acq_rel);
        if ((old & TAG_MASK) != 0) { //check if the msb was setted
            //unset the reference counter
            (void) word.fetch_sub(1ull, std::memory_order_release);
            return false;
        }
        return true;
    }

    /**
     * @brief: get the word content for comparison
     */
    u64 data() const noexcept {
        return word.load(std::memory_order_acquire);
    }

    /**
     * @brief: release a previously incremented reference counter waking any
     * thread waiting on it if the counter reached 0
     */
    void release() noexcept {
        u64 old = word.fetch_sub(1ull, std::memory_order_release);
        //wakes up any thread which was waiting
        if (((old & REFCOUNT_MASK) == 1ull)) {
            word.notify_all();
        }
    }

    /**
     * @brief: explicitly set the tag, making all incoming acquire fail
     * until the version is explicitly advanced
     */
    void set_tag() noexcept {
        word.fetch_or(TAG_MASK, std::memory_order_acq_rel);
    }

    /**
     * @brief checks if the tag has been setted
     */
    bool get_tag() const noexcept {
        return (word.load(std::memory_order_acquire) & TAG_MASK) != 0ull;
    }

    
    /**
    * @brief: advances the version of a tagged reference counter after all threads
    * are out
    */
    void reset_advance_tagged() noexcept {
        u64 current = word.load(std::memory_order_acquire);
        assert((current & ~(VERSION_MASK)) == TAG_MASK && "Cannot advance version until all threads are out");
        //compute the new version
        u64 new_version_mask = (current + (1ull << 32)) & VERSION_MASK;
        word.store(new_version_mask,std::memory_order_release);
        word.notify_all();
    }

    /**
     * @brief: advances the version of a not (possibly untagged) reference counter
     * @returns: true if the reference was advanced to the next version, false otherwise
     */
    bool reset_advance_untagged() noexcept {
        u64 current = word.load(std::memory_order_acquire);
        if((current & REFCOUNT_MASK) != 0) return false;
        u64 new_version_mask = (current + (1ull << 32)) & VERSION_MASK;
        //if the CAS fails then the reference counter allowed one thread
        return word.compare_exchange_strong(current,new_version_mask,std::memory_order_acq_rel,std::memory_order_release);
        word.notify_all();
    }

    /**
     * @brief: blocks the caller until the reference counter hits 0 
     * or the version advances
     */
    void release_wait() noexcept {
        u64 current = word.fetch_sub(1,std::memory_order_acq_rel) - 1;
        if((current & REFCOUNT_MASK) == 0) return;
        //wait until the counter is 0 or the version changed
        do {
            word.wait(current,std::memory_order_acquire);
            //woken up spuriously or notified
            u64 new_ref = word.load(std::memory_order_acquire);
            if((new_ref & REFCOUNT_MASK) == 0 || (current & VERSION_MASK) != (new_ref & VERSION_MASK))
                return;
        } while(true);
    }
};