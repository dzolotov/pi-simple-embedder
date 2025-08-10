#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <mutex>
#include <dlfcn.h>
#include <cmath>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <flutter_embedder.h>

// Window dimensions
static const int WINDOW_WIDTH = 1280;
static const int WINDOW_HEIGHT = 720;

// X11 variables
static Display *display = nullptr;
static Window window;
static int screen;
static Atom wm_delete_window;
static XVisualInfo *vinfo = nullptr;
static Colormap colormap = 0;

// EGL/OpenGL
static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLContext egl_context = EGL_NO_CONTEXT;
static EGLContext egl_resource_context = EGL_NO_CONTEXT;
static EGLSurface egl_surface = EGL_NO_SURFACE;
static EGLSurface egl_pbuffer = EGL_NO_SURFACE;
static EGLConfig egl_config;

// Flutter engine
static FlutterEngine engine = nullptr;
static bool running = true;
static bool surface_ready = false;

// Thread synchronization
static std::mutex egl_mutex;

// Flutter OpenGL callbacks
static bool make_current(void *user_data) {
    (void)user_data;
    EGLBoolean ok = eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
    fprintf(stderr, "[make_current] ok=%d err=0x%04x\n", ok, eglGetError());
    return ok == EGL_TRUE;
}

static bool clear_current(void *user_data) {
    (void)user_data;
    EGLBoolean ok = eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    fprintf(stderr, "[clear_current] ok=%d err=0x%04x\n", ok, eglGetError());
    return ok == EGL_TRUE;
}

static bool present(void *user_data) {
    (void)user_data;
    EGLBoolean ok = eglSwapBuffers(egl_display, egl_surface);
    fprintf(stderr, "[present] ok=%d err=0x%04x\n", ok, eglGetError());
    return ok == EGL_TRUE;
}

static uint32_t fbo_callback(void *user_data) {
    (void)user_data;
    return 0; // Default framebuffer
}

// GL function resolver
static void* libgles = nullptr;

static void* resolve_gl(const char* name) {
    // 1) core функции из libGLESv2
    if (!libgles) {
        libgles = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_LOCAL);
        if (!libgles) libgles = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_LOCAL);
    }
    void* p = nullptr;
    if (libgles) p = dlsym(libgles, name);
    // 2) расширения через eglGetProcAddress
    if (!p) p = (void*)eglGetProcAddress(name);
    // 3) на всякий — глобальный поиск
    if (!p) p = dlsym(RTLD_DEFAULT, name);
    return p;
}

static void *gl_proc_resolver(void *user_data, const char *name) {
    (void)user_data;
    void* proc = resolve_gl(name);
    
    // Отладка для glGetString
    if (strcmp(name, "glGetString") == 0) {
        fprintf(stderr, "Flutter asks for glGetString: %p, current ctx: %p\n", 
                proc, eglGetCurrentContext());
        
        // Тестируем прямо здесь
        if (proc && eglGetCurrentContext() != EGL_NO_CONTEXT) {
            using GLGetString = const GLubyte* (*)(GLenum);
            auto pGetString = (GLGetString)proc;
            const char* ver = (const char*)pGetString(GL_VERSION);
            fprintf(stderr, "  Test call result: %s\n", ver ? ver : "<NULL>");
        }
    }
    
    return proc;
}

static bool make_resource_current(void *user_data) {
    (void)user_data;
    
    if (egl_resource_context == EGL_NO_CONTEXT) {
        return false;
    }
    
    EGLSurface s = (egl_pbuffer != EGL_NO_SURFACE) ? egl_pbuffer : EGL_NO_SURFACE;
    EGLBoolean ok = eglMakeCurrent(egl_display, s, s, egl_resource_context);
    fprintf(stderr, "[resource_current] ok=%d err=0x%04x\n", ok, eglGetError());
    return ok == EGL_TRUE;
}

