CC = gcc
CFLAGS = -std=c99 -O2 -Wextra -Wall -Werror -Wfloat-equal \
					 -Wshadow -Wpointer-arith -Wcast-align -Wstrict-prototypes \
					 -Wstrict-overflow=5 -Wwrite-strings \
					 -Wcast-qual -Wswitch-default -Wswitch-enum -Wconversion \
					 -Wunreachable-code
LDFLAGS = -lm

TARGET = seagc
SRC = $(wildcard src/*.c)

all: $(TARGET)

$(TARGET): $(SRC)
	@echo "Building SeaGC..."
	@mkdir -p bin
	@$(CC) $(CFLAGS) $(SRC) -o bin/$(TARGET) $(LDFLAGS)
	@echo "Build complete."

.PHONY: run clean

run: $(TARGET)
	./bin/$(TARGET)

clean:
	rm -f bin/$(TARGET)
