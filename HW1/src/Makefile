CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2

all: nplab_server nplab_client

nplab_server: nplab_server.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

nplab_client: nplab_client.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f nplab_server nplab_client

.PHONY: all clean
