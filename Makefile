PRESET ?= release

SOURCES = $(shell git ls-files '*.hpp' '*.cpp')

.PHONY: all build test debug asan tsan check format format-check tidy clean

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

# Everything CI runs, for use before pushing.
check: format-check debug test asan tsan tidy

format:
	clang-format -i $(SOURCES)

format-check:
	clang-format --dry-run -Werror $(SOURCES)

tidy:
	cmake --preset debug
	clang-tidy --quiet -p build/debug $(filter %.cpp,$(SOURCES))

clean:
	rm -rf build
