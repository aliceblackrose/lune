.PHONY: all bootstrap selfhost-images test bench bench-build clean

all:
	$(MAKE) -C bootstrap all

bootstrap:
	$(MAKE) -C bootstrap bootstrap

selfhost-images:
	$(MAKE) -C bootstrap selfhost-images

test:
	$(MAKE) -C bootstrap test

bench-build:
	$(MAKE) -C bootstrap bench-build

bench: all
	$(MAKE) -C bootstrap bench
	python3 bench/startup.py

clean:
	$(MAKE) -C bootstrap clean
