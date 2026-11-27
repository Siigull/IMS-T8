# Vendored raylib

raylib 5.5 (https://github.com/raysan5/raylib), commit at clone time 2026-11,
built from source for this project:

- platform: DESKTOP (GLFW, X11), graphics API OpenGL 3.3, no audio module
- `include/` — raylib.h, raymath.h, rlgl.h, config.h
- `lib/libraylib.a` — static library built with:
  `gcc -c src/{rcore,rglfw,rshapes,rtextures,rtext,rmodels,utils}.c -O2
  -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_33 -D_GLFW_X11
  -I src -I src/external/glfw/include && ar rcs libraylib.a *.o`

Link with: `-I third_party/raylib/include third_party/raylib/lib/libraylib.a
-lGL -lm -lpthread -ldl -lrt -lX11`

(Upstream's Makefile misbehaves on this toolchain — a bare `-D` flag — hence
the manual build. Only the prebuilt .a is committed; the full source is not
vendored, get it from upstream if you need to rebuild.)
