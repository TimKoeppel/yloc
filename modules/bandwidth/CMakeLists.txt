string(REPLACE "-" "_" MOD_CPPNAME "${MOD_TARGET}")

configure_file("interface_impl.cc.in" "interface_impl.cc")

set(MOD_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/interface_impl.cc" "init_graph.cc")

add_library(${MOD_TARGET} OBJECT "${MOD_SOURCES}")
target_compile_options(${MOD_TARGET} PRIVATE -O3 -march=native)

set_property(TARGET ${MOD_TARGET} PROPERTY POSITION_INDEPENDENT_CODE ON)

# This include directory is required so that the generated file can include local headers
target_include_directories(${MOD_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")