# CPPFLAGS is deliberately not set here so a value given on the command line
# (e.g. make CPPFLAGS="-DIPC_PORT_MIN=20100 -DIPC_PORT_MAX=20199") is used.
CXX      ?= g++
CXXSTD    = -std=c++17
CXXFLAGS ?= -Wall -Wextra -O2

TARGETS = nplab_client nplab_server_single nplab_server_multi

all: $(TARGETS)

nplab_client: nplab_client.cpp
	$(CXX) $(CPPFLAGS) $(CXXSTD) $(CXXFLAGS) -o $@ $<

nplab_server_single: nplab_server_single.cpp nplab_common.h
	$(CXX) $(CPPFLAGS) $(CXXSTD) $(CXXFLAGS) -o $@ $<

nplab_server_multi: nplab_server_multi.cpp nplab_common.h
	$(CXX) $(CPPFLAGS) $(CXXSTD) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS) *.o

.PHONY: all clean
