CXX ?= g++
CXXFLAGS ?= -O2 -pipe
OPENCV_PC := $(shell pkg-config --exists opencv5 && echo opencv5 || echo opencv4)
OPENCV_CFLAGS := $(shell pkg-config --cflags $(OPENCV_PC))
OPENCV_LIBS = -lopencv_objdetect -lopencv_dnn -lopencv_imgproc -lopencv_core

SOURCES = $(wildcard src/*.cpp)

test: tests/test_core
	./tests/test_core

tests/test_core: tests/test_core.cpp $(SOURCES) include/omaface/core.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Wall -Wextra -std=c++17 -Iinclude $(OPENCV_CFLAGS) -o $@ tests/test_core.cpp $(SOURCES) $(LDFLAGS) $(OPENCV_LIBS)

clean:
	rm -f tests/test_core

.PHONY: test clean
