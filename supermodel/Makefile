# RetroPixels' Model 3 engine (supermodel / supermodel.exe), built from Src/.
# supermodel/build.sh runs this with the pinned libraries; by hand:
#
#   make -C supermodel                         Linux, with the system's SDL2, SDL2_net, GL, GLU and zlib
#   make -C supermodel TARGET=windows CROSS=x86_64-w64-mingw32- SDL2_CONFIG=<prefix>/bin/sdl2-config
#                                              64-bit Windows with MinGW-w64, statically linked
#
# Settings (make VAR=value):
#   TARGET       linux (default) or windows
#   CROSS        tool prefix for the compilers and windres (windows)
#   CC, CXX      compilers (default: $(CROSS)gcc, $(CROSS)g++)
#   SDL2_CONFIG  sdl2-config of the SDL2 to build with
#   EXTRA_CFLAGS, EXTRA_LDFLAGS, LIBS
#                added to the compile flags, link flags and libraries
#   VERSION      the version string (default rp-dev; build.sh gives
#                rp-<commit date>-<commit>)
#   OUT          where objects and the program go (default build/<TARGET>)
#   DEBUG=1      no optimisation, debug info, not stripped

TARGET      ?= linux
VERSION     ?= rp-dev
OUT         ?= build/$(TARGET)
SDL2_CONFIG ?= sdl2-config
CROSS       ?=
CC          := $(CROSS)gcc
CXX         := $(CROSS)g++
WINDRES     ?= $(CROSS)windres
LIBS        ?=

# ---- what goes in -----------------------------------------------------------

SOURCES := \
	BlockFile.cpp GameLoader.cpp ROMSet.cpp \
	CPU/68K/68K.cpp CPU/68K/M68000.cpp \
	CPU/PowerPC/ppc.cpp CPU/PowerPC/PPCDisasm.cpp \
	CPU/Z80/Z80.cpp \
	Graphics/FBO.cpp Graphics/Render2D.cpp Graphics/Shader.cpp Graphics/SuperAA.cpp \
	Graphics/Legacy3D/Error.cpp Graphics/Legacy3D/Legacy3D.cpp Graphics/Legacy3D/Models.cpp \
	Graphics/Legacy3D/TextureRefs.cpp \
	Graphics/New3D/GLSLShader.cpp Graphics/New3D/Mat4.cpp Graphics/New3D/Model.cpp \
	Graphics/New3D/New3D.cpp Graphics/New3D/PolyHeader.cpp Graphics/New3D/R3DFloat.cpp \
	Graphics/New3D/R3DFrameBuffers.cpp Graphics/New3D/R3DScrollFog.cpp Graphics/New3D/R3DShader.cpp \
	Graphics/New3D/TextureBank.cpp Graphics/New3D/VBO.cpp Graphics/New3D/Vec.cpp \
	Inputs/Input.cpp Inputs/Inputs.cpp Inputs/InputSource.cpp Inputs/InputSystem.cpp \
	Inputs/InputTypes.cpp Inputs/MultiInputSource.cpp \
	Model3/53C810.cpp Model3/93C46.cpp Model3/Crypto.cpp Model3/DSB.cpp Model3/IRQ.cpp \
	Model3/JTAG.cpp Model3/Model3.cpp Model3/MPC10x.cpp Model3/PCI.cpp Model3/Real3D.cpp \
	Model3/RTC72421.cpp Model3/SoundBoard.cpp Model3/TileGen.cpp \
	Model3/DriveBoard/BillBoard.cpp Model3/DriveBoard/DriveBoard.cpp \
	Model3/DriveBoard/JoystickBoard.cpp Model3/DriveBoard/SkiBoard.cpp \
	Model3/DriveBoard/WheelBoard.cpp Model3/DriveBoard/Z80CTC.cpp \
	Network/NetBoard.cpp Network/SimNetBoard.cpp Network/TCPReceive.cpp Network/TCPSend.cpp \
	OSD/Logger.cpp OSD/Outputs.cpp OSD/RetroPixels.cpp \
	OSD/SDL/Audio.cpp OSD/SDL/Crosshair.cpp OSD/SDL/Gui.cpp OSD/SDL/Main.cpp \
	OSD/SDL/NetOutputs.cpp OSD/SDL/SDLInputSystem.cpp OSD/SDL/Thread.cpp \
	Sound/SCSP.cpp Sound/SCSPDSP.cpp Sound/MPEG/MpegAudio.cpp \
	Util/ByteSwap.cpp Util/ConfigBuilders.cpp Util/Format.cpp Util/NewConfig.cpp \
	Pkgs/glew.c Pkgs/ioapi.c Pkgs/unzip.c Pkgs/tinyxml2.cpp \
	Pkgs/imgui/imgui.cpp Pkgs/imgui/imgui_draw.cpp Pkgs/imgui/imgui_impl_opengl3.cpp \
	Pkgs/imgui/imgui_impl_sdl2.cpp Pkgs/imgui/imgui_tables.cpp Pkgs/imgui/imgui_widgets.cpp

