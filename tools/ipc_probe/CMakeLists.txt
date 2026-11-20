add_executable(ipc_probe main.cpp)
target_link_libraries(ipc_probe PRIVATE teleop_ipc_client teleop_options)
set_target_properties(ipc_probe PROPERTIES OUTPUT_NAME ipc-probe)
install(TARGETS ipc_probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
