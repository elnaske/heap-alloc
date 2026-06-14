CFLAGS = -Wall -Wextra -Wextra -Wpedantic -g -fsanitize=address -fno-omit-frame-pointer

halloc: halloc.c
	gcc halloc.c -o halloc $(CFLAGS)