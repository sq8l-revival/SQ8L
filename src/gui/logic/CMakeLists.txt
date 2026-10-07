# Editor logic (layer 3 of docs/GUI_ARCHITECTURE.md): plugEdit / lcdControl / lcdSetup /
# formatters / dialogs, driving the ported controls of sq8l_gui.
file(GLOB SQ8L_GUI_LOGIC_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/*.cpp)
add_library(sq8l_gui_logic STATIC ${SQ8L_GUI_LOGIC_SOURCES})
target_include_directories(sq8l_gui_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sq8l_gui_logic PUBLIC sq8l_gui sq8l_engine)
target_compile_options(sq8l_gui_logic PRIVATE ${SQ8L_FP_FLAGS})
target_compile_options(sq8l_gui_logic PRIVATE ${SQ8L_WARN_FLAGS})
