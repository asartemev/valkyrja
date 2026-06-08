# Compiler and Flags
CXX      = g++
CXXFLAGS = -O3 -std=c++17 -Wall -Wextra
TARGET   = valkyrja

# Include paths
INCLUDES = -I/usr/local/include/

# Libraries and Linker paths
LDFLAGS  = -L/usr/local/lib
LIBS = -lhwcpipe -ldevice -lpthread -ldl

# Source files
SRC      = main.cpp
OBJ      = $(SRC:.cpp=.o)

# Default rule
all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -o $(TARGET) $(LDFLAGS) $(LIBS)

# Compile source files to object files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Clean rule
clean:
	rm -f $(OBJ) $(TARGET)

# Convenience rule to run with sudo
run: all
	sudo ./$(TARGET) $(args)

.PHONY: all clean run
