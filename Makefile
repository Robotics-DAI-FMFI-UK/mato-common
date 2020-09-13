CC=gcc
CPP=g++
SAN = -fsanitize=undefined               \
      -fsanitize=address                 \
      -fsanitize=leak                    \
      -fsanitize=shift                   \
      -fsanitize=integer-divide-by-zero  \
      -fsanitize=unreachable             \
      -fsanitize=vla-bound               \
      -fsanitize=null
MATO_BASIC=mato/mato.c \
           mato/mato_core.c \
           mato/mato_net.c \
           mato/mato_logs.c \
           mato/mato_config.c \
           core/config_mato.c \
           bites/bites.c 
TEST_MATO_BASE_SRCS=new-tests/test_mato_base.c \
               modules/live/mato_base_module.c \
               ${MATO_BASIC}

TEST_MATO_BASE_OBJS=${TEST_MATO_BASE_SRCS:.c=.o}

TEST_CPPSRCS=
TEST_CPPOBJS=${TEST_CPPSRCS:.cpp=.o}

#OPTIMIZE=-O0 ${SAN}
OPTIMIZE=-O0
DEBUG_FLAGS=-g
CFLAGS=${OPTIMIZE} ${DEBUG_FLAGS} -std=c11 -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600 -I. -I/usr/include/cairo -I/usr/local/rplidar/sdk/sdk/include -I/usr/include/librsvg-2.0/librsvg `pkg-config --cflags gio-2.0` -I/usr/include/libxml2 -I/usr/include/gdk-pixbuf-2.0 -Wall
LDFLAGS=${DEBUG_FLAGS} -pthread -lrt -lcairo -lX11 -lm -lncurses -L/usr/local/rplidar/sdk/output/Linux/Release -lrsvg-2 -lxml2 -lpng -lstdc++ `pkg-config --libs gio-2.0`
PREFIX=/usr/local
MATO_CORE=$(shell pwd)
export MATO_CORE

all: test

install:

test:	test_mato_base

test_mato_base: ${TEST_MATO_BASE_OBJS}
	${CC} -o test_mato_base $^ ${LDFLAGS} ${DEBUG_FLAGS}

uninstall:

clean:
	rm -f *.o */*.o */*/*.o test_mato_base
