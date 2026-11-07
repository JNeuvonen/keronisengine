#pragma once

struct SizingCoefficientData {
    const char* sizing_coefficient;
    double ml_prediction;
    int raised_exception;
    int has_valid_ml_prediction;
    const char* error_message;
};