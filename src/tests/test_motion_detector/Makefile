# ============================================================
# Host-side unit test for MotionDetector
# ------------------------------------------------------------
# 用法：
#   make           # 编译并运行测试
#   make run       # 同上
#   make clean
# ------------------------------------------------------------
# -I. 必须在 -I../../LingxiGlove_Main 之前，这样 motion_detector.h
# 里的 #include "config.h" 会优先命中本目录下的 test-only stub。
# ============================================================

CXX      ?= g++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O1

TARGET   := run_tests
SRC      := test_motion_detector.cpp ../../LingxiGlove_Main/motion_detector.cpp
INC      := -I. -I../../LingxiGlove_Main

.PHONY: all run clean

all: run

$(TARGET): $(SRC) config.h ../../LingxiGlove_Main/motion_detector.h
	$(CXX) $(CXXFLAGS) $(INC) $(SRC) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
