# btc-miner-psp — Sony PSP Bitcoin pool miner via pspdev / psp-gcc.
#
# Build with:
#   make PSPDEV=/path/to/pspdev
# or set PSPDEV in env (build.bat does that via WSL).
#
# Output: EBOOT.PBP — a bootable PSP package. Drop it in
# /PSP/GAME/btc-miner-psp/ on a memstick, or load directly in
# PPSSPP / JPCSP.

TARGET = btc-miner-psp

OBJS = build/main.o build/sha256.o build/stratum.o

# pspsdk's build.mak handles -I for $(INCDIR), $(PSPSDK)/include,
# and $(PSPDEV)/psp/include automatically — but only via $(INCDIR).
# Don't append more -I to CFLAGS or build.mak munges them at link.
INCDIR  = include
CFLAGS  = -O2 -G0 -Wall

LIBS = -lpspnet_apctl -lpspnet_resolver -lpspnet_inet -lpspnet -lpsputility

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = BTC Miner PSP
PSP_EBOOT_ICON  = NULL
PSP_EBOOT_PIC1  = NULL

PSPSDK = $(shell psp-config --pspsdk-path)

# Custom rule for out-of-tree objects in build/.  Note: no
# order-only dep on a `build:` target — that conflicts with the
# directory name, so we mkdir inline in the recipe instead.
build/%.o: source/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Iinclude -I$(PSPSDK)/include -I$(PSPDEV)/psp/include -c -o $@ $<

include $(PSPSDK)/lib/build.mak
