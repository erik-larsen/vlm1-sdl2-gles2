# Platform specifics (adapted from sgi-demos)
OS:=$(shell uname -s)
HW:=$(shell uname -m)
ifeq ($(OS),Darwin)
	OS = mac
	DYL_EXT = dylib
	CONSOLE_FLAGS =
else ifeq ($(OS),Linux)
	OS = linux
	DYL_EXT = so
	CONSOLE_FLAGS =
else ifneq ($(findstring MINGW64_NT,$(OS)),)
	OS = win
	DYL_EXT = dll
	CONSOLE_FLAGS = -mconsole
endif

# Source and executable dirs
SRC_DIR = ./src
BIN_DIR = ./bin-$(OS)-$(HW)
WEB_DIR = ./web
LIBS_DIR = ./libs

# SDL libs and include
SDL_LIBS = `sdl2-config --libs`
EM_SDL_LIBS = -s USE_SDL=2 -s FULL_ES2=1
SDL_INC = `sdl2-config --cflags`

# GLES libs and include (ANGLE on mac/win, system on linux)
GLES_LIBS_PATH = $(LIBS_DIR)/libgles/lib-$(OS)
GLES_INC = -I$(LIBS_DIR)/libgles/include
GLES_LIBS = -L$(GLES_LIBS_PATH) -l GLESv2 -l EGL
GLES_DYLIBS = libGLESv2.$(DYL_EXT) libEGL.$(DYL_EXT)
ifeq ($(OS),mac)
	GLES_LINK = -Wl,-rpath,@executable_path/../$(GLES_LIBS_PATH)
define GLES_INSTALL
	for dylib in $(GLES_DYLIBS); \
		do install_name_tool -change ./$$dylib @rpath/$$dylib $@; \
	done;
endef
else ifeq ($(OS),linux)
	GLES_LINK = -Wl,-rpath,$(GLES_LIBS_PATH)
else ifeq ($(OS),win)
define GLES_INSTALL
	for dylib in $(GLES_DYLIBS); \
		do cp $(GLES_LIBS_PATH)/$$dylib $(BIN_DIR)/$$dylib; \
	done;
endef
endif

# Optimizer options
OPT_ZERO = -O0 -g
OPT_TWO = -DNDEBUG -O2
OPT = $(OPT_TWO)
EM_OPT = $(OPT_TWO)

# Debug options
EXTRA_DEBUG = -fsanitize=undefined
EM_EXTRA_DEBUG = $(EXTRA_DEBUG) -s ASSERTIONS=2 -s SAFE_HEAP=1 -s STACK_OVERFLOW_CHECK=2

# Base compilers
CC = cc
EMCC = emcc -s WASM=1 -s PRECISE_F32=1

# One shared code path native/web: the app keeps its own main loop and
# emscripten yields to the browser via ASYNCIFY (same approach as sgi-demos).
EM_ASYNCIFY = -sASYNCIFY -sASYNCIFY_STACK_SIZE=65536

CODE_CC = $(CC) -std=gnu17 -Wall -Wextra -Wno-unused-parameter
CODE_EMCC = $(EMCC) -std=gnu17 -Wall -Wextra -Wno-unused-parameter
