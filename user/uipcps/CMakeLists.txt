option(USE_QOS_CUBES, "Use obsolete static QoS cubes, specified by configuration file" OFF)
if (USE_QOS_CUBES)
    add_definitions(-D RL_USE_QOS_CUBES)
endif()

file(GLOB UIPCP_GPB_PROTOFILES "uipcp-gpb/*.proto")

# Protocol buffer processing
protobuf_generate_cpp(UIPCP_GPB_SRC UIPCP_GPB_HDR ${UIPCP_GPB_PROTOFILES})

# Libraries generated by the project
add_library(uipcp-normal STATIC uipcp-normal.cpp uipcp-normal-codecs.cpp uipcp-normal.hpp uipcp-normal-enroll.cpp uipcp-normal-flow-alloc.cpp uipcp-normal-appl-reg.cpp uipcp-normal-lower-flows.cpp uipcp-normal-qos.cpp ${UIPCP_GPB_SRC} ${UIPCP_GPB_HDR})
target_link_libraries(uipcp-normal ${CMAKE_THREAD_LIBS_INIT} rlite-cdap rlite-evloop)

message(STATUS "Adding include dir ${CMAKE_CURRENT_BINARY_DIR} to uipcp-normal target")
target_include_directories(uipcp-normal PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

# Executables generated by the project
add_executable(rlite-uipcps uipcp-container.c uipcp-unix.c uipcp-shim-tcp4.c uipcp-shim-udp4.c)
target_link_libraries(rlite-uipcps rlite rlite-evloop rlite-conf uipcp-normal)

# Installation directives
install(TARGETS rlite-uipcps DESTINATION usr/bin)
install(FILES shim-tcp4-dir DESTINATION etc/rlite)

if (USE_QOS_CUBES)
    install(FILES uipcp-qoscubes.qos DESTINATION etc/rlite)
endif()

