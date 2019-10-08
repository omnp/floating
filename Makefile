draw: draw.c image.h image.c Makefile
	gcc -g -std=gnu17 -o draw -ltiff -lm -lxcb -lxcb-xinput -Wall -fopenmp -march=native -mavx draw.c image.c -Wextra -pedantic -Werror -Wno-unused -O3 -flto
all: draw
.phony: all

