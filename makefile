
CC = gcc
CCFLAGS = -Wno-pointer-sign $(shell curl-config --cflags) $(shell xml2-config --cflags)
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

ifeq ($(UNAME_S),Linux)
	PLIBS = $(LIBS) #-ldl
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

