set(CORE_SOURCES
  src/frame.cpp
  src/display.cpp
  src/buttons.cpp
  src/application.cpp
  src/ssd2683.cpp
  src/font.cpp
  src/calendar_config.cpp
  src/ics_parser.cpp
  src/calendar_renderer.cpp
)
if(ESP_PLATFORM)
  idf_component_register(SRCS ${CORE_SOURCES} INCLUDE_DIRS include)
  target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
else()
  add_library(note4c_core ${CORE_SOURCES})
  target_include_directories(note4c_core PUBLIC include)
  target_compile_options(note4c_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
