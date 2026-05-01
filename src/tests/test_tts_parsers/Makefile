# Host-side 单元测试：tts_player.cpp 里的 ReadWavHeader 等纯逻辑函数
# 用法：make && ./test_tts_parsers

CXX      ?= c++
CXXFLAGS := -std=c++11 -Wall -Wextra -Wno-unused-parameter -O2

.PHONY: all clean

all: test_tts_parsers
	./test_tts_parsers

test_tts_parsers: test_tts_parsers.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f test_tts_parsers
