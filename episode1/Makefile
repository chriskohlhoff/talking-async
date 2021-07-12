CXX=g++
CXXFLAGS=-std=c++20 -Wall -Wextra -fno-inline -I$(ASIO_ROOT)/include -g -DASIO_ENABLE_HANDLER_TRACKING
SOURCE=$(wildcard *.cpp)
PROGRAMS=$(SOURCE:.cpp=)
DSYM=$(SOURCE:.cpp=.dSYM)

all: $(PROGRAMS)

clean:
	rm -rf $(PROGRAMS) $(DSYM)
