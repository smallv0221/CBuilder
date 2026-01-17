CXX = g++
CXXFLAGS = -std=c++17 -Wall -O2
LDFLAGS = -ldl

TARGET = webserver
SOURCES = main.cpp

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SOURCES) $(LDFLAGS)

clean:
	rm -f $(TARGET)
