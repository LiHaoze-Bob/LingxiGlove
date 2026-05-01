# ============================================================
# Host-side contract test for HandFrame wire layout
# ------------------------------------------------------------
# 只在 host 上验证 HandFrame 的 POD / sizeof / offsetof 契约；
# esp_now_sync.cpp 本身依赖 Arduino / ESP-IDF，无法在 host 编译，
# 因此这里只拉头文件 esp_now_sync.h 做静态契约检查。
# 该头文件不 include <esp_now.h>，纯 C++ 可被 host 消费。
# ============================================================
CXX      ?= g++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -Wpedantic -Werror -O1

TARGET   := run_tests
SRC      := test_esp_now_sync.cpp

.PHONY: all run clean

all: run

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
