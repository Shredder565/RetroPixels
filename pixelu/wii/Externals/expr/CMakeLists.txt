# expr by Serge Zaitsev (MIT, header-only): the expression parser behind
# conditional breakpoints. Kept in this folder (include/expr.h), see README.txt.
add_library(expr INTERFACE)
target_include_directories(expr INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
