# SFML 3 (Zlib licence): only its Network module (and the bits of System it
# needs), for netplay, the GameCube BBA and the DSU client.
set(src "${THIRD}/SFML/SFML")

add_library(sfml-system STATIC)
pixelu_add_sources(sfml-system
  IN "${src}/src/SFML/System" Err.cpp String.cpp Utils.cpp
  IN "${src}/include/SFML/System" String.hpp
)
target_compile_definitions(sfml-system PUBLIC SFML_STATIC)

add_library(sfml-network STATIC)
pixelu_add_sources(sfml-network
  IN "${src}/src/SFML/Network"
    Http.cpp IpAddress.cpp Packet.cpp Socket.cpp SocketSelector.cpp
    TcpListener.cpp TcpSocket.cpp UdpSocket.cpp
)
if(WIN32)
  pixelu_add_sources(sfml-network IN "${src}/src/SFML/Network" Win32/SocketImpl.cpp)
else()
  pixelu_add_sources(sfml-network IN "${src}/src/SFML/Network" Unix/SocketImpl.cpp)
endif()
target_link_libraries(sfml-network PUBLIC sfml-system)

foreach(target sfml-system sfml-network)
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_include_directories(${target} PUBLIC "${src}/include" PRIVATE "${src}/src")
  pixelu_third_party_target(${target})
endforeach()
