CXX ?= g++
CC ?= gcc
CXXFLAGS ?= -O2 -pipe
CFLAGS ?= -O2 -pipe
HARDEN = -fstack-protector-strong -D_FORTIFY_SOURCE=3 -fPIE -Wall -Wextra
LDHARDEN = -pie -Wl,-z,relro,-z,now -Wl,--as-needed
# Arch ships OpenCV 5; derivatives may still have 4 (4.5.4+ has the face API).
OPENCV_PC := $(shell pkg-config --exists opencv5 && echo opencv5 || echo opencv4)
OPENCV_CFLAGS := $(shell pkg-config --cflags $(OPENCV_PC))
OPENCV_LIBS = -lopencv_objdetect -lopencv_dnn -lopencv_imgproc -lopencv_core -lsystemd

all: faceid-engine faceid-pam

faceid-engine: faceid-engine.cpp
	$(CXX) $(CXXFLAGS) $(HARDEN) -std=c++17 $(OPENCV_CFLAGS) -o $@ $< $(LDHARDEN) $(OPENCV_LIBS)

faceid-pam: faceid-pam.c
	$(CC) $(CFLAGS) $(HARDEN) -std=c11 -o $@ $< $(LDHARDEN)

clean:
	rm -f faceid-engine faceid-pam

.PHONY: all clean
