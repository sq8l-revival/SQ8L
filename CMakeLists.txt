cmake_minimum_required(VERSION 3.22)
project(sq8l VERSION 1.0.0 LANGUAGES C CXX)
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

# ---------------------------------------------------------------- version of this build
# SQ8LVersion.h (cmake/Version.cmake), derived every build so the commit is the one being built.
# The script rewrites the file only when it changes, so dependents do not rebuild.
set(SQ8L_VERSION_HEADER ${CMAKE_BINARY_DIR}/generated/SQ8LVersion.h)
add_custom_target(sq8l_version_header
  BYPRODUCTS ${SQ8L_VERSION_HEADER}
  COMMAND ${CMAKE_COMMAND} -DSQ8L_NUMBER=${PROJECT_VERSION} -DSQ8L_SRC=${CMAKE_SOURCE_DIR}
          -DSQ8L_IN=${CMAKE_SOURCE_DIR}/src/version/SQ8LVersion.h.in
          -DSQ8L_OUT=${SQ8L_VERSION_HEADER}
          -P ${CMAKE_SOURCE_DIR}/cmake/Version.cmake
  COMMENT "Deriving the version of this build")
add_library(sq8l_version INTERFACE)
target_include_directories(sq8l_version INTERFACE ${CMAKE_BINARY_DIR}/generated)

add_subdirectory(src/gui)
add_subdirectory(src/gui/logic)
add_subdirectory(src/gui/drawn)

# The About dialog and the boot message name the build. PUBLIC: everything linking the editor
# logic gets the header too.
target_link_libraries(sq8l_gui_logic PUBLIC sq8l_version)
add_dependencies(sq8l_gui_logic sq8l_version_header)

# C API used by the Python differential tests against the emulated original.
file(GLOB SQ8L_TESTAPI_SOURCES CONFIGURE_DEPENDS tests/capi*.cpp)
add_library(sq8l_testapi SHARED ${SQ8L_TESTAPI_SOURCES})
target_link_libraries(sq8l_testapi PRIVATE sq8l_engine sq8l_gui sq8l_gui_logic sq8l_sound_parameter_service)
add_dependencies(sq8l_testapi sq8l_version_header)
target_compile_options(sq8l_testapi PRIVATE ${SQ8L_FP_FLAGS})

