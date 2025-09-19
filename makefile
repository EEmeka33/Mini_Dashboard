CC = gcc
CFLAGS = -I. \
         -I./lvgl \
         -I./lvgl/src \
         -I./lvgl/demos \
         -I./lvgl/src/drivers/sdl \
         -I./lvgl/src/libs/lodepng \
         -Wall -O2 `sdl2-config --cflags`

LDLIBS = `sdl2-config --libs`

# SOURCES: compile 1.c separately (do NOT #include it into main.c)
SRC = main.c \
      $(wildcard lvgl/src/*.c) \
      $(wildcard lvgl/src/*/*.c) \
      $(wildcard lvgl/src/*/*/*.c) \
      $(wildcard lvgl/src/*/*/*/*.c) \
      $(wildcard lvgl/demos/*.c) \
      $(wildcard lvgl/demos/*/*.c) \
      $(wildcard lvgl/demos/*/*/*.c) \
      $(wildcard lvgl/demos/*/*/*/*.c)

OBJ = $(SRC:.c=.o)

app: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) app
