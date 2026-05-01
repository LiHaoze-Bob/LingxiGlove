# ============================================================
# Host-side unit test for Calibration core algorithm
# ============================================================
# 说明：本测试只验证"采样均值 - 扣重力"与"Flex 均值 + 量程合法性"
# 的数学正确性，不依赖 Arduino/Preferences/Serial。
# 使用：
#   make         # 编译并运行
#   make clean
# ============================================================

CXX      ?= g++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O1

TARGET   := run_tests
SRC      := test_calibration_core.cpp

.PHONY: all run clean

all: run

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
