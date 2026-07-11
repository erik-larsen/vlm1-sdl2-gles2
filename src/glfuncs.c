#include <stdbool.h>
#include <stdio.h>

#include <SDL.h>

/* Suppress the gl* -> GL.* macros while defining the loader itself. */
#define VLM_GLFUNCS_NO_MACROS
#include "glfuncs.h"

GlFuncs GL;

bool gl_load_funcs(void)
{
    bool ok = true;
#define GL_LOAD(ret, name, args) \
    GL.name = (ret (*) args)SDL_GL_GetProcAddress("gl" #name); \
    if (!GL.name) { fprintf(stderr, "missing GL function: gl%s\n", #name); ok = false; }
    GL_FUNC_LIST(GL_LOAD)
#undef GL_LOAD
    return ok;
}
