# ppengine for the Apollo V4 (SAGA)
# Based on the ApolloCrossDev Makefile Template v1.2 (08-11-2026).
# Differences from the template: sources live in subfolders of src/, objects
# go to build/, there is a 'compile' target, and a host build of the
# headless reference test. The original Linux/OpenSceneGraph build is in
# Makefile.linux.
#
# make compile        build all Amiga targets (no upload): build/ppengine (game),
#                     build/ppengine_headless (reference test)
# make                same as 'make compile'
# make host-headless  build the headless reference test for the host (Linux)
# make check          run the host test and save its output in build/
# make assets         convert models, textures, images, font and sounds into build/data,
#                     and write the Workbench icon build/ppengine.info
# make clean          remove build/
#
# The project folder can be anywhere: set APOLLOCROSSDEV to the
# ApolloCrossDev folder. Without it, the project must be in
# ApolloCrossDev/Projects/.

# Define ApolloCrossDev Folder and Compiler
PWD      := $(shell pwd)
ACD_ROOT ?= $(or $(APOLLOCROSSDEV),$(PWD)/../..)
PREFIX    = $(ACD_ROOT)/Compilers/GCC-6.50-Dev
TARGET    = m68k-amigaos

PROJECT_NAME = ppengine

# Compiler (C only: the C++/OpenSceneGraph layer is not part of the port)
C_COMPILER = $(PREFIX)/bin/m68k-amigaos-gcc
O_LINKER   = $(PREFIX)/bin/m68k-amigaos-gcc
S_COMPILER = $(PREFIX)/bin/vasmm68k_mot

# C runtime: libnix
C_LIB      = -noixemul
C_INCL_CLB = $(PREFIX)/$(TARGET)/libnix/include

# ApolloLib: compiled from source with the project's flags
APOLLOLIB = $(ACD_ROOT)/Projects/ApolloLib

# Libraries: -lm is required (libnix printf/sprintf and the maths functions)
C_LIBS_ALL = -lm

# NDK headers. The target OS is AmigaOS 3.9, so the KB asks for NDK 3.9
# (ndk39-include). This GCC-6.50-Dev install only has NDK 3.2 in
# ndk-include: use it until NDK 3.9 is installed, and only call functions
# that AmigaOS 3.9 has.
C_INCL_SYS = $(PREFIX)/$(TARGET)/sys-include
C_INCL_NDK = $(firstword $(wildcard $(PREFIX)/$(TARGET)/ndk39-include) $(PREFIX)/$(TARGET)/ndk-include)
MAGGIE_INCLUDE = third_party/MaggieLibrary/include
C_INCL_ALL = -Isrc -Isrc/ppengine -isystem $(MAGGIE_INCLUDE) -I$(C_INCL_CLB) -I$(C_INCL_SYS) -I$(C_INCL_NDK) $(if $(APOLLOLIB),-I$(APOLLOLIB))

# C options (KB toolchain/c-compiler-options.md); -m68881 is required for FPU code
C_OPTIONS  = -O2 -m68080 -m68881 -ffast-math -fomit-frame-pointer
C_WARNINGS = -Wall
C_DEBUG    = -g
C_FLAGS    = $(C_LIB) $(C_OPTIONS) $(C_WARNINGS) $(C_DEBUG) $(C_INCL_ALL) -MMD -MP

# Assembler options
S_INCLUDE = $(PREFIX)/$(TARGET)/DevPac/Include
S_FLAGS   = -Fhunk -m68080 -m68881 -opt-speed -linedebug -kick1hunks -I$(S_INCLUDE)

# Folders
BUILD_DIR = build
OBJ_DIR   = $(BUILD_DIR)/amiga
HOST_DIR  = $(BUILD_DIR)/host
LIB_DIR   = $(BUILD_DIR)/apollolib

