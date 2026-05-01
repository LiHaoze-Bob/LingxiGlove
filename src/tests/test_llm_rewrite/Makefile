# Host-side 单元测试：llm_client.cpp 里 TrimRewriteNoise 的引号/空白去噪逻辑
# 用法：make && ./test_llm_rewrite

CXX      ?= c++
CXXFLAGS := -std=c++11 -Wall -Wextra -Wno-unused-parameter -O2

.PHONY: all clean

all: test_llm_rewrite
	./test_llm_rewrite

test_llm_rewrite: test_llm_rewrite.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f test_llm_rewrite
