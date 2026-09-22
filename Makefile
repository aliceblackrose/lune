.PHONY: all test bench bench-build clean

all:
	$(MAKE) -C bootstrap all

test:
	$(MAKE) -C bootstrap test

bench-build:
	$(MAKE) -C bootstrap bench-build

bench: all
	$(MAKE) -C bootstrap bench
	python3 bench/startup.py

clean:
	$(MAKE) -C bootstrap clean
