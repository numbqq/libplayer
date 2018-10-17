M_PREFIX=$(LIBPLAYER_STAGING_DIR)/usr

LIB_DIR?=$(M_PREFIX)/lib
HEADERS_DIR?=$(M_PREFIX)/include

TARGET_OBJS=amlv4l.o amvideo.o ion.o IONmem.o

TARGET=libamvdec.so

INSTALL_DIR=$(LIB_DIR)

LDFLAGS += -shared -lpthread -lm -lasound -lrt

TOPDIR=$(shell pwd)
SRC?=$(TOPDIR)

CFLAGS+=-O2 -fPIC -g
CFLAGS+=-I$(SRC) -I$(SRC)/include -I$(SRC)/../amavutils/include -I$(SRC)/../amplayer/player/include -I${HEADERS_DIR} -L${LIB_DIR}
CFLAGS+=-DALSA_OUT

ifdef DOLBY_UDC
    CFLAGS+=-DDOLBY_USE_ARMDEC
endif

export CC CFLAGS

all: $(TARGET)

$(TARGET): $(TARGET_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(TARGET_OBJS:%.o=%.c) -o $@




install:
	-install -m 555 ${TARGET} $(INSTALL_DIR)
	-install -m 555 $(TARGET) $(TARGET_DIR)/usr/lib
	cp  -rf $(SRC)/include/*  $(HEADERS_DIR)/
	cp  -rf $(SRC)/*.h  $(HEADERS_DIR)/

force:

clean:
	-rm -rf *.o ${AMADEC_DIST} ${MP3LIB} $(PCMLIB) $(ADPCMLIB) $(AACLIB) $(A52LIB)



