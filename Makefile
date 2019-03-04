PREFIX= $(HOME)

# add -D RHO_TRACE for trace logging
CFLAGS = -Wall -Werror -I. -I$(PREFIX)/include
LDFLAGS= -L. -L$(PREFIX)/lib  -lxutp -lutp -lrho -lcrypto -lssl -lpthread
LDLIBS=  -lrho -lutp -lcrypto -lssl -lpthread -lrt

all: libxutp.a server client

libxutp.a: xutp.o
	ar rcu $@ $^;

install: libxutp.a
	mkdir -p $(PREFIX)/lib
	cp libxutp.a $(PREFIX)/lib
	mkdir -p $(PREFIX)/include
	cp xutp.h $(PREFIX)/include

xutp.o: xutp.c xutp.h
	$(CC) $(CFLAGS) -c -o $@ $<

server: server.c libxutp.a
	$(CXX) -o server $(CFLAGS) $(CXXFLAGS) server.c $(LDFLAGS) $(LDLIBS)

client: client.c libxutp.a
	$(CXX) -o client $(CFLAGS) $(CXXFLAGS) client.c $(LDFLAGS) $(LDLIBS)

clean:
	rm -f xutp.o libxutp.a server client

.PHONY: all clean install
