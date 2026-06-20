CFLAGS = -Wall -Wextra -Wextra -Wpedantic -g -fsanitize=address -fno-omit-frame-pointer

halloc: halloc.c main.c
	gcc main.c halloc.c -o halloc $(CFLAGS)