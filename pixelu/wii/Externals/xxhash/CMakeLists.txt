# xxHash (BSD-2-Clause): fast hashes for the shader and texture caches.
project(xxhash C)
set(src "${THIRD}/xxhash/xxHash")

add_library(xxhash STATIC "${src}/xxhash.c")
target_include_directories(xxhash PUBLIC "${src}")
add_library(xxhash::xxhash ALIAS xxhash)
pixelu_third_party_target(xxhash)
