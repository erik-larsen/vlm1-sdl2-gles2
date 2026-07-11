include makefiles/platform.mk

APPNAME = vlm
APP = $(BIN_DIR)/$(APPNAME)
EM_APPNAME = $(WEB_DIR)/$(APPNAME)
EM_APP = $(EM_APPNAME).html

HDRS = $(wildcard $(SRC_DIR)/*.h)
SRC = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BIN_DIR)/%.o,$(SRC))
EM_OBJS = $(patsubst $(SRC_DIR)/%.c,$(WEB_DIR)/%.o,$(SRC))

all: native browser

native: $(APP)

browser: $(EM_APP)

$(BIN_DIR):
	mkdir -p $@
	echo "*" > $@/.gitignore

$(WEB_DIR):
	mkdir -p $@
	echo "*" > $@/.gitignore

$(OBJS): $(BIN_DIR)/%.o: $(SRC_DIR)/%.c $(HDRS) | $(BIN_DIR)
	$(CODE_CC) $(OPT) $(GLES_INC) $(SDL_INC) $< -c -o $@

# GL functions are loaded at runtime via SDL_GL_GetProcAddress, so we
# only need the GLES2 headers, not the ANGLE import libs. SDL dlopens
# ANGLE itself (plat_bootstrap puts libs/libgles/lib-$(OS) on the path).
$(APP): $(OBJS)
	$(CODE_CC) $(OPT) $(OBJS) \
		$(SDL_LIBS) -lm $(CONSOLE_FLAGS) -o $@
	@echo
	@echo BUILT: $@

$(EM_OBJS): $(WEB_DIR)/%.o: $(SRC_DIR)/%.c $(HDRS) | $(WEB_DIR)
	$(CODE_EMCC) $(EM_OPT) $(EM_SDL_LIBS) $< -c -o $@

$(EM_APP): $(EM_OBJS)
	$(CODE_EMCC) $(EM_OPT) $(EM_OBJS) \
		$(EM_SDL_LIBS) $(EM_ASYNCIFY) -lm -o $@
	@echo
	@echo BUILT: $@

.PHONY: all native browser run run-native run-browser clean

run-native: native
	$(APP)

run-browser: browser
	emrun $(EM_APP)

clean:
	rm -f $(APP) $(OBJS)
	rm -rf $(APP).dSYM
	rm -f $(EM_APP) $(EM_OBJS) $(EM_APPNAME).js $(EM_APPNAME).wasm $(EM_APPNAME).data
