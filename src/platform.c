#include "platform.h"
#include "cry.h"
#include "glfuncs.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __APPLE__
#include <unistd.h>
#include <libgen.h>
#endif

void plat_bootstrap(int argc, char **argv)
{
    (void)argc;
#ifdef __APPLE__
    /* SDL must dlopen ANGLE's libEGL/libGLESv2, which dyld only finds
     * via DYLD_LIBRARY_PATH — and dyld reads that env var at process
     * start, so set it and re-exec once. */
    if (getenv("VLM_BOOTSTRAPPED"))
        return;
    char exe[4096];
    strncpy(exe, argv[0], sizeof exe - 1);
    exe[sizeof exe - 1] = 0;
    char *dir = dirname(exe);
    char path[8192];
    snprintf(path, sizeof path, "%s:%s/../libs/libgles/lib-mac:%s",
             dir, dir, getenv("DYLD_LIBRARY_PATH") ? getenv("DYLD_LIBRARY_PATH") : "");
    setenv("DYLD_LIBRARY_PATH", path, 1);
    setenv("VLM_BOOTSTRAPPED", "1", 1);
    execv(argv[0], argv);
    /* If exec fails, fall through and let SDL report the real error. */
#else
    (void)argv;
#endif
}

static struct {
    SDL_Window *window;
    SDL_GLContext gl;
    int fb_w, fb_h;
    GLuint tex;
    GLuint prog;
    GLuint vbo;
    GLint a_pos;
    GLint u_tex;
    uint32_t *rgba;        /* staging buffer for CRY->RGBA conversion */
    uint32_t buttons;
    uint32_t t0;
    uint32_t next_frame;   /* 60Hz pacing, in ms*3 fixed point (16.6667ms => 50/3) */
} P;

static const char *VS_SRC =
    "attribute vec2 a_pos;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = a_pos * vec2(0.5, -0.5) + 0.5;\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *FS_SRC =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader compile failed: %s\n", log);
        exit(1);
    }
    return s;
}

bool plat_init(const PlatformConfig *cfg)
{
    memset(&P, 0, sizeof P);
    P.fb_w = cfg->fb_width;
    P.fb_h = cfg->fb_height;

    cry_init();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }

    /* Use ANGLE / real GLES2 everywhere (same setup sgi-demos uses). */
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_EGL, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    P.window = SDL_CreateWindow(
        cfg->title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        P.fb_w * cfg->window_scale, P.fb_h * cfg->window_scale,
        SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN |
        SDL_WINDOW_RESIZABLE);
    if (!P.window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return false;
    }

    P.gl = SDL_GL_CreateContext(P.window);
    if (!P.gl) {
        fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetSwapInterval(1);
    if (!gl_load_funcs())
        return false;
    printf("GL renderer: %s\n", (const char *)glGetString(GL_RENDERER));
    printf("GL version:  %s\n", (const char *)glGetString(GL_VERSION));

    GLuint vs = compile_shader(GL_VERTEX_SHADER, VS_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FS_SRC);
    P.prog = glCreateProgram();
    glAttachShader(P.prog, vs);
    glAttachShader(P.prog, fs);
    glLinkProgram(P.prog);
    GLint ok = 0;
    glGetProgramiv(P.prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        fprintf(stderr, "program link failed\n");
        return false;
    }
    P.a_pos = glGetAttribLocation(P.prog, "a_pos");
    P.u_tex = glGetUniformLocation(P.prog, "u_tex");

    static const GLfloat quad[] = { -1,-1,  1,-1,  -1,1,  1,1 };
    glGenBuffers(1, &P.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, P.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);

    glGenTextures(1, &P.tex);
    glBindTexture(GL_TEXTURE_2D, P.tex);
    /* Nearest: the Jaguar's chunky pixels are part of the look. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, P.fb_w, P.fb_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    P.rgba = malloc((size_t)P.fb_w * P.fb_h * 4);
    if (!P.rgba)
        return false;

    P.t0 = SDL_GetTicks();
    P.next_frame = P.t0 * 3;
    return true;
}

void plat_shutdown(void)
{
    free(P.rgba);
    if (P.gl)
        SDL_GL_DeleteContext(P.gl);
    if (P.window)
        SDL_DestroyWindow(P.window);
    SDL_Quit();
}

static void handle_key(SDL_Keycode key, bool down)
{
    uint32_t bit = 0;
    switch (key) {
    case SDLK_UP:       bit = PAD_UP; break;
    case SDLK_DOWN:     bit = PAD_DOWN; break;
    case SDLK_LEFT:     bit = PAD_LEFT; break;
    case SDLK_RIGHT:    bit = PAD_RIGHT; break;
    case SDLK_z:        bit = PAD_A; break;
    case SDLK_x:        bit = PAD_B; break;
    case SDLK_c:        bit = PAD_C; break;
    case SDLK_o:        bit = PAD_OPTION; break;
    case SDLK_p:        bit = PAD_PAUSE; break;
    case SDLK_v:        bit = PAD_SCOPE; break;
    case SDLK_m:
    case SDLK_KP_MULTIPLY: bit = PAD_STAR; break;
    case SDLK_h:
    case SDLK_KP_DIVIDE:   bit = PAD_HASH; break;
    default: break;
    }
    if (key >= SDLK_0 && key <= SDLK_9)
        bit = PAD_DIGIT(key - SDLK_0);
    if (key >= SDLK_KP_1 && key <= SDLK_KP_9)
        bit = PAD_DIGIT(key - SDLK_KP_1 + 1);
    if (key == SDLK_KP_0)
        bit = PAD_DIGIT(0);
    if (down)
        P.buttons |= bit;
    else
        P.buttons &= ~bit;
}

#ifdef __EMSCRIPTEN__
static bool (*em_tick)(void);

static void em_loop(void)
{
    if (!em_tick())
        emscripten_cancel_main_loop();
}
#endif

void plat_run(bool (*tick)(void))
{
#ifdef __EMSCRIPTEN__
    em_tick = tick;
    /* Ask the browser for 60Hz to match the Jaguar VI cadence; the
     * native pacing in plat_present_cry() is compiled out for wasm. */
    emscripten_set_main_loop(em_loop, 60, 1);
#else
    while (tick())
        ;
#endif
}

