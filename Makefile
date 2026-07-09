CXX = clang++
CXXFLAGS = -std=c++17 -Wall -Wextra -O3 -pthread

TARGETS = stwp_server stwp_client

all: $(TARGETS)

stwp_server: src/server/server.cpp src/common/stwp_msg.hpp
	$(CXX) $(CXXFLAGS) src/server/server.cpp -o stwp_server

stwp_client: src/client/client.cpp src/common/url_parser.hpp src/common/stwp_msg.hpp
	$(CXX) $(CXXFLAGS) src/client/client.cpp -o stwp_client

clean:
	rm -f $(TARGETS)

.PHONY: all clean
