# expr (MIT, header-only, vendored in Dolphin's Externals): the expression parser
# behind conditional breakpoints.
add_library(expr INTERFACE)
target_include_directories(expr INTERFACE "${THIRD}/expr/include/")
