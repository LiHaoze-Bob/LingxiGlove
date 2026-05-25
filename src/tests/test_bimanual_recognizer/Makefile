# ============================================================
# Host-side unit test for BimanualRuleRecognizer
# ------------------------------------------------------------
# 用法：
#   make           # 编译并运行测试
#   make clean
# ------------------------------------------------------------
# 难点：`gesture_recognizer.cpp` 通过 `#include "config.h"` 走相对路径查找，
# 会优先命中 `LingxiGlove_Main/config.h`（含 Arduino-only 的 DEBUG_LOG 宏），
# 即使 `-I.` 在最前面也无法拦截。
# 解决：用 `-include` 让编译器在每个源文件最顶部预先 include 本目录的
# stub config.h / sensor_manager.h（它们使用与真实文件相同的 guard 名
# CONFIG_H / SENSOR_MANAGER_H），后续真实头被链式 include 时由于 guard
# 已定义而整个文件跳过，stub 接管。
# ============================================================

CXX      ?= g++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O1

TARGET   := run_tests
SRC      := test_bimanual_recognizer.cpp ../../LingxiGlove_Main/gesture_recognizer.cpp
INC      := -I. -I../../LingxiGlove_Main
PREINCLUDE := -include $(CURDIR)/config.h -include $(CURDIR)/sensor_manager.h

.PHONY: all run clean

all: run

$(TARGET): $(SRC) Arduino.h sensor_manager.h config.h ../../LingxiGlove_Main/gesture_recognizer.h
	$(CXX) $(CXXFLAGS) $(INC) $(PREINCLUDE) $(SRC) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