# Sources
ENGINE_C_FILES   = $(shell find src/ppengine -name '*.c' | sort)
GEOMETRY_C_FILES = src/geometry/vertex2d.c $(wildcard src/track_geometry/*.c)
CORE_C_FILES     = $(ENGINE_C_FILES) $(GEOMETRY_C_FILES)
HEADLESS_C_FILES = src/platform/headless/headless_main.c
GAME_C_FILES     = $(sort $(wildcard src/platform/amiga/*.c)) src/scene/scene.c $(sort $(wildcard src/scene/maggie/*.c)) \
                   src/audio/audio.c $(sort $(wildcard src/audio/arne/*.c))

LIB_C_FILES = $(if $(APOLLOLIB),$(wildcard $(APOLLOLIB)/*.c))
LIB_S_FILES = $(if $(APOLLOLIB),$(wildcard $(APOLLOLIB)/*.s))
LIB_O_FILES = $(patsubst $(APOLLOLIB)/%.c,$(LIB_DIR)/%.o,$(LIB_C_FILES)) \
              $(patsubst $(APOLLOLIB)/%.s,$(LIB_DIR)/%.o,$(LIB_S_FILES))

CORE_O_FILES     = $(patsubst %.c,$(OBJ_DIR)/%.o,$(CORE_C_FILES))
HEADLESS_O_FILES = $(patsubst %.c,$(OBJ_DIR)/%.o,$(HEADLESS_C_FILES))
GAME_S_FILES     = $(sort $(wildcard src/platform/amiga/*.s))
GAME_O_FILES     = $(patsubst %.c,$(OBJ_DIR)/%.o,$(GAME_C_FILES)) $(patsubst %.s,$(OBJ_DIR)/%.o,$(GAME_S_FILES))

# Targets
GAME_EXE          = $(BUILD_DIR)/$(PROJECT_NAME)
HEADLESS_EXE      = $(BUILD_DIR)/$(PROJECT_NAME)_headless
HOST_HEADLESS_EXE = $(HOST_DIR)/$(PROJECT_NAME)_headless

.PHONY: all compile apollolib host-headless check assets clean

all: compile

compile: $(GAME_EXE) $(HEADLESS_EXE)
	@echo "ppengine: Amiga build finished"

# The game links ApolloLib; the headless test does not need it.
apollolib: $(LIB_O_FILES)

$(GAME_EXE): $(CORE_O_FILES) $(GAME_O_FILES) $(LIB_O_FILES)
	@mkdir -p $(dir $@)
	$(O_LINKER) $^ $(C_LIB) $(C_OPTIONS) $(C_DEBUG) -lamiga $(C_LIBS_ALL) -o $@

$(HEADLESS_EXE): $(CORE_O_FILES) $(HEADLESS_O_FILES)
	@mkdir -p $(dir $@)
	$(O_LINKER) $^ $(C_LIB) $(C_OPTIONS) $(C_DEBUG) $(C_LIBS_ALL) -o $@

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(C_COMPILER) -c $(C_FLAGS) -o $@ $<

$(OBJ_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	@$(S_COMPILER) $(S_FLAGS) -quiet -o $@ $<

$(LIB_DIR)/%.o: $(APOLLOLIB)/%.c
	@mkdir -p $(dir $@)
	@$(C_COMPILER) -c $(C_FLAGS) -o $@ $<

$(LIB_DIR)/%.o: $(APOLLOLIB)/%.s
	@mkdir -p $(dir $@)
	@$(S_COMPILER) $(S_FLAGS) -o $@ $< >/dev/null

# Host build of the headless reference test (T05)
HOST_CC     = gcc
HOST_CFLAGS = -O2 -Wall -Isrc -Isrc/ppengine

host-headless: $(HOST_HEADLESS_EXE)

$(HOST_HEADLESS_EXE): $(CORE_C_FILES) $(HEADLESS_C_FILES)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $^ -lm -o $@

# Game data for the V4 (models, textures, images, font, sounds): needs
# python3 and ffmpeg. See tools/convert_assets.py for the file formats.
assets:
	python3 tools/convert_assets.py --out $(BUILD_DIR)/data
	python3 tools/make_icon.py --out $(BUILD_DIR)/ppengine.info

check: $(HOST_HEADLESS_EXE)
	$(HOST_HEADLESS_EXE) > $(BUILD_DIR)/headless_host.txt
	@tail -1 $(BUILD_DIR)/headless_host.txt

clean:
	rm -rf $(BUILD_DIR)

# Header dependencies written by -MMD
-include $(CORE_O_FILES:.o=.d) $(HEADLESS_O_FILES:.o=.d) $(GAME_O_FILES:.o=.d)
