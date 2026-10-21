# cpp-ipc (MIT): shared-memory channel of the "IPC" broadband adapter mode. Its
# own CMake project. Gives cpp-ipc::ipc.
add_subdirectory("${THIRD}/cpp-ipc/cpp-ipc" "${CMAKE_CURRENT_BINARY_DIR}/cpp-ipc" EXCLUDE_FROM_ALL)
if(NOT MSVC)
  target_compile_options(ipc PRIVATE -fexceptions)
endif()
add_library(cpp-ipc::ipc ALIAS ipc)
pixelu_third_party_target(ipc)
