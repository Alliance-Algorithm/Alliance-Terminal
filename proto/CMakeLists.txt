find_package(Protobuf REQUIRED)

if(TARGET protobuf::protoc)
    set(_teleop_protoc protobuf::protoc)
else()
    set(_teleop_protoc ${Protobuf_PROTOC_EXECUTABLE})
endif()

set(_gen_dir ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${_gen_dir})

set(_proto_files
    rm/terminal.proto
    ipc.proto
    ui_link.proto
)

set(_generated_sources)
foreach(_proto IN LISTS _proto_files)
    get_filename_component(_base ${_proto} NAME_WE)
    get_filename_component(_dir ${_proto} DIRECTORY)
    if(_dir)
        set(_rel ${_dir}/${_base})
    else()
        set(_rel ${_base})
    endif()

    set(_cc ${_gen_dir}/${_rel}.pb.cc)
    set(_hh ${_gen_dir}/${_rel}.pb.h)

    add_custom_command(
        OUTPUT ${_cc} ${_hh}
        COMMAND ${_teleop_protoc}
        ARGS --cpp_out=${_gen_dir} -I${CMAKE_CURRENT_SOURCE_DIR}
             ${CMAKE_CURRENT_SOURCE_DIR}/${_proto}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_proto} ${_teleop_protoc}
        COMMENT "protoc ${_proto}"
        VERBATIM
    )
    list(APPEND _generated_sources ${_cc})
endforeach()

add_library(teleop_proto STATIC ${_generated_sources})
add_library(teleop::proto ALIAS teleop_proto)

target_link_libraries(teleop_proto PUBLIC protobuf::libprotobuf)
# 生成代码不参与本仓库的告警策略
target_include_directories(teleop_proto SYSTEM PUBLIC ${_gen_dir})
