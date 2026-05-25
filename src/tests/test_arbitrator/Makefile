# ============================================================
# Host-side unit test for GestureArbitrator
# ------------------------------------------------------------
# 用法：
#   make           # 编译并运行测试
#   make clean
# ============================================================

CXX      ?= g++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O1

TARGET   := run_tests
SRC      := test_arbitrator.cpp ../../LingxiGlove_Main/gesture_arbitrator.cpp
INC      := -I. -I../../LingxiGlove_Main
PREINCLUDE := -include $(CURDIR)/config.h

.PHONY: all run clean

all: run

$(TARGET): $(SRC) config.h ../../LingxiGlove_Main/gesture_arbitrator.h
	$(CXX) $(CXXFLAGS) $(INC) $(PREINCLUDE) $(SRC) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