// Vsync callback - Flutter calls this when it needs the next frame
static void vsync_callback(void* /*user_data*/, intptr_t baton) {
    // timestamps в микросекундах (Flutter монотонное время)
    uint64_t now = FlutterEngineGetCurrentTime();
    // целимся в 60 Гц ( ~16.666 ms )
    const uint64_t interval = 16666666ull;
    uint64_t frame_start  = now;
    uint64_t frame_target = now + interval;

    FlutterEngineResult r = FlutterEngineOnVsync(engine, baton, frame_start, frame_target);
    if (r != kSuccess) {
        fprintf(stderr, "OnVsync failed: %d\n", r);
    }
}

// Initialize X11 window with proper visual
static bool init_x11_with_visual(XVisualInfo* vinfo, Colormap cmap) {
    if (!display) {
        display = XOpenDisplay(nullptr);
        if (!display) {
            fprintf(stderr, "Failed to open X display\n");
            return false;
        }
    }
    screen = DefaultScreen(display);
    
    XSetWindowAttributes swa = {};
    swa.colormap = cmap;
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     StructureNotifyMask;
    
    window = XCreateWindow(
        display, RootWindow(display, vinfo->screen),
        0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, 0,
        vinfo->depth, InputOutput, vinfo->visual,
        CWColormap | CWEventMask, &swa);
    
    if (!window) {
        fprintf(stderr, "XCreateWindow failed\n");
        return false;
    }
    
    XStoreName(display, window, "Flutter on X11");
    wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete_window, 1);
    
    XMapWindow(display, window);
    XSync(display, False);
    
    printf("X11 window created with proper visual (ID: %lu)\n", window);
    return true;
}

// Initialize EGL and get proper visual
static bool init_egl() {
    // 1) Get EGL display
    egl_display = eglGetDisplay(display);
    if (egl_display == EGL_NO_DISPLAY) {
        // Try platform-specific approach
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = 
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (get_platform_display) {
            egl_display = get_platform_display(EGL_PLATFORM_X11_KHR, display, nullptr);
        }
    }
    if (egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display\n");
        return false;
    }
    
    // Initialize EGL
    EGLint major, minor;
    if (!eglInitialize(egl_display, &major, &minor)) {
        fprintf(stderr, "eglInitialize failed\n");
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    
    printf("EGL version: %d.%d\n", major, minor);
    
    // 2) Choose config with depth/stencil for Skia
    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    EGLint num_cfg = 0;
    if (!eglChooseConfig(egl_display, cfg_attrs, &egl_config, 1, &num_cfg) || num_cfg == 0) {
        fprintf(stderr, "eglChooseConfig failed\n");
        return false;
    }
    
    // 3) Get native visual ID and create XVisualInfo
    EGLint vid = 0;
    eglGetConfigAttrib(egl_display, egl_config, EGL_NATIVE_VISUAL_ID, &vid);
    if (!vid) {
        fprintf(stderr, "No EGL_NATIVE_VISUAL_ID in config\n");
        return false;
    }
    
    XVisualInfo tmpl = {}; 
    tmpl.visualid = (VisualID)vid;
    int nvi = 0;
    vinfo = XGetVisualInfo(display, VisualIDMask, &tmpl, &nvi);
    if (!vinfo || nvi == 0) {
        fprintf(stderr, "XGetVisualInfo failed for visual 0x%lx\n", (unsigned long)vid);
        return false;
    }
    
    // 4) Create colormap for this visual
    colormap = XCreateColormap(display, RootWindow(display, vinfo->screen), vinfo->visual, AllocNone);
    
    printf("EGL config matches X11 visual 0x%lx\n", (unsigned long)vid);
    return true;
}

// Create EGL surface and contexts after X11 window is created
static bool create_egl_surface_and_contexts() {
    // Create window surface
    egl_surface = eglCreateWindowSurface(egl_display, egl_config, (EGLNativeWindowType)window, nullptr);
    if (egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "eglCreateWindowSurface failed: 0x%04x\n", eglGetError());
        return false;
    }
    
    // Create contexts
    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, ctx_attrs);
    if (egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context: 0x%04x\n", eglGetError());
        return false;
    }
    
    egl_resource_context = eglCreateContext(egl_display, egl_config, egl_context, ctx_attrs);
    if (egl_resource_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create shared context: 0x%04x\n", eglGetError());
        return false;
    }
    
    // Create pbuffer for resource context
    const EGLint pbuf_attrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    egl_pbuffer = eglCreatePbufferSurface(egl_display, egl_config, pbuf_attrs);
    if (egl_pbuffer == EGL_NO_SURFACE) {
        fprintf(stderr, "eglCreatePbufferSurface failed: 0x%04x\n", eglGetError());
    }
    
    printf("EGL surface and contexts created successfully\n");
    return true;
}

