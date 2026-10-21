# {fmt} (MIT), built with its own CMake project; gives fmt::fmt.
add_subdirectory("${THIRD}/fmt/fmt" "${CMAKE_CURRENT_BINARY_DIR}/fmt")
pixelu_third_party_target(fmt)
