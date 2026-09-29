PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib/lune

.PHONY: all bootstrap selfhost-images test test-web bench bench-build install uninstall clean

all:
	$(MAKE) -C bootstrap all

bootstrap:
	$(MAKE) -C bootstrap bootstrap

selfhost-images:
	$(MAKE) -C bootstrap selfhost-images

test:
	$(MAKE) -C bootstrap test
	$(MAKE) test-web

test-web: all
	./bootstrap/build/lune tests/web/web_helpers.lune
	python3 tests/web/test_dev_server.py ./bootstrap/build/lune

bench-build:
	$(MAKE) -C bootstrap bench-build

bench: all
	$(MAKE) -C bootstrap bench
	python3 bench/startup.py

install: all
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(LIBDIR)/selfhost"
	install -m 755 bootstrap/build/lune "$(DESTDIR)$(LIBDIR)/lune"
	install -m 644 bootstrap/build/selfhost/*.lbc "$(DESTDIR)$(LIBDIR)/selfhost/"
	ln -sfn ../lib/lune/lune "$(DESTDIR)$(BINDIR)/lune"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/lune"
	rm -rf "$(DESTDIR)$(LIBDIR)"

clean:
	$(MAKE) -C bootstrap clean
