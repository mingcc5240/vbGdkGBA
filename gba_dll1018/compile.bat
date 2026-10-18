c:\mingw\bin\gcc -m32 -O3 -fomit-frame-pointer -funroll-loops -shared -o gdkgba.dll arm.c arm_mem.c dma.c io.c sound.c timer.c video.c gba_dll.c -Wl,--kill-at -static
