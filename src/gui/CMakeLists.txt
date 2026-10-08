# SQ8L editor GUI (framework independent): graphics core, assets, ported controls, EditorView,
# HD rendering.
file(GLOB SQ8L_GUI_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/controls/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/hd/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/text/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/*.cpp)
add_library(sq8l_gui STATIC ${SQ8L_GUI_SOURCES})
target_include_directories(sq8l_gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Knob arithmetic mirrors the original's x87 code: same strict FP flags as the engine.
target_compile_options(sq8l_gui PRIVATE ${SQ8L_FP_FLAGS})
target_compile_options(sq8l_gui PRIVATE ${SQ8L_WARN_FLAGS})
