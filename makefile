
CC = gcc
CCFLAGS = -std=c99 -Wno-pointer-sign -D_BSD_SOURCE -D_XOPEN_SOURCE -D_XOPEN_SOURCE_EXTENDED $(shell curl-config --cflags) $(shell xml2-config --cflags)
DBGFLAGS = -g -DDEBUG=1

HEADERS = 
SRCS = metar.c
LIBS = $(shell curl-config --libs) $(shell xml2-config --libs)

EXE = metar

##################################################
## conditionals
ifdef DEBUG
	FLAGS = $(CCFLAGS) $(DBGFLAGS)
else
	FLAGS = $(CCFLAGS) -DMETAR_NO_THROTTLE
endif

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
	PLIBS = $(LIBS) -lm #-ldl
else
	PLIBS = $(LIBS)
endif

##################################################
## default rules
OBJS = $(SRCS:.c=.o)

$(EXE): $(OBJS)
	$(CC) $(FLAGS) -o $(EXE) $(OBJS) $(PLIBS)

%.o: %.c $(HEADERS)
	$(CC) $(FLAGS) -c -o $@ $<

clean:
	rm -f $(EXE) $(OBJS)

install:
	cp $(EXE) /usr/local/bin/$(EXE)

.PHONY: clean install
.PRECIOUS: %.o

