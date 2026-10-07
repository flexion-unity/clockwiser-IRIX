# IRIX compatibility

- Static SDL2 (`libSDL2.a`) and SDL2_image (`libSDL2_image.a`) are bundled in
  `IRIX-SDL2/lib32`, headers in `IRIX-SDL2/include/SDL2`. On IRIX, CMake uses
  these and never searches the system for SDL2 or SDL2_image.

- Needs at least SDL 2.0.18 for SDL_GetTicks64()
- Needs at least SDL2_image 2.6.0.  clockwiser_movie_player.c uses them to play animated GIFs

# cmake options

without 'copy data' in POST_BUILD:

$ cmake -B build -DCLOCKWISER_COPY_DATA=OFF -DCMAKE_BUILD_TYPE=Release

Note: an optimized release build will take a loooong time to build on MIPS. Depending on your CPU speed, it can take 15 minutes or longer
