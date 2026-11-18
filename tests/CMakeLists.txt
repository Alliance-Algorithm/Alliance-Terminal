find_package(GTest REQUIRED)

# 测试代码不套用生产告警策略：gtest 头文件在 -Werror 下容易误报
function(teleop_add_unit_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE rm_proto GTest::gtest_main)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

teleop_add_unit_test(test_topics unit/test_topics.cpp)
teleop_add_unit_test(test_codec unit/test_codec.cpp)
