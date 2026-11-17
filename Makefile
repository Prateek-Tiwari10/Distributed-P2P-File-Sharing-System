CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -pthread
LDLIBS   = -lcrypto

# TODO(design): DESIGN.md runs ./tracker and ./client, but tracker/ and client/ are the
# source dirs, so binaries go to bin/ (./bin/tracker, ./bin/client).

COMMON_SRC  = $(wildcard common/*.cpp)
COMMON_HDR  = $(wildcard common/*.h)
TRACKER_SRC = $(wildcard tracker/*.cpp)
TRACKER_HDR = $(wildcard tracker/*.h)
CLIENT_SRC  = $(wildcard client/*.cpp)
CLIENT_HDR  = $(wildcard client/*.h)

all: tracker client

tracker: bin/tracker
client: bin/client

bin/tracker: $(TRACKER_SRC) $(TRACKER_HDR) $(COMMON_SRC) $(COMMON_HDR)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $(TRACKER_SRC) $(COMMON_SRC) $(LDLIBS)

bin/client: $(CLIENT_SRC) $(CLIENT_HDR) $(COMMON_SRC) $(COMMON_HDR)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $(CLIENT_SRC) $(COMMON_SRC) $(LDLIBS)

bin/net_test: tests/net_test.cpp $(COMMON_SRC) $(COMMON_HDR)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ tests/net_test.cpp $(COMMON_SRC) $(LDLIBS)

test: bin/net_test
	./bin/net_test

clean:
	rm -rf bin

.PHONY: all tracker client test clean
