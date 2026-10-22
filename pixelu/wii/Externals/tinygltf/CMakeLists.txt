# TinyGLTF (MIT): loads glTF meshes for graphics mods. No image loading of its own
# and no exceptions.
set(src "${THIRD}/tinygltf")

add_library(tinygltf STATIC "${src}/tinygltf/tiny_gltf.cc")
target_include_directories(tinygltf INTERFACE "${src}")
target_compile_definitions(tinygltf PUBLIC
  TINYGLTF_NOEXCEPTION
  TINYGLTF_NO_EXTERNAL_IMAGE
  TINYGLTF_USE_CPP14
)
if(NOT MSVC)
  target_compile_features(tinygltf PRIVATE cxx_std_20)
endif()
pixelu_third_party_target(tinygltf)
