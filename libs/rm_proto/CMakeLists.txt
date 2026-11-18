# header-only：只有头文件，没有编译单元
add_library(rm_proto INTERFACE)
add_library(teleop::rm_proto ALIAS rm_proto)

target_compile_features(rm_proto INTERFACE cxx_std_20)

target_include_directories(
    rm_proto
    INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
              $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/proto>
)

target_link_libraries(rm_proto INTERFACE teleop_proto)
