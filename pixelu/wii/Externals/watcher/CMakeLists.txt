# watcher (MIT, header-only): watches the folders of loaded mods and textures.
add_library(watcher INTERFACE IMPORTED GLOBAL)
set_target_properties(watcher PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${THIRD}/watcher/watcher/include")
