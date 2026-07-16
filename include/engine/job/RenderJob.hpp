#pragma once
#include <cstddef>
#include <variant>
#include <boost/multiprecision/cpp_bin_float.hpp>
#include <cassert>
#include "core/Numeric.hpp"
#include "core/Pixel.hpp"
#include "../util/TaggedReferenceCounter.hpp"
#include "../job/EscapeTimeJob.hpp"
#include "../job/PerturbationJob.hpp"

namespace engine::job {

    class EscapeTimeEngine;
    class PerturbationEngine;
    class RenderJobStack;

    struct RenderJob {
        
        using BigFloat = core::BigFloat;
        /**
         * @brief: specifics for a RenderJob
         * 
         * @note: for ETA x,y it's the top left point while for PTB it's the 
         * center point
         * @note: why BigFloat for x,y: because it has to be compatible as double
         * for ETA and as BigFloat as PTB (it's an implicit union because it would
         * have been ugly to actually put one)
         */
        struct JobSpecs {
            BigFloat x{0.0};
            BigFloat y{0.0};
            unsigned int width{0};
            unsigned int height{0};
            unsigned int iterations{0};
            double pixelStepX{0.0};
            double pixelStepY{0.0};
            size_t chunks{0};
            bool enableDeltaProbing{true}; // NEW: Toggle flag for Delta Probing
            // ETA float-vs-double lane width, decided at dispatch (from zoom) so
            // the worker routine no longer needs to read the camera.
            bool useFloat{true};
        };

        using ETAJob = EscapeTimeJob;
        using PTBJob = PerturbationJob;

        /**
         * @brief: tries to acquire a job
         * @param: uint64_t ref stamp_ref: reference to the current stamp job
         * @returns: true if the job could have been acquired, false if the 
         * job wasn't acquired and is completed or done
         * 
         * Acquires the current job preventing the RenderJobStack to recycle it. Acquire
         * works as an atomic reference count increment, which prevents the RenderJobStack 
         * to recycle it (modify the job data) util all threads stop observing the job
         */
        [[nodiscard]] inline bool acquire(uint64_t& stamp_ref) noexcept {
            if(refCount.acquire()) {
                stamp_ref = stamp;
                return true;
            }
            return false;
        }

        /**
         * @brief: release a job previously acquired
         * 
         * @warning: this method should be called only on code execution paths where
         * a successful ::acquire() was called
         */
        inline void release() noexcept { 
            refCount.release();
        }

        /**
         * @brief: releases a job previously acquired and blocks the
         * caller until all the other threads have released the job
         */
        inline void releaseWait() noexcept {
            refCount.release_wait();
        }

        /**
         * @brief: check if a job is registered as completed or aborted
         * 
         * @note: in this specific implementation we're only considering 
         * PerturbationJob or EscapeTimeJob concrete implementations
         * @note: this method should be called only if a previous ::acquire()
         * was successful (or unless we're sure that no thread can modify the object)
         */
        inline bool done() const noexcept {
            return std::visit([this](const auto& jobState) noexcept -> bool {
                return jobState.done(specs.chunks);
            },jobState);
        }

        /**
         * @brief: check if a job was successfuly completed
         * 
         * @note: in this specific implementation we're only considering 
         * PerturbationJob or EscapeTimeJob concrete implementations
         * @note: this method should be called only if a previous ::acquire()
         * was successful (or unless we're sure that no thread can modify the object)
         */
        inline bool completed() const noexcept {
            return std::visit([this](const auto& jobState) noexcept -> bool {
                return jobState.completed(specs.chunks);
            },jobState);
        }

        /**
         * @brief: check if a job was registered as aborted
         * 
         * @note: an aborted job necessarily sets the SEAL_BIT on the 
         * reference counter, so we first check that. If the bit hasn't 
         * been set then we return false. It's possible that hte SEAL_BIT
         * was setted but the job hasn't been aborted (it's completed) so 
         * we check that too
         * 
         * @note: abort can be issued on a job, but if a job was previously
         * completed, this method will return false (see done())
         * @note: this method should be called only if a previous ::acquire()
         * was successful (or unless we're sure that no thread can modify the object)
         */
        inline bool aborted() const noexcept {
            if(!refCount.get_tag()) return false;
            return std::visit([this](const auto& jobState) noexcept -> bool {
                return jobState.aborted();
            },jobState);
        }