// Initialize Flutter engine
static bool init_flutter(const char* assets_path, const char* icu_path) {
    // Set up renderer config
    FlutterRendererConfig config = {};
    config.type = kOpenGL;
    config.open_gl.struct_size = sizeof(FlutterOpenGLRendererConfig);
    config.open_gl.make_current = make_current;
    config.open_gl.clear_current = clear_current;
    config.open_gl.present = present;
    config.open_gl.fbo_callback = fbo_callback;
    config.open_gl.make_resource_current = make_resource_current;
    config.open_gl.gl_proc_resolver = gl_proc_resolver;
    
    // Set up project args
    FlutterProjectArgs args = {};
    args.struct_size = sizeof(FlutterProjectArgs);
    args.assets_path = assets_path;
    args.icu_data_path = icu_path;
    args.vsync_callback = vsync_callback;
    
    // Command line arguments for debug mode
    const char* command_line_args[] = {
        "--disable-service-auth-codes",
    };
    
    args.command_line_argc = sizeof(command_line_args) / sizeof(command_line_args[0]);
    args.command_line_argv = command_line_args;
    
    // Create engine
    FlutterEngineResult result = FlutterEngineRun(
        FLUTTER_ENGINE_VERSION,
        &config,
        &args,
        nullptr,
        &engine
    );
    
    if (result != kSuccess) {
        fprintf(stderr, "Failed to start Flutter engine: %d\n", result);
        return false;
    }
    
    printf("Flutter engine started\n");
    surface_ready = true;
    
    // Send initial window metrics
    FlutterWindowMetricsEvent event = {};
    event.struct_size = sizeof(event);
    event.width = WINDOW_WIDTH;
    event.height = WINDOW_HEIGHT;
    event.pixel_ratio = 1.0;
    
    result = FlutterEngineSendWindowMetricsEvent(engine, &event);
    if (result != kSuccess) {
        fprintf(stderr, "Failed to send window metrics\n");
    }
    
    return true;
}

// Convert X11 button to Flutter button
static FlutterPointerMouseButtons x11_button_to_flutter(unsigned int button) {
    switch (button) {
        case Button1: return kFlutterPointerButtonMousePrimary;
        case Button2: return kFlutterPointerButtonMouseMiddle;
        case Button3: return kFlutterPointerButtonMouseSecondary;
        default: return (FlutterPointerMouseButtons)0;
    }
}

// Handle X11 events
static void handle_events() {
    XEvent event;
    static bool first_expose = true;
    static uint64_t last_vsync = 0;
    
    while (running) {
        // Check for X11 events
        while (XPending(display) > 0) {
            XNextEvent(display, &event);
            
            switch (event.type) {
                case Expose:
                    // Window exposed - Flutter will request vsync through callback
                    if (first_expose) {
                        first_expose = false;
                        printf("Window exposed - Flutter will request vsync\n");
                    }
                    break;
                    
                case ConfigureNotify:
                    // Window resized
                    if (engine && (event.xconfigure.width > 0 && event.xconfigure.height > 0)) {
                        FlutterWindowMetricsEvent metrics = {};
                        metrics.struct_size = sizeof(metrics);
                        metrics.width = event.xconfigure.width;
                        metrics.height = event.xconfigure.height;
                        metrics.pixel_ratio = 1.0;
                        FlutterEngineSendWindowMetricsEvent(engine, &metrics);
                    }
                    break;
                    
                case MotionNotify:
                    // Mouse move
                    if (engine) {
                        FlutterPointerEvent pointer = {};
                        pointer.struct_size = sizeof(pointer);
                        pointer.phase = kMove;
                        pointer.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()
                        ).count();
                        pointer.x = event.xmotion.x;
                        pointer.y = event.xmotion.y;
                        pointer.device = 0;
                        pointer.signal_kind = kFlutterPointerSignalKindNone;
                        pointer.device_kind = kFlutterPointerDeviceKindMouse;
                        
                        FlutterEngineSendPointerEvent(engine, &pointer, 1);
                    }
                    break;
                    
                case ButtonPress:
                case ButtonRelease:
                    // Mouse button
                    if (engine) {
                        FlutterPointerEvent pointer = {};
                        pointer.struct_size = sizeof(pointer);
                        pointer.phase = (event.type == ButtonPress) ? kDown : kUp;
                        pointer.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()
                        ).count();
                        pointer.x = event.xbutton.x;
                        pointer.y = event.xbutton.y;
                        pointer.device = 0;
                        pointer.signal_kind = kFlutterPointerSignalKindNone;
                        pointer.device_kind = kFlutterPointerDeviceKindMouse;
                        pointer.buttons = x11_button_to_flutter(event.xbutton.button);
                        
                        FlutterEngineSendPointerEvent(engine, &pointer, 1);
                    }
                    break;
                    
                case KeyPress:
                case KeyRelease:
                    // Keyboard events (simplified - in production would need proper key mapping)
                    break;
                    
                case ClientMessage:
                    // Window close
                    if ((Atom)event.xclient.data.l[0] == wm_delete_window) {
                        running = false;
                    }
                    break;
            }
        }
        
        // Vsync disabled for now to avoid issues
        
        // Force display update
        XFlush(display);
        
        // Small sleep to prevent CPU spinning
        usleep(16666); // ~60 FPS
    }
}

