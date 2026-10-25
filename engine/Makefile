CXX ?= g++
CC ?= gcc
CXXFLAGS ?= -O2 -pipe
CFLAGS ?= -O2 -pipe
# Where the models live: /usr/local for the git install, /usr for the package.
PREFIX ?= /usr/local
HARDEN = -fstack-protector-strong -D_FORTIFY_SOURCE=3 -fPIE -Wall -Wextra
LDHARDEN = -pie -Wl,-z,relro,-z,now -Wl,--as-needed
# Arch ships OpenCV 5; derivatives may still have 4 (4.5.4+ has the face API).
OPENCV_PC := $(shell pkg-config --exists opencv5 && echo opencv5 || echo opencv4)
OPENCV_CFLAGS := $(shell pkg-config --cflags $(OPENCV_PC))
OPENCV_LIBS = -lopencv_objdetect -lopencv_dnn -lopencv_imgcodecs -lopencv_imgproc -lopencv_core -lsystemd

all: omaface-engine omaface-pam

omaface-engine: omaface-engine.cpp calibration.h config-settings.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(HARDEN) -std=c++17 -DOMAFACE_MODEL_DIR='"$(PREFIX)/share/omaface/models"' \
		$(OPENCV_CFLAGS) -o $@ $< $(LDFLAGS) $(LDHARDEN) $(OPENCV_LIBS)

omaface-pam: omaface-pam.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(HARDEN) -std=c11 -o $@ $< $(LDFLAGS) $(LDHARDEN)

test:
	python3 -I ../tests/test_calibration_engine.py

clean:
	rm -f omaface-engine omaface-pam

.PHONY: all test clean
