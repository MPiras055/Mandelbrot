#pragma once
#include <atomic>
#include <cassert>
#include <complex>
#include <limits>
#include <boost/multiprecision/cpp_bin_float.hpp>
#include "core/Numeric.hpp"
#include "../../macro_util.hpp"

namespace engine::job {

//forward declaration for friend specifier
struct RenderJob;

//forward declaration for pointer specification
struct InitialOrbit;

//forward declaration for pointer specification
struct OptimalOrbitCache;

struct PerturbationJob {
    using ComplexDouble = core::ComplexDouble;

    PerturbationJob()  noexcept = default;
    ~PerturbationJob() noexcept {}

    CACHE_ALIGN std::atomic<void*> initialOrbitData{nullptr};
    std::atomic<void*> finalOrbitData{nullptr};
    CACHE_PAD(std::atomic<void*>)
    CACHE_ALIGN std::atomic_size_t nextProbe{0};    //index for the next probe for deltaProbing
    CACHE_ALIGN std::atomic_size_t doneProbe{0};    //index for the done probe for deltaProbing
    CACHE_ALIGN std::atomic_size_t nextChunk{0};    //index for the next chunk (general)
    CACHE_ALIGN std::atomic_size_t doneChunk{0};    //index for the done chunk (general)
    CACHE_PAD(std::atomic<size_t>)

    /**
     * @brief: checks if the current job was completed or aborted
     * 
     * @note: the done check is performed by checking that the done
     * chunks match the total number of chunks. 
     */
    bool done(size_t total_chunks) const noexcept { 
        return (doneChunk.load(std::memory_order_acquire) & (~MSB_MASK)) == total_chunks; 
    }


    /**
     * @brief: checks if the current job was completed successfuly
     * 
     * @note: jobs that are successfuly aborted set a special flag in
     * the doneChunks, so we check if that flag is present, if not
     * we check that the doneChunks match the total chunks provided
     */
    bool completed(size_t total_chunks) const noexcept { 
        return doneChunk.load(std::memory_order_acquire) == total_chunks; 
    } 

    /**
     * @brief: checks if the current job was registered as aborted
     * 
     * @note: a job is registered as aborted by an abort call on an uncompleted job
     * @note: a successfuly aborted job sets a flag on the doneChunk counter, we check
     * for the flag
     */
    bool aborted() const noexcept { 
        return (doneChunk.load(std::memory_order_acquire) & MSB_MASK) != 0ul; 
    }
    
    /**
     * @brief: try to acquire a chunk for the delta probing
     * @note: the method returns false if the delta probing was completed
     * or the job aborted
     * @note: the metoh sets the chunk_i reference only if a chunk was successfuly
     * got
     */
    [[nodiscard]] bool getProbeChunk(size_t total_probes, size_t& chunk_i) noexcept {
        const size_t i = nextProbe.fetch_add(1, std::memory_order_acq_rel);
        if (i >= total_probes) return false;
        chunk_i = i;
        return true;
    }

    /**
     * @brief: mark a previously acquired probeChunk as completed
     */
    void markCompletedProbe() noexcept {
        (void) doneProbe.fetch_add(1, std::memory_order_release);
    }

    /**
     * @brief: checks if the probing is done
     * 
     * @note: this method also returns true if the job was aborted
     */
    bool probeCompleted(size_t total_probes) const noexcept {
        return doneProbe.load(std::memory_order_acquire) >= total_probes;
    }

    /**
     * @brief: try to acquire a chunk of the current job
     * @note: the method returns false if the job was aborted or completed
     * @note: the method sets the chunk_i reference only if a chunk was successfuly
     * got
     */
    [[nodiscard]] bool getChunk(size_t total_chunks, size_t& chunk_i) noexcept {
        const size_t i = nextChunk.fetch_add(1, std::memory_order_acq_rel);
        if (i >= total_chunks) return false;
        chunk_i = i;
        return true;
    }

    /**
     * @brief: mark a previously acquired chunk as completed
     */
    void markCompletedChunk([[maybe_unused]] size_t chunk_i) noexcept {
        (void) doneChunk.fetch_add(1, std::memory_order_release);
    }
    
    private:
    static constexpr size_t MSB_MASK = size_t(1) << (std::numeric_limits<size_t>::digits - 1);

    protected:
    friend RenderJob;
    void reset(size_t total_chunks) noexcept {
        assert((total_chunks & MSB_MASK) == 0 && "total_chunks too high");
        initialOrbitData.store(nullptr,std::memory_order_relaxed);
        finalOrbitData.store(nullptr,std::memory_order_relaxed);
        nextProbe.store(0,std::memory_order_relaxed);
        doneProbe.store(0, std::memory_order_relaxed);
        nextChunk.store(0, std::memory_order_relaxed);
        doneChunk.store(0, std::memory_order_relaxed);
    }

    
    /**
     * @brief: attempts to register the job as aborte
     * 
     * @note this method fails if the job was previously completed
     */
    bool abort(size_t total_chunks) noexcept {
        const size_t last = nextChunk.fetch_add(MSB_MASK,std::memory_order_acq_rel);
        if(last < total_chunks) {
            //also invalidate the nextProbe to force threads to abort probing
            // if enabled
            nextProbe.fetch_or(MSB_MASK,std::memory_order_release);
            const size_t flag = (total_chunks - last) | MSB_MASK;
            doneChunk.fetch_add(flag,std::memory_order_release);
            return true;
        }
        return false;
    }
};

} //namespace engine::job