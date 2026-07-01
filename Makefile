CC = gcc
CFLAGS = -std=c99 -O2 -Wextra -Wall -Werror -Wfloat-equal \
					 -Wshadow -Wpointer-arith -Wcast-align -Wstrict-prototypes \
					 -Wstrict-overflow=5 -Wwrite-strings \
					 -Wcast-qual -Wswitch-default -Wswitch-enum -Wconversion \
					 -Wunreachable-code
LDFLAGS = -lm

TARGET = seagc
BENCH_TARGET = seagc_bench
CORE_SRC = $(filter-out src/main.c src/bench.c,$(wildcard src/*.c))
SMOKE_SRC = $(CORE_SRC) src/main.c
BENCH_SRC = $(CORE_SRC) src/bench.c

all: $(TARGET)

$(TARGET): bin/$(TARGET)

bin/$(TARGET): $(SMOKE_SRC)
	@echo "Building SeaGC..."
	@mkdir -p bin
	@$(CC) $(CFLAGS) $(SMOKE_SRC) -o bin/$(TARGET) $(LDFLAGS)
	@echo "Build complete."

bench: $(BENCH_TARGET)

$(BENCH_TARGET): bin/$(BENCH_TARGET)

bin/$(BENCH_TARGET): $(BENCH_SRC)
	@echo "Building SeaGC benchmarker..."
	@mkdir -p bin
	@$(CC) $(CFLAGS) $(BENCH_SRC) -o bin/$(BENCH_TARGET) $(LDFLAGS)
	@echo "Benchmark build complete."

.PHONY: run run-bench bench-smoke clean

run: $(TARGET)
	./bin/$(TARGET)

run-bench: bench
	./bin/$(BENCH_TARGET)

bench-smoke: bench
	./bin/$(BENCH_TARGET) --quick --csv

clean:
	rm -f bin/$(TARGET) bin/$(BENCH_TARGET)
