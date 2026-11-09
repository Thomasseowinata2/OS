# tsim - a time-sharing system simulated with POSIX signals and timers
# COMP6697001 Operating Systems, AOL Case Study
#
#   make              build ./tsim
#   make run          build and run the built-in workload
#   make demo         the scripted demonstration used in the video
#   make compare      the same workload under all four policies
#   make test         the regression tests
#   make edge         the awkward inputs
#   make stress       hunt for lost signals under timer pressure
#   make check        all three test suites
#   make clean

CC      ?= gcc

# -D_GNU_SOURCE is needed for sigqueue(3), wait4(2) and SIGRTMIN on glibc.
# -lrt is needed for timer_create(2) on glibc older than 2.34, which is what
# Ubuntu 20.04 ships; on newer releases it is a harmless empty stub.
CSTD    := -std=c11
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wold-style-definition
CFLAGS  ?= -O2 -g
CFLAGS  += $(CSTD) $(WARN) -D_GNU_SOURCE -Iinclude
LDLIBS  += -lrt

BIN     := tsim
BUILD   := build
SRC     := $(sort $(wildcard src/*.c))
OBJ     := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))
DEP     := $(OBJ:.o=.d)

.PHONY: all run demo compare test edge stress check clean help

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
	@echo "built ./$(BIN)"

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD):
	@mkdir -p $(BUILD)

-include $(DEP)

run: $(BIN)
	./$(BIN)

demo: $(BIN)
	@./scripts/demo.sh

compare: $(BIN)
	@./scripts/compare.sh

test: $(BIN)
	@./tests/run_tests.sh

edge: $(BIN)
	@./tests/edge_cases.sh

stress: $(BIN)
	@./tests/stress.sh 8

check: test edge stress

clean:
	rm -rf $(BUILD) $(BIN) *.csv

help:
	@./$(BIN) --help 2>/dev/null || echo "run make first"
