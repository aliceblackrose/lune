.PHONY: all test clean

all:
	$(MAKE) -C bootstrap all

test:
	$(MAKE) -C bootstrap test

clean:
	$(MAKE) -C bootstrap clean
