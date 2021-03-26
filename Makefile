CC=g++
CFLAGS=-I -Wall
INTF_SRC=intfMonitor.cpp
NET_SRC=networkMonitor.cpp

intfMonitor: $(INTF_SRC)
	$(CC) $(CFLAGS) $(INTF_SRC) -o intfMonitor

networkMonitor: $(NET_SRC)
	$(CC) $(CFLAGS) $(NET_SRC) -o networkMonitor

clean:
	rm -rf *.o intfMonitor networkMonitor

all: clean intfMonitor networkMonitor