bool plat_poll(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            return false;
        case SDL_KEYDOWN:
            if (e.key.keysym.sym == SDLK_ESCAPE)
                return false;
            handle_key(e.key.keysym.sym, true);
            break;
        case SDL_KEYUP:
            handle_key(e.key.keysym.sym, false);
            break;
        default:
            break;
        }
    }
    return true;
}

uint32_t plat_buttons(void)
{
    return P.buttons;
}

uint32_t plat_ticks(void)
{
    return SDL_GetTicks() - P.t0;
}

void plat_present_cry(const uint16_t *cry_pixels)
{
    const int n = P.fb_w * P.fb_h;
    for (int i = 0; i < n; i++)
        P.rgba[i] = cry_to_rgba[cry_pixels[i]];

    int win_w, win_h;
    SDL_GL_GetDrawableSize(P.window, &win_w, &win_h);

    /* Letterbox to preserve the framebuffer aspect. */
    float fb_aspect = (float)P.fb_w / (float)P.fb_h;
    int vw = win_w, vh = (int)(win_w / fb_aspect + 0.5f);
    if (vh > win_h) {
        vh = win_h;
        vw = (int)(win_h * fb_aspect + 0.5f);
    }
    glViewport((win_w - vw) / 2, (win_h - vh) / 2, vw, vh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(P.prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, P.tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, P.fb_w, P.fb_h,
                    GL_RGBA, GL_UNSIGNED_BYTE, P.rgba);
    glUniform1i(P.u_tex, 0);
    glBindBuffer(GL_ARRAY_BUFFER, P.vbo);
    glEnableVertexAttribArray((GLuint)P.a_pos);
    glVertexAttribPointer((GLuint)P.a_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    SDL_GL_SwapWindow(P.window);

#ifndef __EMSCRIPTEN__
    /* Pace to 60Hz independent of display refresh. In the browser the
     * emscripten_set_main_loop(..., 60, ...) callback provides the
     * cadence instead, so no delay there. */
    P.next_frame += 50; /* 50/3 ms per frame, in ms*3 units */
    uint32_t now3 = SDL_GetTicks() * 3;
    if ((int32_t)(P.next_frame - now3) > 3)
        SDL_Delay((P.next_frame - now3) / 3);
    else if ((int32_t)(now3 - P.next_frame) > 300)
        P.next_frame = now3; /* fell behind badly: resync */
#endif
}
