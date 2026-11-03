#pragma once
#include <cmath>
#include <stdexcept>
#include <string>
namespace automation_check {
inline void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
inline void near(double value, double expected, const char *message) {
  require(std::abs(value - expected) < 0.00001, message);
}
} // namespace automation_check
