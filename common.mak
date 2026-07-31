# TODO include this only once and export variables

CFLAGS ?= $(OPTIMIZATIONS) -Wall -std=c++17
override CFLAGS += -I../libs/MTS-ESP/Client -I../libs/lv2/include -I../libs/robtk/

IS_OSX=
UNAME=$(shell uname)
ifeq ($(UNAME),Darwin)
  IS_OSX=yes
  HOMEBREW=$$(brew --prefix)
  override CFLAGS += -I$(HOMEBREW)/include -DGL_SILENCE_DEPRECATION
endif

PREFIX ?= /usr/local
ifneq ($(IS_OSX),)
  OPTIMIZATIONS ?= -ffast-math -fomit-frame-pointer -O3 -fno-finite-math-only
else
  OPTIMIZATIONS ?= -msse -msse2 -mfpmath=sse -ffast-math -fomit-frame-pointer -O3 -fno-finite-math-only
endif
ENABLE_CONVOLUTION ?= no
INSTALL_EXTRA_LV2 ?= no
FONTFILE?=/usr/share/fonts/truetype/ttf-bitstream-vera/VeraBd.ttf
VERSION?=$(shell git describe --tags HEAD 2>/dev/null | sed 's/-g.*$$//;s/^v//' || true)
OSXCOMPAT?=-mmacosx-version-min=10.15
ifeq ($(VERSION),)
  VERSION=$(EXPORTED_VERSION)
endif
ifeq ($(VERSION),)
  $(warning "Cannot determine version information.")
  $(warning "Use the top-level makefile (or full git clone).")
  $(error "-^-")
endif

LV2VERSION=$(VERSION)

bindir = $(PREFIX)/bin
sharedir = $(PREFIX)/share/setBfree
lv2dir = ../libs/lv2/lv2

# XWIN flag was being used to cross-compile for Windows
# Now it's being used to compile directly on Windows with MSYS2
ifeq ($(OS),Windows_NT)
  XWIN=yes
endif

ifeq ($(XWIN),)
override CFLAGS += -fPIC
endif
override CFLAGS += -DVERSION="\"$(VERSION)\""

STRIP      ?= strip
PKG_CONFIG ?= pkg-config

CXXFLAGS ?= $(OPTIMIZATIONS) -Wall
GLUICFLAGS=-I. -I.. -Wno-unused-function
STRIPFLAGS=-s

# check for LV2
LV2AVAIL=yes

GLUICFLAGS+=-DHAVE_IDLE_IFACE
LV2UIREQ=lv2:requiredFeature ui:idleInterface; lv2:extensionData ui:idleInterface;
HAVE_IDLE=yes

# check for lv2_atom_forge_object  new in 1.8.1 deprecates lv2_atom_forge_blank
override CFLAGS += -DHAVE_LV2_1_8

override CFLAGS += -DHAVE_LV2_1_18_6

ifeq ($(origin CC),default)
CC=g++
endif

ifneq ($(SANITIZE),)
CC=clang++
CXX=clang++
override CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=address,undefined
endif

PUGL_DIR=../libs/robtk/pugl/

IS_WIN=
PKG_GL_LIBS=
ifneq ($(IS_OSX),)
  LV2LDFLAGS=-dynamiclib
  LIB_EXT=.dylib
  GLUILIBS=-framework Cocoa -framework OpenGL -framework CoreFoundation
  STRIPFLAGS=-u -r -arch all -s $(RW)lv2syms
  UI_TYPE=CocoaUI
  PUGL_SRC=$(PUGL_DIR)/pugl_osx.mm
  EXTENDED_RE=-E
else
  ifneq ($(XWIN),)
    IS_WIN=yes
    LV2LDFLAGS=-Wl,-Bstatic -Wl,-Bdynamic -Wl,--as-needed -lpthread
    LIB_EXT=.dll
    EXE_EXT=.exe
    GLUILIBS=-lws2_32 -lwinmm -lopengl32 -lglu32 -lgdi32 -lcomdlg32 -lpthread
    UI_TYPE=WindowsUI
    PUGL_SRC=$(PUGL_DIR)/pugl_win.cpp
    override CFLAGS+= -DHAVE_MEMSTREAM
    override LDFLAGS += -static-libgcc -static-libstdc++ -DPTW32_STATIC_LIB
    override LDFLAGS += -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive -Wl,-Bdynamic
  else
    override CFLAGS+= -DHAVE_MEMSTREAM
    LV2LDFLAGS=-Wl,-Bstatic -Wl,-Bdynamic -Wl,--as-needed
    LIB_EXT=.so
    PKG_GL_LIBS=glu gl
    GLUICFLAGS+=-pthread
    GLUILIBS=-lX11
    UI_TYPE=X11UI
    PUGL_SRC=$(PUGL_DIR)/pugl_x11.c
  endif
  EXTENDED_RE=-r
endif

GLUICFLAGS+=`pkg-config --cflags cairo pango $(PKG_GL_LIBS)`
GLUILIBS+=`pkg-config $(PKG_UI_FLAGS) --libs cairo pango pangocairo $(PKG_GL_LIBS)`

ifneq ($(XWIN),)
GLUILIBS+=-lpthread -lusp10
endif

GLUICFLAGS+=$(LIC_CFLAGS)
GLUILIBS+=$(LIC_LOADLIBES)

ifeq ($(ENABLE_CONVOLUTION), yes)
  CC=$(CXX)
endif

