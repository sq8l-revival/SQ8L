// Export macro for the C API the Python differential tests load (see capi*.cpp).
#pragma once

#if defined(_MSC_VER)
#define SQ8L_API extern "C" __declspec(dllexport)
#else
#define SQ8L_API extern "C" __attribute__((visibility("default")))
#endif
