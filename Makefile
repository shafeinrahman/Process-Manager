CC = gcc
CFLAGS = -Wall -Wextra -pthread -std=c11 -O2
LDFLAGS = -pthread

TARGET = pm_sim
SOURCES = main.c pm.c
HEADERS = pm.h
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)
	@echo "Build successful: $(TARGET)"

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET) snapshots.txt

run: $(TARGET)
	./$(TARGET) commands1.txt

help:
	@echo "Available targets:"
	@echo "  make all       - Build the process manager"
	@echo "  make clean     - Clean build artifacts"
	@echo "  make run       - Build and run with sample commands"
	@echo "  make help      - Show this help message"
