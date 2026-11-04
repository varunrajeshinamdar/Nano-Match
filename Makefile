PRESET ?= release

SOURCES = $(shell git ls-files '*.hpp' '*.cpp')

.PHONY: all build test debug asan tsan coverage bench bench-smoke check matrix format format-check tidy clean

all: test

build:
	cmake --preset $(PRESET)
	cmake --build --preset $(PRESET)

test: build
	ctest --preset $(PRESET)

debug:
	$(MAKE) test PRESET=debug

asan:
	$(MAKE) test PRESET=asan

tsan:
	$(MAKE) test PRESET=tsan

coverage:
	$(MAKE) test PRESET=coverage
	lcov --quiet --capture --directory build/coverage --rc branch_coverage=1 \
		--ignore-errors mismatch,inconsistent --include '$(CURDIR)/include/*' --include '$(CURDIR)/src/*' \
		--output-file build/coverage/coverage.info
	genhtml --quiet --branch-coverage --ignore-errors inconsistent --output-directory build/coverage/html \
		build/coverage/coverage.info

# Run scripts/machine_setup.sh first, or the results are marked as not citable. See docs/BENCHMARKS.md.
bench:
	$(MAKE) build PRESET=release
	scripts/run_bench.py

# Checks the benchmark pipeline quickly. Results go to the build directory and are never citable.
bench-smoke:
	$(MAKE) build PRESET=release
	scripts/run_bench.py --smoke --runs 1 --label smoke --cpu none --data-dir build/smoke --out-dir build/smoke

# Everything CI runs, for use before pushing.
check: format-check debug test asan tsan tidy

# Every compiler and preset CI covers. Build directories cache their compiler, so this starts clean.
matrix:
	for compiler in gcc:g++ clang:clang++; do \
		rm -rf build; \
		for preset in debug release asan tsan; do \
			CC=$${compiler%:*} CXX=$${compiler#*:} $(MAKE) test PRESET=$$preset || exit 1; \
		done; \
	done
	rm -rf build

format:
	clang-format -i $(SOURCES)

format-check:
	clang-format --dry-run -Werror $(SOURCES)

tidy:
	cmake --preset debug
	clang-tidy --quiet -p build/debug $(filter %.cpp,$(SOURCES))

clean:
	rm -rf build
