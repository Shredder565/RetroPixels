# Tests of RetroPixels' 68000 for the Model 3 engine.
#
#   make -C supermodel/tests/m68k        builds and runs the wrapper test
#
# The instruction-by-instruction comparison with the Go 68000 (cases.cpp)
# runs from Go: go test ./cores/m68k -run Supermodel

CXX      ?= g++
SRC      := ../../Src
OUT      ?= build
CXXFLAGS := -std=c++17 -O2 -Wall -Werror -I$(SRC) -I$(SRC)/OSD/SDL -I$(SRC)/CPU/68K

.PHONY: test
test: $(OUT)/wrapper
	cd $(OUT) && ./wrapper

$(OUT)/wrapper: wrapper.cpp $(SRC)/CPU/68K/68K.cpp $(SRC)/CPU/68K/M68000.cpp $(SRC)/BlockFile.cpp $(SRC)/CPU/68K/68K.h $(SRC)/CPU/68K/M68000.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ wrapper.cpp $(SRC)/CPU/68K/68K.cpp $(SRC)/CPU/68K/M68000.cpp $(SRC)/BlockFile.cpp