        /**
         * @brief: checks if a job was sealed (meaning it was set as aborted or completed)
         * @note: this check is made to be a lightweight for workers, we don't actually
         * check if the job was aborted or completed, we just check if the seal flag was 
         * setted (this usually mean that the job is old and should be throwed)
         */
        inline bool sealed() const noexcept {
            return refCount.get_tag();
        }

        /**
         * @brief: templated get method to get the variant job state
         * @note: this method should be called only if a previous ::acquire()
         * was successful (or unless we're sure that no thread can modify the object)
         */
        template<typename T> [[nodiscard]] inline T& getState() noexcept { 
            return std::get<T>(jobState); 
        }

        /**
         * @brief: templated get method to get the variant job state in an
         * immutable manner
         * @note: this method should be called only if a previous ::acquire()
         * was successful (or unless we're sure that no thread can modify the object)
         */
        template<typename T> [[nodiscard]] inline const T& getState_const() const noexcept { 
            return std::get<T>(jobState); 
        }

        /**
         * @brief: getter for the job stamp
         * @note: this method should be called only if a previous ::acquire()
         * was successful (or unless we're sure that no thread can modify the object)
         */
        uint64_t getStamp() const noexcept { return stamp; }

        /**
         * @brief: getter for the job specs
         * @note: this method should be called only if a previous ::acquire()
         * was successful (or unless we're sure that no thread can modify the object)
         */
        JobSpecs getSpecs() const noexcept { return specs; } 

        /**
         * @brief: getter for the underlying type of the variant jobState
         * 
         * @note: this method should be called only if a previous ::acquire()
         * was successful (or unless we're sure that no thread can modify the object)
         */
        template<typename T> inline constexpr bool holds() const noexcept { return std::holds_alternative<T>(jobState); }

        private:
        static constexpr uint64_t SEAL_BIT = 1ull << 63;
        friend RenderJobStack;
        uint64_t stamp{};
        JobSpecs specs{};
        TaggedReferenceCount refCount;
        std::variant<ETAJob, PTBJob> jobState;

        /**
         * @brief: emplace a new render job on the current object
         * 
         * 
         * @asserts: that the job is safe to reclaim, checking that
         * the SEAL bit was set and that no threads are done observing the
         * job. 
         * @note: sets the specs, the stamp and the jobState and at the end
         * releases the SEAL flag
         * @note: calls reset on EscapeTimeJob and PerturbationJob 
         * implementations
         */
        template<typename T, typename... Args>
        void emplace(JobSpecs s, uint64_t tag, Args&&... args) {
            assert(safeToRecycle() && "emplacing job not safe");
            static_assert(std::is_same_v<T, ETAJob> || std::is_same_v<T, PTBJob>, "Invalid type");
            specs = s; 
            if constexpr (std::is_same_v<T, ETAJob>) {
                jobState.emplace<ETAJob>().reset(specs.chunks);
            } else if constexpr (std::is_same_v<T, PTBJob>) {
                jobState.emplace<PTBJob>().reset(specs.chunks, std::forward<Args>(args)...);
            }
            stamp = tag;
            refCount.reset_advance_tagged();
        }

        /**
         * @brief: seals the job making all incoming acquire fail
         */
        inline void seal() noexcept { 
            refCount.set_tag(); 
        }

        /**
         * @brief: registers the job as aborted if it wasn't completed
         * @note: seals the job and calls abort on the variant job implementation
         */
        inline bool abort() noexcept {
            seal(); //seal the job from incoming threads
            return std::visit([this](auto& job) noexcept -> bool { 
                return job.abort(specs.chunks); //return the abort flag from the underlying implementation
            },jobState); 
        }

        /**
         * @brief: checks if a job is safe to recycle
         * 
         * @note: a job is safe to recycle when it was previously sealed
         * (blocks all future threads to acquire the job) and when all threads
         * that acquired the job left it
         */
        inline bool safeToRecycle() const noexcept { 
            return refCount.zero(); 
        }
    };

} // namespace engine::job