ifeq ($(TARGET),windows)
SOURCES  += OSD/Windows/DirectInputSystem.cpp OSD/Windows/FileSystemPath.cpp OSD/Windows/WinOutputs.cpp
PROGRAM  := supermodel.exe
else ifeq ($(TARGET),linux)
SOURCES  += OSD/Unix/FileSystemPath.cpp
PROGRAM  := supermodel
else
$(error TARGET must be linux or windows)
endif

# Every source folder is on the include path (the code includes headers by
# their bare names), Src/ itself first.
INCLUDES := -ISrc $(addprefix -ISrc/,$(sort $(patsubst %/,%,$(filter-out ./,$(dir $(SOURCES))))))

# ---- flags --------------------------------------------------------------------

ifeq ($(DEBUG),1)
OPT    := -O0 -g
STRIP  :=
else
OPT    := -O3
STRIP  := -s
endif

DEFINES  := -DGLEW_STATIC -DSUPERMODEL_VERSION='"$(VERSION)"'
CFLAGS   := $(OPT) -Wall -std=c11 $(INCLUDES) $(DEFINES) $(EXTRA_CFLAGS)
CXXFLAGS := $(OPT) -Wall -std=c++17 $(INCLUDES) $(DEFINES) $(EXTRA_CFLAGS)

ifeq ($(TARGET),windows)
# A console program (the log and -help go to the console RetroPixels reads),
# so -mwindows is taken out of SDL's flags; everything linked statically.
SDL_CFLAGS := $(shell $(SDL2_CONFIG) --cflags)
SDL_LIBS   := $(filter-out -mwindows,$(shell $(SDL2_CONFIG) --static-libs))
CXXFLAGS   += $(SDL_CFLAGS) -DSUPERMODEL_WIN32
CFLAGS     += $(SDL_CFLAGS) -DSUPERMODEL_WIN32
LDFLAGS    := -static $(STRIP) $(EXTRA_LDFLAGS)
LDLIBS     := $(SDL_LIBS) -lSDL2_net -liphlpapi -ldxerr8 -ldinput8 -lglu32 -lole32 -loleaut32 \
              -lopengl32 -lwbemuuid -lws2_32 -lz $(LIBS)
RESOURCES  := $(OUT)/Supermodel.res.o
else
SDL_CFLAGS := $(shell $(SDL2_CONFIG) --cflags)
CXXFLAGS   += $(SDL_CFLAGS)
CFLAGS     += $(SDL_CFLAGS)
LDFLAGS    := $(STRIP) $(EXTRA_LDFLAGS)
LDLIBS     := $(if $(LIBS),$(LIBS),$(shell $(SDL2_CONFIG) --libs) -lSDL2_net -lGL -lGLU -lz -lm -lpthread)
RESOURCES  :=
endif

# ---- rules --------------------------------------------------------------------

OBJECTS := $(addprefix $(OUT)/obj/,$(addsuffix .o,$(basename $(SOURCES))))

.PHONY: all clean version
all: $(OUT)/$(PROGRAM)

version:
	@echo $(VERSION)

$(OUT)/$(PROGRAM): $(OBJECTS) $(RESOURCES)
	@echo "link    $@"
	@$(CXX) -o $@ $(OBJECTS) $(RESOURCES) $(LDFLAGS) $(LDLIBS)

$(OUT)/obj/%.o: Src/%.cpp
	@mkdir -p $(dir $@)
	@echo "c++     $<"
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# GLEW is third-party: its warnings aren't ours to fix.
$(OUT)/obj/Pkgs/glew.o: CFLAGS += -w

$(OUT)/obj/%.o: Src/%.c
	@mkdir -p $(dir $@)
	@echo "cc      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# The version string reaches the resource script through a generated header.
$(OUT)/Supermodel.res.o: Src/OSD/Windows/Supermodel.rc Src/OSD/Windows/Supermodel.manifest
	@mkdir -p $(dir $@)
	@echo '#define SM_VERSION_STR "$(VERSION)"' > $(OUT)/smversion.h
	@echo "windres $<"
	@$(WINDRES) --include-dir=Src/OSD/Windows --include-dir=$(OUT) -i $< -o $@

clean:
	rm -rf $(OUT)

-include $(OBJECTS:.o=.d)
