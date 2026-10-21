# ed25519 by Orson Peters (Zlib licence, vendored in Dolphin's Externals):
# signature checks in UpdaterCommon.
project(ed25519 C)
set(src "${THIRD}/ed25519")

set(parts add_scalar fe ge key_exchange keypair sc seed sha512 sign verify)
list(TRANSFORM parts PREPEND "${src}/")
list(TRANSFORM parts APPEND ".c")
add_library(ed25519 ${parts})
pixelu_third_party_target(ed25519)
