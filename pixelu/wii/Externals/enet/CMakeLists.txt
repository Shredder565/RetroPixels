# ENet (MIT): netplay transport. Its own CMake project builds the library.
set(src "${THIRD}/enet/enet")
add_subdirectory("${src}" "${CMAKE_CURRENT_BINARY_DIR}/enet")
target_include_directories(enet PUBLIC "${src}/include")
add_library(enet::enet ALIAS enet)
pixelu_third_party_target(enet)
