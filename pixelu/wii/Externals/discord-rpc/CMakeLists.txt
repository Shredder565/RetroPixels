# discord-rpc (MIT, vendored in Dolphin's Externals with its bundled RapidJSON):
# Discord Rich Presence. Built here from its Windows sources as a static library
# with the background I/O thread.
cmake_minimum_required(VERSION 3.2.0...4.1.1)
project(DiscordRPC)
set(src "${THIRD}/discord-rpc")

add_library(discord-rpc)
pixelu_add_sources(discord-rpc
  IN "${src}/include" discord_rpc.h discord_register.h
  IN "${src}/src"
    discord_rpc.cpp rpc_connection.cpp rpc_connection.h serialization.cpp serialization.h
    connection.h backoff.h msg_queue.h connection_win.cpp discord_register_win.cpp
)
target_include_directories(discord-rpc PRIVATE "${src}/include" "${src}/thirdparty/include")
target_compile_definitions(discord-rpc PRIVATE DISCORD_WINDOWS)
target_link_libraries(discord-rpc PRIVATE psapi advapi32)
pixelu_third_party_target(discord-rpc)
