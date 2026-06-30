CC ?= cc
CFLAGS ?= -flto -O3 -Wall -Wextra -Wshadow -std=c99
LDLIBS ?= -lX11 -lXext -lXfixes -lXi

highlight-pointer: highlight-pointer.c
	$(CC) $(CPPFLAGS) $(CFLAGS) highlight-pointer.c -o highlight-pointer $(LDFLAGS) $(LDLIBS)
