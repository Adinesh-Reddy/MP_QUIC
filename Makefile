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

.PHONY: all apps clean

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

clean:
	rm -rf build bin

# Keep the object files of the programs (make would delete them as intermediates)
.SECONDARY:

-include $(shell find build -name '*.d' 2>/dev/null)
