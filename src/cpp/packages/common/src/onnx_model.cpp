#include "onnx_model.h"

#include <iostream>
#include <numeric>

OnnxModel::OnnxModel(const char* model_path)
    : env_(ORT_LOGGING_LEVEL_ERROR, "onnx"),
      allocator_(Ort::AllocatorWithDefaultOptions()) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        Ort::SessionOptions session_options;
        session_options.SetInterOpNumThreads(1);
        session_options.SetIntraOpNumThreads(1);
        session_options.SetLogSeverityLevel(3);
        session_ =
            std::make_unique<Ort::Session>(env_, model_path, session_options);

        size_t num_inputs = session_->GetInputCount();
        size_t num_outputs = session_->GetOutputCount();
        is_classifier_ = (num_outputs >= 2);

        Ort::TypeInfo type_info = session_->GetInputTypeInfo(0);
        auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
        input_shape_ = tensor_info.GetShape();

        for (size_t i = 0; i < input_shape_.size(); ++i) {
            std::cout << input_shape_[i]
                      << (i < input_shape_.size() - 1 ? ", " : "");
        }

        input_size_ = 1;
        for (auto dim : input_shape_) {
            if (dim > 0) {
                input_size_ *= dim;
            }
        }

        if (input_shape_[0] == -1) {
            input_shape_[0] = 1;
        }

    } catch (const Ort::Exception& e) {
        std::cerr << "ONNX Runtime Error: " << e.what() << std::endl;
        throw;
    } catch (const std::exception& e) {
        std::cerr << "Standard Exception: " << e.what() << std::endl;
        throw;
    } catch (...) {
        std::cerr << "Unknown error occurred while loading the model"
                  << std::endl;
        throw;
    }
    std::cout << "Model initialization completed successfully" << std::endl;
}

float OnnxModel::predict(const std::vector<float>& input_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (input_data.size() != input_size_) {
        std::string error_msg =
            "Input data size does not match model input size. "
            "Expected: " +
            std::to_string(input_size_) +
            ", Got: " + std::to_string(input_data.size()) + ". Input shape: [";

        for (size_t i = 0; i < input_shape_.size(); ++i) {
            error_msg += std::to_string(input_shape_[i]);
            if (i < input_shape_.size() - 1) error_msg += ", ";
        }
        error_msg += "]";

        throw std::runtime_error(error_msg);
    }

    auto in_name_alloc = session_->GetInputNameAllocated(0, allocator_);
    std::string in_name_str(in_name_alloc.get());
    const std::vector<const char*> input_names = {in_name_str.c_str()};

    size_t output_idx = is_classifier_ ? 1 : 0;
    auto out_name_alloc =
        session_->GetOutputNameAllocated(output_idx, allocator_);
    std::string out_name_str(out_name_alloc.get());
    const std::vector<const char*> output_names = {out_name_str.c_str()};

    auto memory_info =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto input_tensor = Ort::Value::CreateTensor<float>(
        memory_info, const_cast<float*>(input_data.data()), input_data.size(),
        input_shape_.data(), input_shape_.size());

    auto output_tensors =
        session_->Run(Ort::RunOptions{nullptr}, input_names.data(),
                      &input_tensor, 1, output_names.data(), 1);

    float* output_data = output_tensors[0].GetTensorMutableData<float>();
    if (is_classifier_) {
        return output_data[1];  // P(class=1) from [P(class=0), P(class=1)]
    }
    return output_data[0];
}