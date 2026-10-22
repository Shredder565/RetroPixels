expr, kept in this folder (third-party code, MIT, LICENSE)

include/expr.h is Serge Zaitsev's expr (https://github.com/zserge/expr,
MIT), the tiny expression parser behind conditional breakpoints, in the
extended form PixelU-Wii's Core/PowerPC/Expression.cpp is written against.

Upstream expr (last change December 2017, commit 5e7d72c) evaluates in
float and has no strings. The dialect the breakpoint conditions use was added
inside the Dolphin project between 2020 and 2023: double values and 64-bit
integer operators, hexadecimal and exponent literals through strtod, "~" as
bitwise NOT (so "^" is XOR), string literals read with expr_get_str(), and
C++-compatible names (expr_get_func, expr_get_var). Upstream cannot
reproduce that, so this one header was copied unchanged from Dolphin's
Externals/expr (Dolphin 2609a, commit 409881c3) rather than fetched; the
licence stays expr's MIT licence. Replacing it with an own parser is a
separate task (it would change Expression.cpp, which is not in this batch).
