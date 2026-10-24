CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17

TARGET = myshell
OBJECTS = command.o tokenizer.o

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJECTS)

command.o: command.cc command.h tokenizer.h
	$(CXX) $(CXXFLAGS) -c command.cc

tokenizer.o: tokenizer.cc tokenizer.h
	$(CXX) $(CXXFLAGS) -c tokenizer.cc

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean
