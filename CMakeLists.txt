cmake_minimum_required(VERSION 3.22)
project(sq8l VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# True for cl.exe and for clang in MSVC-ABI mode (clang-cl, or clang --target=*-windows-msvc).
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
  set(SQ8L_MSVC_ABI TRUE)
endif()

# Bit-faithful floating point: no FMA contraction, honor the dynamic rounding
# mode (the engine switches to round-toward-zero like the original x87 code).
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(SQ8L_FP_FLAGS -ffp-model=strict -ffp-contract=off)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(SQ8L_FP_FLAGS -frounding-math -fsignaling-nans -ffp-contract=off)
elseif(MSVC)
  set(SQ8L_FP_FLAGS /fp:strict)
endif()

# Keyed on the command-line flavour, not the compiler: clang-cl parses -Wall as MSVC's
# /Wall, which is -Weverything (thousands of warnings), and rejects -Wextra outright.
# /W4 is what it maps to -Wall -Wextra.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  set(SQ8L_WARN_FLAGS /W4)
else()
  set(SQ8L_WARN_FLAGS -Wall -Wextra)
endif()

if(SQ8L_MSVC_ABI)
  # windows.h min/max macros break std::min/max; the CRT deprecates fopen and friends.
  add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # Sources are UTF-8; the engine data tables exceed the default section limit.
    add_compile_options(/utf-8 /bigobj)
  endif()
endif()

file(GLOB SQ8L_ENGINE_SOURCES CONFIGURE_DEPENDS src/engine/*.cpp src/engine/data/*.cpp)
# Correctly rounded exp/log/sin/cos/tan (identical results on every platform).
set(SQ8L_CRMATH_SOURCES third_party/core-math/exp.c third_party/core-math/log.c third_party/core-math/sin.c
    third_party/core-math/cos.c third_party/core-math/tan.c)
if(WIN32)
  list(APPEND SQ8L_CRMATH_SOURCES third_party/core-math/roundeven_shim.c)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
  # cl.exe has no 128-bit integer type, which core-math needs for its 64x64 -> 128
  # products, and none of the GCC builtins it calls. src/compat/core_math_msvc.hpp
  # supplies both; it needs operator overloading, so these C sources are compiled as
  # C++ (C++20 for the designated initialisers in their tables).
  set_source_files_properties(${SQ8L_CRMATH_SOURCES} PROPERTIES LANGUAGE CXX
      COMPILE_OPTIONS "/std:c++20;/FI${CMAKE_SOURCE_DIR}/src/compat/core_math_msvc.hpp")
endif()
add_library(sq8l_engine STATIC ${SQ8L_ENGINE_SOURCES} ${SQ8L_CRMATH_SOURCES})
target_include_directories(sq8l_engine PUBLIC src/engine)
target_compile_options(sq8l_engine PRIVATE ${SQ8L_FP_FLAGS} ${SQ8L_WARN_FLAGS})
add_subdirectory(src/gui)
add_subdirectory(src/gui/logic)
add_subdirectory(src/gui/drawn)

# C API used by the Python differential tests against the emulated original.
file(GLOB SQ8L_TESTAPI_SOURCES CONFIGURE_DEPENDS tests/capi*.cpp)
add_library(sq8l_testapi SHARED ${SQ8L_TESTAPI_SOURCES})
target_link_libraries(sq8l_testapi PRIVATE sq8l_engine sq8l_gui sq8l_gui_logic)
target_compile_options(sq8l_testapi PRIVATE ${SQ8L_FP_FLAGS})

# ---------------------------------------------------------------- plugin (DPF)
option(SQ8L_BUILD_PLUGIN "Build the SQ8L plugin (VST2/VST3/AU/CLAP)" ON)
if(SQ8L_BUILD_PLUGIN)
  # Local DPF / pugl patches (kept out of the submodules): apply once if not yet applied.
  foreach(entry "dpf-vst2-compat.patch=third_party/DPF" "dpf-msvc-enum.patch=third_party/DPF"
                "pugl-x11-nested-update.patch=third_party/DPF/dgl/src/pugl-upstream")
    string(REPLACE "=" ";" entry "${entry}")
    list(GET entry 0 patch)
    list(GET entry 1 dir)
    execute_process(COMMAND git apply --reverse --check ${CMAKE_SOURCE_DIR}/patches/${patch}
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/${dir}
                    RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
    if(NOT already EQUAL 0)
      execute_process(COMMAND git apply ${CMAKE_SOURCE_DIR}/patches/${patch}
                      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/${dir}
                      RESULT_VARIABLE failed)
      if(failed)
        message(FATAL_ERROR "could not apply patches/${patch} to ${dir}")
      endif()
    endif()
  endforeach()

  # The Windows SDK ships GL/gl.h but not GL/glext.h, which DGL includes. DPF downloads
  # the Khronos headers itself but ignores download failures (and the khronos.org ->
  # registry.khronos.org redirect loses khrplatform.h), so fetch them here and check.
  # DPF gitignores third_party/DPF/khronos and puts it on the include path for MSVC.
  if(SQ8L_MSVC_ABI)
    set(SQ8L_KHRONOS_DIR ${CMAKE_SOURCE_DIR}/third_party/DPF/khronos)
    foreach(entry "GL/glext.h=OpenGL" "KHR/khrplatform.h=EGL")
      string(REPLACE "=" ";" entry "${entry}")
      list(GET entry 0 header)
      list(GET entry 1 registry)
      if(NOT EXISTS ${SQ8L_KHRONOS_DIR}/${header})
        message(STATUS "Downloading ${header} (not in the Windows SDK)")
        file(DOWNLOAD https://registry.khronos.org/${registry}/api/${header}
             ${SQ8L_KHRONOS_DIR}/${header} STATUS status TLS_VERIFY ON)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
          file(REMOVE ${SQ8L_KHRONOS_DIR}/${header})
          list(GET status 1 reason)
          message(FATAL_ERROR "could not download ${header}: ${reason}")
        endif()
      endif()
    endforeach()
  endif()

  add_subdirectory(third_party/DPF)
  if(SQ8L_MSVC_ABI AND TARGET dgl-system-libs-definitions)
    # Idempotent: DPF adds this itself when CMake's MSVC is set, but not for the
    # GNU-driver clang that merely targets the MSVC ABI.
    target_include_directories(dgl-system-libs-definitions INTERFACE ${SQ8L_KHRONOS_DIR})
  endif()
  # Formats per system (Audio Unit on macOS, LV2 on Linux); the Linux editor uses DPF's
  # file browser (its menus and dialogs are drawn in the window, src/gui/drawn).
  if(APPLE)
    set(SQ8L_FORMATS vst2 vst3 au clap)
    set(SQ8L_DPF_OPTIONS)
  elseif(WIN32)
    set(SQ8L_FORMATS vst2 vst3 clap)
    set(SQ8L_DPF_OPTIONS)
  else()
    set(SQ8L_FORMATS vst2 vst3 clap lv2)
    set(SQ8L_DPF_OPTIONS USE_FILE_BROWSER)
  endif()
  # The target is named like the original's file (SQ8L.dll), so the plug-ins come out as
  # SQ8L.vst3, SQ8L.clap, SQ8L.component, SQ8L.lv2...
  dpf_add_plugin(SQ8L ${SQ8L_DPF_OPTIONS}
    TARGETS ${SQ8L_FORMATS}
    UI_TYPE opengl
    FILES_DSP src/plugin/SQ8LPlugin.cpp
              src/plugin/MtsEspTuning.cpp
              ${CMAKE_SOURCE_DIR}/third_party/MTS-ESP/Client/libMTSClient.cpp
    FILES_UI src/plugin/SQ8LUI.cpp
             $<$<PLATFORM_ID:Darwin>:${CMAKE_SOURCE_DIR}/src/plugin/mac/PlatformUiMac.mm>
             $<$<PLATFORM_ID:Windows>:${CMAKE_SOURCE_DIR}/src/plugin/win/PlatformUiWin.cpp>
             $<$<PLATFORM_ID:Windows>:${CMAKE_SOURCE_DIR}/src/plugin/win/DarkMenu.cpp>
             $<$<PLATFORM_ID:Linux>:${CMAKE_SOURCE_DIR}/src/plugin/linux/X11Pointer.cpp>)
  target_include_directories(SQ8L PUBLIC src/plugin)
  target_link_libraries(SQ8L PUBLIC sq8l_engine)
  target_link_libraries(SQ8L-dsp PUBLIC sq8l_engine)
  # MTS-ESP client (third_party/MTS-ESP, 0BSD): dlopens the master's library at run time.
  target_include_directories(SQ8L-dsp PUBLIC ${CMAKE_SOURCE_DIR}/third_party/MTS-ESP/Client)
  if(NOT WIN32)
    target_link_libraries(SQ8L-dsp PUBLIC ${CMAKE_DL_LIBS})
  endif()
  target_link_libraries(SQ8L-ui PUBLIC sq8l_gui sq8l_gui_logic sq8l_gui_drawn sq8l_engine)
  if(WIN32)
    target_link_libraries(SQ8L-ui PUBLIC comdlg32 gdi32 user32 uxtheme)
  endif()
  if(APPLE)
    set_source_files_properties(src/plugin/mac/PlatformUiMac.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
    target_link_libraries(SQ8L-ui PUBLIC "-framework Cocoa" "-framework UniformTypeIdentifiers")
  endif()
  target_include_directories(SQ8L-ui PUBLIC src/plugin src/gui)
  target_compile_options(SQ8L-dsp PRIVATE ${SQ8L_FP_FLAGS})
  # VST2 on Windows/Linux: SQ8L.dll / SQ8L.so like the original, not DPF's SQ8L-vst2.
  if(TARGET SQ8L-vst2 AND NOT APPLE)
    set_target_properties(SQ8L-vst2 PROPERTIES OUTPUT_NAME SQ8L)
  endif()
endif()

# The drawn menus and dialogs of the Linux editor, without a window (tests/drawn_check.cpp).
add_executable(sq8l_drawn_check tests/drawn_check.cpp)
target_link_libraries(sq8l_drawn_check PRIVATE sq8l_gui_drawn)

# HD graphics without a window (tests/hd_check.cpp).
add_executable(sq8l_hd_check tests/hd_check.cpp)
target_link_libraries(sq8l_hd_check PRIVATE sq8l_gui)

# Native full-render checker (tests/render_check.cpp), see tests/export_golden_raw.py.
add_executable(sq8l_render_check tests/render_check.cpp)
target_link_libraries(sq8l_render_check PRIVATE sq8l_engine)
target_compile_options(sq8l_render_check PRIVATE ${SQ8L_FP_FLAGS})
