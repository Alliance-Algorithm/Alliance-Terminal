find_package(GTest REQUIRED)

# 测试代码不套用生产告警策略：gtest 头文件在 -Werror 下容易误报
function(teleop_add_unit_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE teleop_ipc_client teleop_core teleop_ipc rm_proto teleop_proto GTest::gtest_main)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

teleop_add_unit_test(test_topics unit/test_topics.cpp)
teleop_add_unit_test(test_codec unit/test_codec.cpp)
teleop_add_unit_test(test_core unit/test_core.cpp)
teleop_add_unit_test(test_frame unit/test_frame.cpp)
teleop_add_unit_test(test_seqpacket unit/test_seqpacket.cpp)
teleop_add_unit_test(test_session unit/test_session.cpp)
teleop_add_unit_test(test_control_link unit/test_control_link.cpp)
teleop_add_unit_test(test_ipc_client unit/test_ipc_client.cpp)

# 公共头文件用生产告警策略（ci 下含 -Werror）单独编一遍
add_executable(test_ipc_headers unit/test_ipc_headers.cpp)
target_link_libraries(test_ipc_headers PRIVATE teleop_ipc_client teleop_core teleop_ipc teleop_options)
add_test(NAME test_ipc_headers COMMAND test_ipc_headers)
