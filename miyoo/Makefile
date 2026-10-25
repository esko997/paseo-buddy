CORE := src/main.c src/ui.c src/pet_view.c src/gfx.c src/feed.c src/net.c src/pet.c src/scene.c src/audio.c src/usage.c
HDRS := $(wildcard src/*.h)
WARN := -Wall -Wextra -Wno-unused-parameter
LIBS := -lm -lpthread
CROSS ?= arm-linux-gnueabihf-
ARM_FLAGS := -march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
# Device glibc (Onion's toolchain sysroot, see Containerfile); the binary must not need newer symbols.
DEVICE_GLIBC := GLIBC_2.28

.PHONY: all desktop host miyoo dist clean test

all: desktop host

# Desktop window for development (SDL2).
desktop: build/pager-desktop
build/pager-desktop: $(CORE) src/plat_sdl.c $(HDRS)
	@mkdir -p build
	$(CC) -O2 -g $(WARN) $(shell sdl2-config --cflags) -o $@ $(CORE) src/plat_sdl.c $(shell sdl2-config --libs) $(LIBS)

# Host build of the Miyoo backend; useful for --shot screenshots and compile checks.
host: build/pager-host
build/pager-host: $(CORE) src/plat_miyoo.c $(HDRS)
	@mkdir -p build
	$(CC) -O2 -g $(WARN) -o $@ $(CORE) src/plat_miyoo.c $(LIBS)

# Collection migration, duplicate-free finds and persistence boundaries.
test: build/collection-test
	./build/collection-test
build/collection-test: tests/collection.c src/pet.c src/pet.h src/feed.h
	@mkdir -p build
	$(CC) -O2 -g $(WARN) -Isrc -o $@ tests/collection.c src/pet.c

# armhf binary for the Miyoo Mini Plus (run via ./build-miyoo.sh if you lack the cross compiler).
# Dynamically linked on purpose: Onion's LD_PRELOAD=libpadsp.so (sound) only hooks dynamic binaries.
miyoo: build/miyoo/pager
build/miyoo/pager: $(CORE) src/plat_miyoo.c $(HDRS)
	@mkdir -p build/miyoo
	$(CROSS)gcc -O2 $(ARM_FLAGS) $(WARN) -o $@ $(CORE) src/plat_miyoo.c $(LIBS)
	$(CROSS)strip $@
	@need=$$($(CROSS)objdump -T $@ | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n1); \
	echo "$@ needs $$need (device: $(DEVICE_GLIBC))"; \
	test "$$(printf '%s\n' $(DEVICE_GLIBC) $$need | sort -V | tail -n1)" = $(DEVICE_GLIBC)

# Onion OS app folder: copy build/PaseoPager to /mnt/SDCARD/App/ on the SD card.
# Rebuilds in place so a pager.cfg (and pet.txt) you put there survives.
dist: build/miyoo/pager
	mkdir -p build/PaseoPager
	cp build/miyoo/pager onion/launch.sh onion/config.json onion/icon.png pager.cfg.example build/PaseoPager/
	chmod +x build/PaseoPager/launch.sh build/PaseoPager/pager

clean:
	rm -rf build
