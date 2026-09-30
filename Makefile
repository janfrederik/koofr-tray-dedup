PREFIX ?= $(HOME)/.local/lib/koofr-tray-dedup
CFLAGS ?= -O2 -Wall -Wextra -Werror

# Named like the library Koofr tries first, so LD_LIBRARY_PATH picks it up.
libappindicator3.so.1: koofr_tray_dedup.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< -ldl -lpthread

install: libappindicator3.so.1
	install -D -m 0644 $< $(PREFIX)/$<

clean:
	rm -f libappindicator3.so.1

.PHONY: install clean