# ---------------------------------------------------------------- plugin (DPF)
option(SQ8L_BUILD_PLUGIN "Build the SQ8L plugin (VST2/VST3/AU/CLAP)" ON)
if(SQ8L_BUILD_PLUGIN)
  # Adapter extensions overlap earlier compatibility hunks. Peel recognized
  # extensions off in reverse application order before checking the base stack;
  # otherwise git cannot recognize the already-applied compatibility patch.
  foreach(patch dpf-sq8l-parameters-au.patch dpf-sq8l-parameters-clap.patch dpf-sq8l-parameters-vst3.patch dpf-sq8l-parameters-vst2.patch)
    execute_process(COMMAND git apply --reverse --check ${CMAKE_SOURCE_DIR}/patches/${patch}
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/DPF
                    RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
    if(applied EQUAL 0)
      execute_process(COMMAND git apply --reverse ${CMAKE_SOURCE_DIR}/patches/${patch}
                      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/DPF
                      RESULT_VARIABLE failed)
      if(failed)
        message(FATAL_ERROR "could not remove adapter extension patches/${patch}")
      endif()
    endif()
  endforeach()
  # Local DPF / pugl patches (kept out of the submodules): apply once if not yet applied.
  foreach(entry "dpf-vst2-compat.patch=third_party/DPF" "dpf-msvc-enum.patch=third_party/DPF"
                "dpf-swap-interval.patch=third_party/DPF" "dpf-au-preset-name.patch=third_party/DPF"
                "dpf-vst3-program-change.patch=third_party/DPF"
                "dpf-sq8l-parameters-vst2.patch=third_party/DPF"
                "dpf-sq8l-parameters-vst3.patch=third_party/DPF"
                "dpf-sq8l-parameters-clap.patch=third_party/DPF"
                "dpf-sq8l-parameters-au.patch=third_party/DPF"
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
  # MONOLITHIC: the LV2 DSP and UI in one binary. The UI reaches the plugin instance directly
  # (DISTRHO_PLUGIN_WANT_DIRECT_ACCESS), so DPF's manifest names a single binary for both;
  # built apart, hosts looked for the UI in the DSP binary and showed none.
  dpf_add_plugin(SQ8L ${SQ8L_DPF_OPTIONS} MONOLITHIC
    TARGETS ${SQ8L_FORMATS}
    UI_TYPE opengl
    FILES_DSP src/plugin/SQ8LPlugin.cpp
              src/plugin/MtsEspTuning.cpp
              ${CMAKE_SOURCE_DIR}/third_party/MTS-ESP/Client/libMTSClient.cpp
    FILES_UI src/plugin/SQ8LUI.cpp
             $<$<PLATFORM_ID:Darwin>:${CMAKE_SOURCE_DIR}/src/plugin/mac/PlatformUiMac.mm>
             $<$<PLATFORM_ID:Windows>:${CMAKE_SOURCE_DIR}/src/plugin/win/PlatformUiWin.cpp>
             $<$<PLATFORM_ID:Windows>:${CMAKE_SOURCE_DIR}/src/plugin/win/DarkMenu.cpp>
             $<$<PLATFORM_ID:Linux>:${CMAKE_SOURCE_DIR}/src/plugin/linux/X11Pointer.cpp>
             $<$<PLATFORM_ID:Linux>:${CMAKE_SOURCE_DIR}/src/plugin/linux/ModalFiber.cpp>)
  target_include_directories(SQ8L PUBLIC src/plugin)
  target_link_libraries(SQ8L PUBLIC sq8l_engine)
  target_link_libraries(SQ8L-dsp PUBLIC sq8l_sound_parameter_service sq8l_version)
  add_dependencies(SQ8L-dsp sq8l_version_header)
  # MTS-ESP client (third_party/MTS-ESP, 0BSD): dlopens the master's library at run time.
  target_include_directories(SQ8L-dsp PUBLIC ${CMAKE_SOURCE_DIR}/third_party/MTS-ESP/Client)
  if(NOT WIN32)
    target_link_libraries(SQ8L-dsp PUBLIC ${CMAKE_DL_LIBS})
  endif()
  target_link_libraries(SQ8L-ui PUBLIC sq8l_gui sq8l_gui_logic sq8l_gui_drawn sq8l_sound_parameter_service)
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

# The Audio Unit's presets and state as a host drives them (tests/au_check.cpp).
if(APPLE)
  add_executable(sq8l_au_check tests/au_check.cpp)
  target_link_libraries(sq8l_au_check PRIVATE sq8l_engine "-framework AudioToolbox" "-framework CoreFoundation")
endif()

# Native full-render checker (tests/render_check.cpp), see tests/export_golden_raw.py.
add_executable(sq8l_render_check tests/render_check.cpp)
target_link_libraries(sq8l_render_check PRIVATE sq8l_engine)
target_compile_options(sq8l_render_check PRIVATE ${SQ8L_FP_FLAGS})

# Frozen semantic parameter identities and codecs, checked against the actual page builder.
enable_testing()
add_executable(sq8l_sound_parameters_check tests/sound_parameters_check.cpp)
target_link_libraries(sq8l_sound_parameters_check PRIVATE sq8l_gui_logic sq8l_engine)
add_test(NAME sound_parameters COMMAND sq8l_sound_parameters_check
  ${CMAKE_SOURCE_DIR}/tests/fixtures/sound_parameter_registry.json)

# Framework-independent service; engine ownership is supplied by callers.
add_library(sq8l_sound_parameter_service STATIC
  src/plugin/SoundParameterService.cpp src/plugin/SoundParameterState.cpp)
target_include_directories(sq8l_sound_parameter_service PUBLIC src/plugin)
target_link_libraries(sq8l_sound_parameter_service PUBLIC sq8l_engine)
target_compile_options(sq8l_sound_parameter_service PRIVATE ${SQ8L_FP_FLAGS} ${SQ8L_WARN_FLAGS})
find_package(Threads REQUIRED)
add_executable(sq8l_sound_parameter_service_check tests/sound_parameter_service_check.cpp)
target_link_libraries(sq8l_sound_parameter_service_check PRIVATE sq8l_sound_parameter_service Threads::Threads)
add_test(NAME sound_parameter_service COMMAND sq8l_sound_parameter_service_check)
set_tests_properties(sound_parameter_service PROPERTIES TIMEOUT 30)

add_executable(sq8l_sound_parameter_lifecycle_check tests/sound_parameter_lifecycle_check.cpp)
target_link_libraries(sq8l_sound_parameter_lifecycle_check PRIVATE sq8l_sound_parameter_service Threads::Threads)
add_test(NAME sound_parameter_lifecycle COMMAND sq8l_sound_parameter_lifecycle_check)
set_tests_properties(sound_parameter_lifecycle PROPERTIES TIMEOUT 30)

# Keep sanitizer evidence separate from timing/release builds. Instrument the
# service, its engine codecs and this native checker, without global tool flags.
option(SQ8L_SOUND_PARAMETER_TSAN "Instrument native parameter service checks with TSan" OFF)
if(SQ8L_SOUND_PARAMETER_TSAN)
  foreach(t sq8l_engine sq8l_sound_parameter_service sq8l_sound_parameter_service_check sq8l_sound_parameter_lifecycle_check)
    target_compile_options(${t} PRIVATE -fsanitize=thread -g)
    target_link_options(${t} PUBLIC -fsanitize=thread)
  endforeach()
endif()

add_executable(sq8l_clap_panel_notifications_check tests/clap_panel_notifications_check.cpp)
target_link_libraries(sq8l_clap_panel_notifications_check PRIVATE sq8l_sound_parameter_service)
add_test(NAME clap_panel_notifications COMMAND sq8l_clap_panel_notifications_check)

# Native host-side ABI regression; no test-only entry points in the plugin.
if(SQ8L_BUILD_PLUGIN)
  add_executable(sq8l_clap_parameter_check tests/clap_parameter_check.cpp)
  target_include_directories(sq8l_clap_parameter_check PRIVATE third_party/DPF/distrho/src)
  target_link_libraries(sq8l_clap_parameter_check PRIVATE sq8l_engine Threads::Threads ${CMAKE_DL_LIBS})
  add_executable(sq8l_vst3_parameter_check tests/vst3_parameter_check.cpp)
  target_include_directories(sq8l_vst3_parameter_check PRIVATE third_party/DPF/distrho/src)
  target_link_libraries(sq8l_vst3_parameter_check PRIVATE sq8l_engine Threads::Threads ${CMAKE_DL_LIBS})
  # Resolve each real format artifact through DPF's existing targets.
  foreach(format clap vst3)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${format}-abi-runtime")
    add_test(NAME ${format}_parameter_abi COMMAND ${CMAKE_COMMAND} -E env
      "SQ8L_DATA_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/${format}-abi-runtime"
      $<TARGET_FILE:sq8l_${format}_parameter_check> --plugin $<TARGET_FILE:SQ8L-${format}>)
    set_tests_properties(${format}_parameter_abi PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
  endforeach()
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME vst2_parameter_abi COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_plugin_vst2.py --automation-only
      --plugin $<TARGET_FILE:SQ8L-vst2>)
    set_tests_properties(vst2_parameter_abi PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
  endif()
  # cl.exe has no supported equivalent of the fixture's private-access flag.
  # The actual CLAP ABI test above remains enabled with MSVC.
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # Exact CLAP idle dispatch; access existing private wrapper state only in this fixture.
    add_executable(sq8l_clap_idle_refresh_check tests/clap_idle_refresh_check.cpp
      third_party/DPF/distrho/DistrhoUIMain.cpp)
    target_compile_definitions(sq8l_clap_idle_refresh_check PRIVATE DISTRHO_PLUGIN_TARGET_CLAP)
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
      target_compile_options(sq8l_clap_idle_refresh_check PRIVATE /clang:-fno-access-control)
    else()
      target_compile_options(sq8l_clap_idle_refresh_check PRIVATE -fno-access-control)
    endif()
    target_link_libraries(sq8l_clap_idle_refresh_check PRIVATE SQ8L-dsp SQ8L-ui ${CMAKE_DL_LIBS})
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/clap-idle-runtime")
    add_test(NAME clap_idle_refresh COMMAND ${CMAKE_COMMAND} -E env
      "SQ8L_DATA_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/clap-idle-runtime"
      $<TARGET_FILE:sq8l_clap_idle_refresh_check>)
  else()
    message(STATUS "CLAP source-linked idle fixture unavailable with cl.exe; actual ABI checker remains enabled")
  endif()
  # Exercise the exact native wrapper idle handler without creating a desktop UI.
  add_executable(sq8l_vst3_idle_refresh_check tests/vst3_idle_refresh_check.cpp
    third_party/DPF/distrho/DistrhoUIMain.cpp)
  target_compile_definitions(sq8l_vst3_idle_refresh_check PRIVATE DISTRHO_PLUGIN_TARGET_VST3)
  target_link_libraries(sq8l_vst3_idle_refresh_check PRIVATE SQ8L-dsp SQ8L-ui ${CMAKE_DL_LIBS})
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vst3-idle-runtime")
  add_test(NAME vst3_idle_refresh COMMAND ${CMAKE_COMMAND} -E env
    "SQ8L_DATA_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/vst3-idle-runtime"
    $<TARGET_FILE:sq8l_vst3_idle_refresh_check>)
endif()

# Qualification recorder is opt-in and separate from optimized timing/release builds.
option(SQ8L_SOUND_PARAMETER_INSTRUMENTATION "Record native callback/mutex qualification scopes" OFF)
if(SQ8L_SOUND_PARAMETER_INSTRUMENTATION)
  target_compile_definitions(sq8l_sound_parameter_service PUBLIC SQ8L_SOUND_PARAMETER_INSTRUMENTATION=1)
endif()
if(SQ8L_BUILD_PLUGIN)
  add_executable(sq8l_sound_parameter_qualification tests/sound_parameter_qualification.cpp
    third_party/DPF/distrho/DistrhoUIMain.cpp)
  target_compile_definitions(sq8l_sound_parameter_qualification PRIVATE DISTRHO_PLUGIN_TARGET_VST3
    SQ8L_BENCHMARK_OPTIMIZED=$<AND:$<CONFIG:Release>,$<NOT:$<BOOL:${SQ8L_SOUND_PARAMETER_TSAN}>>>)
  target_compile_options(sq8l_sound_parameter_qualification PRIVATE ${SQ8L_FP_FLAGS})
  target_link_libraries(sq8l_sound_parameter_qualification PRIVATE SQ8L-dsp SQ8L-ui Threads::Threads ${CMAKE_DL_LIBS})
  if(SQ8L_SOUND_PARAMETER_TSAN)
    foreach(t SQ8L-dsp sq8l_sound_parameter_qualification)
      target_compile_options(${t} PRIVATE -fsanitize=thread -g)
      target_link_options(${t} PUBLIC -fsanitize=thread)
    endforeach()
  endif()
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qualification-runtime")
  foreach(mode parity instrumentation)
    if(mode STREQUAL "instrumentation" AND NOT SQ8L_SOUND_PARAMETER_INSTRUMENTATION)
      continue()
    endif()
    add_test(NAME sound_parameter_qualification_${mode} COMMAND ${CMAKE_COMMAND} -E env
      "SQ8L_DATA_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/qualification-runtime"
      $<TARGET_FILE:sq8l_sound_parameter_qualification> --${mode})
    set_tests_properties(sound_parameter_qualification_${mode} PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
  endforeach()
endif()

# Opt-in isolated pre-change plugin comparator. Archives are read-only inputs.
# Generated copies keep the current pure codec's quoted includes on archived Program.h.
set(SQ8L_AUTOMATION_BASELINE_SOURCE "" CACHE PATH "Preserved pre-change source for native plugin timing comparator")
if(SQ8L_AUTOMATION_BASELINE_SOURCE)
  if(SQ8L_SOUND_PARAMETER_INSTRUMENTATION OR SQ8L_SOUND_PARAMETER_TSAN OR NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "Baseline timing comparator requires uninstrumented Release configuration")
  endif()
  set(baseline_helpers "${CMAKE_CURRENT_BINARY_DIR}/baseline-helpers")
  file(MAKE_DIRECTORY "${baseline_helpers}")
  foreach(name SoundParameters.h SoundParameters.cpp SoundParameterLabels.h SoundParameterLabels.cpp)
    configure_file("src/engine/${name}" "${baseline_helpers}/${name}" COPYONLY)
  endforeach()
  foreach(name SQ8LPlugin.hpp SQ8LPlugin.cpp)
    configure_file("${SQ8L_AUTOMATION_BASELINE_SOURCE}/src/plugin/${name}" "${baseline_helpers}/${name}" COPYONLY)
  endforeach()
  # Sole archived helper adaptation: route fixture persistence to its explicit directory.
  file(READ "${SQ8L_AUTOMATION_BASELINE_SOURCE}/src/plugin/SharedLibrary.h" baseline_shared)
  string(REPLACE "inline std::string userDataDir() {"
    "inline std::string userDataDir() {\n    if (const char* directory = std::getenv(\"SQ8L_DATA_DIRECTORY\")) {\n        if (*directory) return std::string(directory) + \"/\";\n    }"
    baseline_shared "${baseline_shared}")
  file(WRITE "${baseline_helpers}/SharedLibrary.h" "${baseline_shared}")
  file(GLOB baseline_engine CONFIGURE_DEPENDS
    "${SQ8L_AUTOMATION_BASELINE_SOURCE}/src/engine/*.cpp"
    "${SQ8L_AUTOMATION_BASELINE_SOURCE}/src/engine/data/*.cpp")
  set(baseline_crmath)
  foreach(name exp.c log.c sin.c cos.c tan.c)
    list(APPEND baseline_crmath "${SQ8L_AUTOMATION_BASELINE_SOURCE}/third_party/core-math/${name}")
  endforeach()
  add_library(sq8l_automation_baseline_engine STATIC ${baseline_engine} ${baseline_crmath})
  target_include_directories(sq8l_automation_baseline_engine PUBLIC "${SQ8L_AUTOMATION_BASELINE_SOURCE}/src/engine")
  target_compile_options(sq8l_automation_baseline_engine PRIVATE ${SQ8L_FP_FLAGS})
  set(baseline_dpf "${SQ8L_AUTOMATION_BASELINE_SOURCE}/third_party/DPF/distrho")
  add_executable(sq8l_automation_benchmark_baseline tests/automation_benchmark_baseline.cpp
    "${baseline_helpers}/SQ8LPlugin.cpp"
    "${baseline_helpers}/SoundParameters.cpp" "${baseline_helpers}/SoundParameterLabels.cpp"
    "${SQ8L_AUTOMATION_BASELINE_SOURCE}/src/plugin/MtsEspTuning.cpp"
    "${SQ8L_AUTOMATION_BASELINE_SOURCE}/third_party/MTS-ESP/Client/libMTSClient.cpp"
    "${baseline_dpf}/src/DistrhoPlugin.cpp" "${baseline_dpf}/src/DistrhoUtils.cpp")
  target_include_directories(sq8l_automation_benchmark_baseline BEFORE PRIVATE "${baseline_helpers}"
    "${SQ8L_AUTOMATION_BASELINE_SOURCE}/src/plugin" "${baseline_dpf}"
    "${SQ8L_AUTOMATION_BASELINE_SOURCE}/third_party/MTS-ESP/Client")
  target_compile_definitions(sq8l_automation_benchmark_baseline PRIVATE DISTRHO_PLUGIN_TARGET_VST3 DISTRHO_IS_STANDALONE=0)
  target_link_libraries(sq8l_automation_benchmark_baseline PRIVATE sq8l_automation_baseline_engine sq8l_version Threads::Threads ${CMAKE_DL_LIBS})
  add_dependencies(sq8l_automation_benchmark_baseline sq8l_version_header)
  target_compile_options(sq8l_automation_benchmark_baseline PRIVATE ${SQ8L_FP_FLAGS})
endif()
