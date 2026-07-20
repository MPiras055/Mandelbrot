#pragma once
#include <cstdlib>
#include <limits>
#include <atomic>
#include <cassert>
#include "Counters.hpp"
#include "../../macro_util.hpp"

namespace engine::job {

//forward declaration for friend specifier
struct RenderJob;

struct EscapeTimeJob {
    EscapeTimeJob()     noexcept = default;
    ~EscapeTimeJob()    noexcept = default;

    /**
     * @brief: checks if the current job was completed or aborted
     * 
     * @note: the done check is performed by checking that the done
     * chunks match the total number of chunks. 
     */
    bool done(size_t total_chunks) const noexcept { 
        return (done_chunk.load(std::memory_order_acquire) & (~MSB_MASK)) == total_chunks; 
    }

    /**
     * @brief: checks if the current job was completed successfuly
     * 
     * @note: jobs that are successfuly aborted set a special flag in
     * the doneChunks, so we check if that flag is present, if not
     * we check that the doneChunks match the total chunks provided
     */
    bool completed(size_t total_chunks) const noexcept { 
        return done_chunk.load(std::memory_order_acquire) == total_chunks; 
    }

    /**
     * @brief: get the completion percentage of the current job
     * 
     * @returns: a number from 0 to 100 of the chunks processed in respect of the 
     * total chunks of the job
     * @note: doesn't check if the job has been aborted
     */
    unsigned int percentageStatus(size_t total_chunks) const noexcept {
        return counters::percentOf(done_chunk.load(std::memory_order_relaxed) & ~MSB_MASK,
                                   total_chunks);
    }

    /**
     * @brief: checks if the current job was registered as aborted
     * 
     * @note: a job is registered as aborted by an abort call on an uncompleted job
     * @note: a successfuly aborted job sets a flag on the doneChunk counter, we check
     * for the flag
     */
    bool aborted() const noexcept { 
        return (done_chunk.load(std::memory_order_acquire) & MSB_MASK) != size_t{0}; 
    }

    /**
     * @brief: try to acquire a chunk of the current job
     * @note: the method returns false if the job was aborted or completed
     * @note: the method sets the chunk_i reference only if a chunk was successfuly
     * got
     */
    [[nodiscard]] bool get_chunk(size_t total_chunks, size_t& chunk_i) noexcept {
        const size_t i = next_chunk.fetch_add(1, std::memory_order_acq_rel);
        if (i >= total_chunks) return false;
        chunk_i = i;
        return true;
    }

    /**
     * @brief: mark a previously acquired chunk as completed
     */
    void mark_completed_chunk([[maybe_unused]] size_t chunk_i) noexcept {
        (void) done_chunk.fetch_add(1, std::memory_order_release);
    }
    
    private:
    //MSB MASK for abort
    static constexpr size_t MSB_MASK = size_t(1) << (std::numeric_limits<size_t>::digits - 1);
    CACHE_ALIGN std::atomic_size_t next_chunk{0};
    CACHE_ALIGN std::atomic_size_t done_chunk{0};
    CACHE_PAD(std::atomic_size_t)

    protected:
    friend RenderJob;

    /**
     * @brief: reset the job to its default clear state
     */
    void reset(size_t total_chunks) noexcept {
        assert((total_chunks & MSB_MASK) == 0 && "total_chunks too high");
        next_chunk.store(0, std::memory_order_relaxed);
        done_chunk.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief: attempts to register the job as aborted
     * 
     * @note: this method fails if the job was previously completed
     */
    bool abort(size_t total_chunks) noexcept {
        //block new threads to acquire new chunks
        const size_t last = next_chunk.fetch_add(MSB_MASK, std::memory_order_acq_rel);
        if (last < total_chunks) {
            //commit the chunks stolen setting the MSB
            const size_t flag = (total_chunks - last) | MSB_MASK;
            done_chunk.fetch_add(flag, std::memory_order_release);
            return true;
        }
        return false;
    }
}; 

}