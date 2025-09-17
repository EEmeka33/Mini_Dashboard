CC = gcc
CFLAGS = -I. \
         -I./lvgl \
         -I./lvgl/src \
         -I./lvgl/demos \
         -I./lvgl/src/drivers/sdl \
         -Wall `sdl2-config --cflags`

LDFLAGS = `sdl2-config --libs`

# Collect all sources recursively
SRC = main.c \
      $(wildcard lvgl/src/*.c) \
      $(wildcard lvgl/src/*/*.c) \
      $(wildcard lvgl/src/*/*/*.c) \
      $(wildcard lvgl/src/*/*/*/*.c) \
      $(wildcard lvgl/demos/*.c) \
      $(wildcard lvgl/demos/*/*.c) \
      $(wildcard lvgl/demos/*/*/*.c) \
      $(wildcard lvgl/demos/*/*/*/*.c) \
      $(wildcard lvgl/src/drivers/sdl/*.c)

OBJ = $(SRC:.c=.o)

app: $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(OBJ) app
