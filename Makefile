CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -pthread


SRCS_MANAGER := nfs_manager.cpp config_parser.cpp logger.cpp buffer.cpp
OBJS_MANAGER := $(SRCS_MANAGER:.cpp=.o)

SRCS_CLIENT := nfs_client.cpp logger.cpp
OBJS_CLIENT := $(SRCS_CLIENT:.cpp=.o)

SRCS_CONSOLE := nfs_console.cpp logger.cpp
OBJS_CONSOLE := $(SRCS_CONSOLE:.cpp=.o)

.PHONY: all clean

all: nfs_manager nfs_client nfs_console


nfs_manager: $(OBJS_MANAGER)
	$(CXX) $(CXXFLAGS) -o $@ $^

nfs_client: $(OBJS_CLIENT)
	$(CXX) $(CXXFLAGS) -o $@ $^

nfs_console: $(OBJS_CONSOLE)
	$(CXX) $(CXXFLAGS) -o $@ $^


%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f *.o nfs_manager nfs_client nfs_console
