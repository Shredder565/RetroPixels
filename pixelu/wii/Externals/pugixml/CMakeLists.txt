# pugixml (MIT), built with its own CMake project: the static library only, no
# exceptions, no install rules. Gives the "pugixml" target.
set(PUGIXML_NO_EXCEPTIONS ON)
set(PUGIXML_STATIC_CRT OFF)
set(PUGIXML_BUILD_DEFINES OFF)
set(PUGIXML_INSTALL OFF)
add_subdirectory("${THIRD}/pugixml/pugixml" "${CMAKE_CURRENT_BINARY_DIR}/pugixml")
pixelu_third_party_target(pugixml-static)
