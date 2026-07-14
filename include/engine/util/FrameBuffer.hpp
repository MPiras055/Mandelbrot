#pragma once 
#include <cstdlib>
#include <raylib.h>
#include <cassert>
#include <utility> // For std::swap

namespace engine::util {
    
struct FrameBuffer {
    private:
    size_t capacity_; 
    size_t size_;
    Color *data_;

    public:

    struct View {
        const Color* const pixels;
        const unsigned int height;
        const unsigned int width;
        bool uptodate;

        private:
        friend FrameBuffer;
        View(Color* const pixels, unsigned int height,unsigned int width, bool uptodate = false):
            pixels{pixels},width{width},height{height},uptodate{uptodate} {};
    };

        
    FrameBuffer(size_t capacity, unsigned int height, unsigned int width):
        capacity_{capacity}, size_{height * width},
        data_{new Color[capacity_]} {
            assert(capacity_ != 0 && "FrameBuffer: capacity must be non null");
            assert(size_ != 0 && "FrameBuffer: size must be non null");
    }

    ~FrameBuffer() {
        delete[] data_;
    }
    
    void resize(unsigned int new_height, unsigned int new_width) {
        assert(new_height * new_width != 0 && "FrameBuffer::resize: new size must be non-null");
        assert(new_height * new_width <= capacity_ && "FrameBuffer::resize: new size exceeds max capacity");
        size_ = new_height * new_width;
    }

    /**
     * @brief const access operator
     */
    Color operator[](size_t index) const {
        assert(index < capacity_ && "FrameBuffer: out of bounds access");
        return data_[index];
    }

    /**
     * @brief index set operator
     */
    Color& operator[](size_t index) {
        assert(index < capacity_ && "FrameBuffer: out of bounds set");
        return data_[index];
    }

    /**
     * @brief: get the underlying memory
     */
    Color* data() {
        return data_;
    }

    /**
     * @brief: get the active size
     */
    size_t size() {
        return size_;
    }


    /**
     * @brief: get a constant view of the frame, tagged with height
     * and width ratio and a boolean flag
     */
    View getView(unsigned int height, unsigned int width, bool is_up_to_date = false) {
        assert(height * width != 0 && "FrameBuffer::getView: height * width must be non-null");
        return View(data(),height,width,is_up_to_date);
    }

    /**
     * @brief: static swap function, which swaps 2 framebuffers
     */
    static void swap(FrameBuffer& a, FrameBuffer& b) noexcept {
        std::swap(a.capacity_, b.capacity_);
        std::swap(a.size_, b.size_);
        std::swap(a.data_, b.data_);
    }
};

    
}//mandelbrot engine