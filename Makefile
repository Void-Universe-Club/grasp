# grasp: lightweight LLM-based agent framework (C++11 + RAII)
CXX      := g++
CXXFLAGS := -std=c++11 -Wall -Wextra -O2 -pthread -MMD -MP -Isrc -Ithird_party
SRCS     := src/main.cpp src/cli.cpp src/model.cpp src/store.cpp src/os.cpp \
            src/session.cpp src/llm.cpp src/driver.cpp src/repl.cpp \
            src/svg.cpp
OBJS     := $(SRCS:.cpp=.o)
DEPS     := $(OBJS:.o=.d)
TARGET   := grasp

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include $(DEPS)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

test: $(TARGET)
	bash tests/run_tests.sh

.PHONY: all clean test
