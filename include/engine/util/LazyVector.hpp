#pragma once
#include <cstdlib>
#include <utility>
#include <cassert>


namespace engine::util {

/**
 * @class LazyVector
 * @brief A high-performance, raw-pointer container that explicitly manages capacity.
 * @details Never deallocates or shrinks and only reallocates up if requested capacity 
 * exceeds current capacity, guaranteeing minimal allocations during continuous rendering 
 * loops.
 */
template<typename T>
class LazyVector {
private:
    T* data_ = nullptr;
    size_t capacity_ = 0;
    size_t size_ = 0;

public:
    LazyVector() = default;
    ~LazyVector() { delete[] data_; }

    // Delete copy semantics to prevent accidental deep copies
    LazyVector(const LazyVector&) = delete;
    LazyVector& operator=(const LazyVector&) = delete;

    /**
     * @brief: resets the size to 0 and reallocates if necessary
     * @note: this method can be called multiple times (it's strongly suggested)
     */
    void init(size_t req_capacity) {
        size_ = 0;
        if (req_capacity > capacity_) {
            delete[] data_;
            data_ = new T[req_capacity];
            capacity_ = req_capacity;
        }
    }

    /**
     * @brief Unchecked addition to the vector for maximum loop speed.
     */
    inline void push_back(const T& value) {
        data_[size_++] = value;
    }

    /**
        * @brief Unchecked in-place construction for maximum performance.
        * @details Constructs the element directly in the allocated memory, 
        * avoiding copy/move overhead entirely.
        */
    template<typename... Args>
    inline T& emplace_back(Args&&... args) {
        // Construct the object directly in-place at the current size_ index
        ::new (static_cast<void*>(data_ + size_)) T(std::forward<Args>(args)...);
        return data_[size_++];
    }

    inline size_t size() const { return size_; }
    inline const T* data() const { return data_; }
    
    inline const T& operator[](size_t index) const { return data_[index]; }
    inline T& operator[](size_t index) { return data_[index]; }
};

}