ifneq ($(MOD),)
	MODBRAND=mod:brand \"x42\";
	MODLABEL=mod:label \"$(MODNAME)\";
	MODGUITTL=, <modgui.ttl>
else
	MODBRAND=
	MODLABEL=
	MODGUITTL=
endif

#LV2 / GL-GUI

ifeq ($(FONTFILE),verabd.h)
  FONT_FOUND=yes
else
  ifeq ($(shell test -f $(FONTFILE) || echo no ), no)
    FONT_FOUND=no
  else
    FONT_FOUND=yes
  endif
endif

ifeq ($(MOD),)
  HAVE_UI=$(shell pkg-config --exists $(PKG_GL_LIBS) ftgl && echo $(FONT_FOUND))
else
  HAVE_UI=no
endif

ifeq ($(LV2AVAIL)$(HAVE_UI)$(HAVE_IDLE), yesyesyes)
  UICFLAGS=-I..
  UIDEPS=$(PUGL_DIR)/pugl.h $(PUGL_DIR)/pugl_internal.h ../b_synth/ui_model.h
  UIDEPS+=$(TX)drawbar.cpp $(TX)wood.cpp $(TX)dial.cpp
  UIDEPS+=$(TX)btn_vibl.cpp $(TX)btn_vibu.cpp $(TX)btn_overdrive.cpp $(TX)btn_perc_volume.cpp
  UIDEPS+=$(TX)btn_perc.cpp $(TX)btn_perc_decay.cpp $(TX)btn_perc_harmonic.cpp
  UIDEPS+=$(TX)bg_right_ctrl.cpp $(TX)bg_left_ctrl.cpp $(TX)bg_leslie_drum.cpp $(TX)bg_leslie_horn.cpp
  UIDEPS+=$(TX)help_screen_image.cpp
  UIDEPS+=$(TX)ui_button_image.cpp $(TX)ui_proc_image.cpp
  UIDEPS+=$(TX)uim_background.cpp $(TX)uim_cable1.cpp $(TX)uim_cable2.cpp $(TX)uim_caps.cpp
  UIDEPS+=$(TX)uim_tube1.cpp $(TX)uim_tube2.cpp
  ifeq ($(IS_OSX), yes)
    STATICLIBS?=$(HOMEBREW)/lib/
    UIDEPS+=$(PUGL_DIR)/pugl_osx.mm
    UILIBS=$(PUGL_DIR)/pugl_osx.mm -framework Cocoa -framework OpenGL
    UILIBS+=$(STATICLIBS)/libftgl.a $(STATICLIBS)/libfreetype.a
    # libbz2.a is found in homebrew/Cellar
    UILIBS+=`find $$(dirname $(STATICLIBS)) -name libbz2.a`
    UILIBS+=$(STATICLIBS)/libpng.a
    UILIBS+=`pkg-config --libs zlib`
    UILIBS+=-lm $(OSXCOMPAT)
  else
    ifeq ($(IS_WIN), yes)
      UIDEPS+=$(PUGL_DIR)/pugl_win.cpp
      UILIBS=$(PUGL_DIR)/pugl_win.cpp
      STATICLIBS?=/ucrt64/lib/
      UILIBS+=$(STATICLIBS)/libftgl.a $(STATICLIBS)/libfreetype.a
      # The libs on the two lines below are used by freetype
      UILIBS+=$(STATICLIBS)/libpng.a $(STATICLIBS)/libbrotlidec.a $(STATICLIBS)/libbrotlicommon.a $(STATICLIBS)/libharfbuzz.a
      UILIBS+=$(STATICLIBS)/libbz2.a $(STATICLIBS)/libgraphite2.a $(STATICLIBS)/librpcrt4.a $(STATICLIBS)/libz.a
      UILIBS+=-lws2_32 -lwinmm -lopengl32 -lglu32 -lgdi32 -lcomdlg32 -lpthread -ldwrite
    else
      UIDEPS+=$(PUGL_DIR)/pugl_x11.c
      override CFLAGS+=`pkg-config --cflags glu`
      UILIBS=$(PUGL_DIR)/pugl_x11.c -lX11
      ifeq ($(STATICBUILD), yes)
        UILIBS+=`pkg-config --libs glu`
        UILIBS+=`pkg-config --variable=libdir ftgl`/libftgl.a `pkg-config --variable=libdir ftgl`/libfreetype.a
        UILIBS+=`pkg-config --libs zlib`
      else
        UILIBS+=`pkg-config --libs glu gl ftgl`
      endif
      UICFLAGS+=-DFONTFILE=\"$(FONTFILE)\"
    endif
  endif
  UICFLAGS+=`pkg-config --cflags freetype2` `pkg-config --cflags ftgl` -DHAVE_FTGL -DUINQHACK=Sbf
endif

#NOTE: midi.cpp and cfgParser.cpp needs to be re-compiled w/o HAVE_ASEQ
# and HAVE_ZITACONVOLVE. Other objects are identical.
LV2OBJ= \
  ../src/midi.cpp \
  ../src/cfgParser.cpp \
  ../src/program.cpp \
  ../src/vibrato.cpp \
  ../src/state.cpp \
  ../src/tonegen.cpp \
  ../libs/MTS-ESP/Client/libMTSClient.cpp \
  ../src/tuning.cpp \
  ../src/pgmParser.cpp \
  ../src/memstream.cpp \
  ../src/midnam.cpp \
  ../src/overdrive.cpp \
  ../src/reverb.cpp \
  ../src/eqcomp.cpp \
  ../src/whirl.cpp \

override CXXFLAGS=$(CFLAGS)
