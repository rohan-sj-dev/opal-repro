CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O3 -march=native -g -Iinclude -Wall -Wno-interference-size
LDFLAGS  ?= -static

bench: src/bench.cpp include/*.h
	$(CXX) $(CXXFLAGS) src/bench.cpp -o bench $(LDFLAGS)

clean:
	rm -f bench
