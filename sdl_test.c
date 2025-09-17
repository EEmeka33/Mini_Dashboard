#include <SDL2/SDL.h>
#include <stdio.h>

int main(int argc, char *argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SDL_Init Error: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow("SDL Test", 
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       400, 400, SDL_WINDOW_SHOWN);
    if (!win) {
        printf("SDL_CreateWindow Error: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) {
        printf("SDL_CreateRenderer Error: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    int quit = 0;
    SDL_Event e;

    while (!quit) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT ||
                (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) {
                quit = 1;
            }
        }

        SDL_SetRenderDrawColor(ren, 255, 0, 0, 255); // red
        SDL_RenderClear(ren);
        SDL_RenderPresent(ren);

        SDL_Delay(16); // ~60 FPS
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();

    return 0;
}

/*
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
*/