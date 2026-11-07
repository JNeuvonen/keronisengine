#pragma once

#include <onnxruntime_cxx_api.h>

#include <array>
#include <mutex>
#include <vector>

class OnnxModel {
   public:
    OnnxModel(const char* model_path);

    float predict(const std::vector<float>& input_data);
    size_t get_input_size() const { return input_size_; }

    void cleanup() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_) {
            session_.reset();
        }
    }

   private:
    Ort::Env env_;
    std::unique_ptr<Ort::Session> session_;
    Ort::AllocatorWithDefaultOptions allocator_;
    mutable std::mutex mutex_;
    std::vector<int64_t> input_shape_;
    size_t input_size_;
    bool is_classifier_ = false;
};
