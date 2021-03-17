highlight-pointer: highlight-pointer.c
	$(CC) $^ -o $@ -flto -O3 -Wall -Wextra -Wshadow -std=c99 -lX11 -lXext -lXfixes -lXi