int main(int argc, char **argv) {
    printf("Flutter X11 Embedder\n");
    printf("====================\n");
    
    // Parse command line arguments
    const char* assets_path = "flutter_assets";
    const char* icu_path = "icudtl.dat";
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc) {
            assets_path = argv[i + 1];
            i++; // Skip next argument
        } else if (strcmp(argv[i], "--icu") == 0 && i + 1 < argc) {
            icu_path = argv[i + 1];
            i++; // Skip next argument
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [OPTIONS]\n", argv[0]);
            printf("Options:\n");
            printf("  --assets PATH    Path to flutter_assets directory (default: flutter_assets)\n");
            printf("  --icu PATH       Path to icudtl.dat file (default: icudtl.dat)\n");
            printf("  --help, -h       Show this help message\n");
            return 0;
        }
    }
    
    printf("Assets path: %s\n", assets_path);
    printf("ICU data path: %s\n", icu_path);
    
    // 1) XInitThreads before any X11 calls
    if (!XInitThreads()) {
        fprintf(stderr, "XInitThreads failed\n");
        return 1;
    }
    
    // 2) Open display and initialize EGL to get proper visual
    display = XOpenDisplay(nullptr);
    if (!display) {
        fprintf(stderr, "Failed to open X display\n");
        return 1;
    }
    
    if (!init_egl()) {
        return 1;
    }
    
    // 3) Create X11 window with correct visual
    if (!init_x11_with_visual(vinfo, colormap)) {
        return 1;
    }
    
    // 4) Create EGL surface and contexts after window is ready
    if (!create_egl_surface_and_contexts()) {
        return 1;
    }
    
    // 5) Initialize Flutter engine
    printf("Ready to initialize Flutter...\n");
    if (!init_flutter(assets_path, icu_path)) {
        return 1;
    }
    
    printf("Application running. Close window to exit.\n");
    
    // Main event loop
    handle_events();
    
    // Cleanup
    if (engine) {
        FlutterEngineShutdown(engine);
    }
    
    if (egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(egl_display, egl_surface);
    }
    
    if (egl_pbuffer != EGL_NO_SURFACE) {
        eglDestroySurface(egl_display, egl_pbuffer);
    }
    
    if (egl_resource_context != EGL_NO_CONTEXT) {
        eglDestroyContext(egl_display, egl_resource_context);
    }
    
    if (egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(egl_display, egl_context);
    }
    
    if (egl_display != EGL_NO_DISPLAY) {
        eglTerminate(egl_display);
    }
    
    if (display) {
        if (colormap) XFreeColormap(display, colormap);
        if (vinfo) XFree(vinfo);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
    }
    
    printf("Application terminated.\n");
    
    return 0;
}