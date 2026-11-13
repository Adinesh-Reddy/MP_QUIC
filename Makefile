CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -pthread
CPPFLAGS += -Iinclude -Iapps

# ---- The protocol library: every .cpp under src/<module>/ ----
SRCS := $(wildcard src/*/*.cpp)
OBJS := $(patsubst src/%.cpp,build/%.o,$(SRCS))

# ---- Programs (apps/mpq_*.cpp) and the code they share (the other files of apps/) ----
APP_MAINS       := $(wildcard apps/mpq_*.cpp)
APP_COMMON_SRCS := $(filter-out $(APP_MAINS),$(wildcard apps/*.cpp))
APP_COMMON_OBJS := $(patsubst apps/%.cpp,build/apps/%.o,$(APP_COMMON_SRCS))
APPS            := $(patsubst apps/%.cpp,bin/%,$(APP_MAINS))

# ---- Tests ----
UNIT_TESTS        := $(patsubst tests/unit/%.cpp,bin/%,$(wildcard tests/unit/*.cpp))
INTEGRATION_TESTS := $(patsubst tests/integration/%.cpp,bin/%,$(wildcard tests/integration/*.cpp))
INTEGRATION_SCRIPTS := $(wildcard tests/integration/*.sh)

.PHONY: all apps test test-unit test-integration clean

all: $(OBJS) apps

apps: $(APPS)

# Object files go to build/, next to nothing else. -MMD -MP writes the list of headers each
# object depends on, so changing a header rebuilds everything that includes it.
build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

build/apps/%.o: apps/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

bin/mpq_%: build/apps/mpq_%.o $(APP_COMMON_OBJS) $(OBJS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $^

build/tests/%.o: tests/unit/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

build/tests/%.o: tests/integration/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

bin/test_%: build/tests/test_%.o $(APP_COMMON_OBJS) $(OBJS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $^

# Unit tests: one class at a time, no network
test-unit: $(UNIT_TESTS)
	@for t in $(UNIT_TESTS); do echo "./$$t"; ./$$t || exit 1; done

# Integration tests: real UDP sockets on loopback (the .sh tests run the programs of apps/)
test-integration: $(INTEGRATION_TESTS) $(APPS)
	@for t in $(INTEGRATION_TESTS); do echo "./$$t"; ./$$t || exit 1; done
	@for t in $(INTEGRATION_SCRIPTS); do echo "sh $$t"; sh $$t || exit 1; done

test: test-unit test-integration
	@echo "=== ALL TEST PROGRAMS PASSED ==="

clean:
	rm -rf build bin

# Keep the object files of the tests and programs (make would delete them as intermediates)
.SECONDARY:

-include $(shell find build -name '*.d' 2>/dev/null)
