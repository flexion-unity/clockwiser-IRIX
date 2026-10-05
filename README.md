# Clockwiser Portable

Portable C/SDL2 port of the Amiga game **Clockwiser** generated from 68000 assembly source code.

## Gameplay Instructions

Check the Demo mode that's accessible from the Clockwiser menu for gameplay instructions.

## About the port

This port has been optimized to be 'portable' (Can be run on any hardware). However it is not very readable. For every 68000 source code instruction, it was transformed into an equivalent bit of c code. The assembly code wasn't already the most readable thing ever (it was created some 32 years ago), the transformed c code will be added noise on top of it. If you look closely at the c code, you can still read the assembly opcodes. It is even possible to edit the c code and alter the game, but it is not adviced to do so. The game runs on two things: a 68000k runtime and an Amiga host. The 68k runtime keeps track of the processor flags and state, so that the c code logically works the same as the original. The Amiga host is a very thin Amiga emulation layer which emulates the basics needed to run Clockwiser (Bitplanes, few videomodes, copperlists and sprites).

How the original game was created: Clockwiser originally was 1 big assembly file (263 kb). With our transpiler this sourcefile has been transpiled to c code and you can find these in the repository. 
A little bit of 'glue' has been build around Clockwiser. So some code to transform mouse coordinates from the host os to the Amiga/game world, a sound effect handler, music player and some logic around the loading of levelpacks.

The 68000 -> c code transpiler is released and this is a much better workflow for people who want to mod the gameplay:

- Alter the original source
- Transpile into c code
- Compile c code
- Test the game

The transpiler allows for custom 'hooks' into the assembly where you can do things outside of what an 'Amiga' could do, and call into your own native code.   

This transpiler also allows any other Amiga developer to port their old games (Given that they also made it in 68k assembly using the Asm-one or Seka assembler) to the modern platforms, so people can play it without having to install Emulators.  

Clockwiser doesn't call into any operating system function of the Amiga (It basically removed all of it on startup) and so I didn't need to emulate any of that. The only part where I did need to emulate the OS was the disk access methods. Amiga games that rely more on Amiga libraries will not work using this method without some additional effrot.

The transpiler can be found here: <[https://bitbucket.org/rhinoid/convert68000toc/src/main/](https://bitbucket.org/rhinoid/convert68000toc/src/main/)>

## Status

This repository is a first release of the game. I don't expect a lot of changes being added. Maybe just 'web browser' support (so I can run it on my site), and bugfixes in case these are found. 

## Requirements

- CMake 3.16+
- C compiler (clang/gcc/msvc)
- SDL2
- SDL2_image 2.6+ (PNG menus and animated GIF intro/extro)
- Optional: SDL2_mixer (music)

## Build

### Mac:
```bash
cmake -S . -B build
cmake --build build
```

Run from the build directory:

```bash
cd build
open clockwiser.app
```

### Windows:
```Cmd shell (with compiler paths correct)
   cmake -S . -B build `
  -DSDL2_DIR=D:/sdl/deps/SDL2-2.30.11/cmake `
  -DSDL2_image_DIR=D:/sdl/deps/SDL2_image-devel-2.8.12-VC/SDL2_image-2.8.12/cmake `
  -DSDL2_mixer_DIR=D:/sdl/deps/SDL2_mixer-devel-2.8.2-VC/SDL2_mixer-2.8.2/cmake

(Change the pathes to where you have downloaded the SDL libraries.)
then:

cmake --build build
```

Run from the build directory:

```Cmd
cd build\Debg
clockwiser.exe
```

## Create Portable Package (Mac)

This creates distributable archives that include the executable, `data/`, and discovered runtime shared libraries.

```bash
cmake -S . -B build
cmake --build build
cmake --build build --target package
```

Generated artifacts are placed in `build/` (for example: `.zip`, `.tar.gz`).

### Emscriptem

Basic support for emscriptem added (Run on Web) just to see if it works. See the CMakeLists.txt for details.
Most 'lazy' version possible. One load of all the assets, no special async data loading, special webpage or any other 
optimization. ALso note that disk functions don't work in the web version.

## Editor

Clockwiser comes with a level editor. The level editor is clucnky and designed to make some sense on the Amiga. It works a bit odd when seen with modern sensibilities.

How it works is as following:
 - On initial startup, all .pack files are copied to the working folder (the folder on your system where preferences or hiscores are saved. On Mac that is: "~/Library/Application Support/ClockwiserPortable/ClockwiserPortable") 
 - In the game when you press the disk icon you can load any of these levelpacks.
 - a levelpack contains 100 levels
 - WHen you go to the editor you can select any of the levels and 'rstore them'.
 - you can then edit them... add blocks or remove blocks.
 - when you are happy, you need to 'hussle' the level. 
 - when you reach the desired endstate you can press the gallows
 - you now have created a level, give it a name and a time for solving and you can store it.
 - don't forget to store your levelpack!!!!!

yes... I think a dedicated level editor makes a lot more sense to create at some point.
For now, consider this part as a 'historically accurate' interesting thing . This is how these crazy people made levels back in the day. Yes, it can still be done. But probably not the way forward.


## Credits

Enjoy!

### Music

**Ramon Braumuller**

Bluesky: <[https://bsky.app/profile/ramonbraumuller.eurosky.social](https://bsky.app/profile/ramonbraumuller.eurosky.social)>

### Graphics

**Metin Seven**

Web: <[https://www.metinseven.nl](https://www.metinseven.nl)>
Bluesky: <[https://bsky.app/profile/seven.eurosky.social](https://bsky.app/profile/seven.eurosky.social)>

### Original game code (and porting code)

**Reinier van Vliet**

Web: <[https://www.proofofconcept.nl](https://www.proofofconcept.nl)>
Bluesky: <[https://bsky.app/profile/reiniervanvliet.eurosky.social](https://bsky.app/profile/reiniervanvliet.eurosky.social)>

