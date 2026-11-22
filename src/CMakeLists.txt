# B2b：core 与 IPC 客户端都是 header-only；B4 再加 node
add_library(teleop_core INTERFACE)
target_include_directories(teleop_core INTERFACE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(teleop_core INTERFACE cxx_std_20)
target_link_libraries(teleop_core INTERFACE teleop_ipc)

add_library(teleop_ipc_client INTERFACE)
target_include_directories(teleop_ipc_client INTERFACE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(teleop_ipc_client INTERFACE cxx_std_20)
target_link_libraries(teleop_ipc_client INTERFACE teleop_core teleop_ipc)

# B4: add_subdirectory(node)
