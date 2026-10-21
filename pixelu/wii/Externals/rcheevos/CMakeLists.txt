# rcheevos (MIT): the RetroAchievements client. Built from its sources without
# Lua, libretro support or the ROM, zip and encrypted-disc hashers, which a
# GameCube/Wii emulator does not need; with the RAIntegration bridge on Windows.
set(src "${THIRD}/rcheevos/rcheevos")

file(GLOB rcheevos_files
  "${src}/include/*.h"
  "${src}/src/*.c" "${src}/src/*.h"
  "${src}/src/rapi/*.c" "${src}/src/rapi/*.h"
  "${src}/src/rcheevos/*.c" "${src}/src/rcheevos/*.h"
  "${src}/src/rhash/*.c" "${src}/src/rhash/*.h"
)
list(FILTER rcheevos_files EXCLUDE REGEX "/(rc_libretro\\.[ch]|hash_encrypted\\.c|hash_rom\\.c|hash_zip\\.c)$")

add_library(rcheevos ${rcheevos_files})
target_include_directories(rcheevos
  PRIVATE "${src}/include"
  # Users include <rcheevos/include/rc_client.h>.
  INTERFACE "${THIRD}/rcheevos"
)
target_compile_definitions(rcheevos PRIVATE
  RC_DISABLE_LUA=1
  RCHEEVOS_URL_SSL
  RC_CLIENT_SUPPORTS_HASH
  RC_CLIENT_SUPPORTS_EXTERNAL
  RC_HASH_NO_ENCRYPTED
  RC_HASH_NO_ROM
  RC_HASH_NO_ZIP
  $<$<BOOL:${RC_CLIENT_SUPPORTS_RAINTEGRATION}>:RC_CLIENT_SUPPORTS_RAINTEGRATION>
  _CRT_SECURE_NO_WARNINGS
)
pixelu_third_party_target(rcheevos)
