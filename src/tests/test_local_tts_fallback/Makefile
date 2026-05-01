# ============================================================
# Host-side unit test for local_tts_fallback lookup logic
# ============================================================
CXX      ?= g++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O1

TARGET   := run_tests
SRC      := test_local_tts_fallback.cpp

.PHONY: all run clean

all: run

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
