/*
  highlight-ponter

  Highlight mouse pointer/cursor using a dot - useful for
  presentations, screen sharing, ...

  MIT License

  Copyright (c) 2020 Sven Willner <sven.willner@yfx.de>

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xmd.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#define TARGET_FPS 0

static Display* dpy;
static GC gc = 0;
static Window win;
static Window root;
static int screen;
static int selfpipe[2]; /* for self-pipe trick to cancel select() call */

#define KEY_MODMAP_SIZE 4
static struct {
    char symbol;
    unsigned int modifiers;
} key_modifier_mapping[KEY_MODMAP_SIZE] = {
    {'S', ShiftMask},   /* shift */
    {'C', ControlMask}, /* control */
    {'M', Mod1Mask},    /* alt/meta */
    {'H', Mod4Mask}     /* super/"windows" */
};

#define KEY_OPTION_OFFSET 1000
#define KEY_ARRAY_SIZE 5
struct {
    KeySym keysym;
    unsigned int modifiers;
} keys[KEY_ARRAY_SIZE] = {
#define KEY_QUIT 0
    {NoSymbol, 0},
#define KEY_TOGGLE_CURSOR 1
    {NoSymbol, 0},
#define KEY_TOGGLE_HIGHLIGHT 2
    {NoSymbol, 0},
#define KEY_TOGGLE_AUTOHIDE_CURSOR 3
    {NoSymbol, 0},
#define KEY_TOGGLE_AUTOHIDE_HIGHLIGHT 4
    {NoSymbol, 0}};

static unsigned int numlockmask = 0;

static XColor pressed_color;
static XColor released_color;
static int button_pressed = 0;
static int cursor_visible = 1;
static int highlight_visible = 0;

static struct {
    char* pressed_color_string;
    char* released_color_string;
    int auto_hide_cursor;
    int auto_hide_highlight;
    int cursor_visible;
    int hide_timeout;
    int highlight_visible;
    int outline;
    int radius;
    double opacity;  // Add opacity field
} options;

static void redraw();
static int get_pointer_position(int* x, int* y);

static void show_cursor() {
    XFixesShowCursor(dpy, root);
    cursor_visible = 1;
}

static void hide_cursor() {
    XFixesHideCursor(dpy, root);
    cursor_visible = 0;
}

static void show_highlight() {
    int x, y;
    int total_radius = options.radius + options.outline;
    get_pointer_position(&x, &y);
    XMoveWindow(dpy, win, x - total_radius - 1, y - total_radius - 1);
    XMapWindow(dpy, win);
    redraw();
    highlight_visible = 1;
}

static void hide_highlight() {
    XUnmapWindow(dpy, win);
    highlight_visible = 0;
}

static int init_events() {
    XIEventMask events;
    unsigned char mask[(XI_LASTEVENT + 7) / 8];
    memset(mask, 0, sizeof(mask));

    XISetMask(mask, XI_RawButtonPress);
    XISetMask(mask, XI_RawButtonRelease);
    XISetMask(mask, XI_RawMotion);

    events.deviceid = XIAllMasterDevices;
    events.mask = mask;
    events.mask_len = sizeof(mask);

    XISelectEvents(dpy, root, &events, 1);

    return 0;
}

static int get_pointer_position(int* x, int* y) {
    Window w;
    int i;
    unsigned int ui;
    return XQueryPointer(dpy, root, &w, &w, x, y, &i, &i, &ui);
}

static void set_window_mask() {
    XGCValues gc_values;
    int total_radius = options.radius + options.outline;
    Pixmap mask = XCreatePixmap(dpy, win, 2 * total_radius + 2, 2 * total_radius + 2, 1);
    GC mask_gc = XCreateGC(dpy, mask, 0, &gc_values);
    XSetForeground(dpy, mask_gc, 0);
    XFillRectangle(dpy, mask, mask_gc, options.outline, options.outline, 2 * total_radius + 2, 2 * total_radius + 2);

    XSetForeground(dpy, mask_gc, 1);
    if (options.outline) {
        XSetLineAttributes(dpy, mask_gc, options.outline, LineSolid, CapButt, JoinBevel);
        XDrawArc(dpy, mask, mask_gc, options.outline, options.outline, 2 * options.radius + 1, 2 * options.radius + 1, 0, 360 * 64);
    } else {
        XFillArc(dpy, mask, mask_gc, options.outline, options.outline, 2 * options.radius + 1, 2 * options.radius + 1, 0, 360 * 64);
    }

    XShapeCombineMask(dpy, win, ShapeBounding, 0, 0, mask, ShapeSet);

    XFreeGC(dpy, mask_gc);
    XFreePixmap(dpy, mask);
}

static int init_window() {
    int total_radius = options.radius + options.outline;
    XSetWindowAttributes win_attributes;
    win_attributes.event_mask = ExposureMask | VisibilityChangeMask;
    win_attributes.override_redirect = True;

    win = XCreateWindow(dpy, root, options.outline, options.outline, 2 * total_radius + 2, 2 * total_radius + 2, 0, DefaultDepth(dpy, screen), InputOutput, DefaultVisual(dpy, screen),
                        CWEventMask | CWOverrideRedirect, &win_attributes);
    if (!win) {
        fprintf(stderr, "Can't create highlight window\n");
        return 1;
    }

    // Set window opacity
    unsigned long opacity_value = (unsigned long)(options.opacity * 0xFFFFFFFF);
    Atom opacity_atom = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
    XChangeProperty(dpy, win, opacity_atom, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&opacity_value, 1);

    XClassHint class_hint;
    XStoreName(dpy, win, "highlight-pointer");
    class_hint.res_name = "highlight-pointer";
    class_hint.res_class = "HighlightPointer";
    XSetClassHint(dpy, win, &class_hint);

    Atom window_type_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DND", False);
    XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False), XA_ATOM, 32, PropModeReplace, (unsigned char*)&window_type_atom, 1);

    /* hide window decorations */
    /* after https://github.com/akkana/moonroot */
    Atom motif_wm_hints = XInternAtom(dpy, "_MOTIF_WM_HINTS", True);
    struct {
        CARD32 flags;
        CARD32 functions;
        CARD32 decorations;
        INT32 input_mode;
        CARD32 status;
    } mwmhints;
    mwmhints.flags = 1L << 1 /* MWM_HINTS_DECORATIONS */;
    mwmhints.decorations = 0;
    XChangeProperty(dpy, win, motif_wm_hints, motif_wm_hints, 32, PropModeReplace, (unsigned char*)&mwmhints, 5 /* PROP_MWM_HINTS_ELEMENTS */);

    /* always stay on top */
    /* after gdk_wmspec_change_state */
    XClientMessageEvent xclient;
    memset(&xclient, 0, sizeof(xclient));
    xclient.type = ClientMessage;
    xclient.window = win;
    xclient.message_type = XInternAtom(dpy, "_NET_WM_STATE", False);
    xclient.format = 32;
    xclient.data.l[0] = 1 /* _NET_WM_STATE_ADD */;
    xclient.data.l[1] = XInternAtom(dpy, "_NET_WM_STATE_STAYS_ON_TOP", False);
    xclient.data.l[2] = 0;
    xclient.data.l[3] = 1;
    xclient.data.l[4] = 0;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, (XEvent*)&xclient);

    /* let clicks fall through */
    /* after https://stackoverflow.com/a/9279747 */
    XRectangle rect;
    XserverRegion region = XFixesCreateRegion(dpy, &rect, 1);
    XFixesSetWindowShapeRegion(dpy, win, ShapeInput, 0, 0, region);
    XFixesDestroyRegion(dpy, region);

    XGCValues gc_values;
    gc_values.foreground = WhitePixel(dpy, screen);
    gc_values.background = BlackPixel(dpy, screen);
    gc = XCreateGC(dpy, win, GCForeground | GCBackground, &gc_values);

    set_window_mask();

    return 0;
}

static void redraw() {
    XSetForeground(dpy, gc, button_pressed ? pressed_color.pixel : released_color.pixel);
    if (options.outline) {
        XSetLineAttributes(dpy, gc, options.outline, LineSolid, CapButt, JoinBevel);
        XDrawArc(dpy, win, gc, options.outline, options.outline, 2 * options.radius + 1, 2 * options.radius + 1, 0, 360 * 64);
    } else {
        XFillArc(dpy, win, gc, options.outline, options.outline, 2 * options.radius + 1, 2 * options.radius + 1, 0, 360 * 64);
    }
}

static void quit() { write(selfpipe[1], "", 1); }

static void handle_key(KeySym keysym, unsigned int modifiers) {
    modifiers = modifiers & ~(numlockmask | LockMask);
    int k;
    for (k = 0; k < KEY_ARRAY_SIZE; ++k) {
        if (keys[k].keysym == keysym && keys[k].modifiers == modifiers) {
            break;
        }
    }
    switch (k) {
        case KEY_QUIT:
            quit();
            break;

        case KEY_TOGGLE_CURSOR:
            options.cursor_visible = 1 - options.cursor_visible;
            if (options.cursor_visible && !cursor_visible) {
                show_cursor();
            } else if (!options.cursor_visible && cursor_visible) {
                hide_cursor();
            }
            break;

        case KEY_TOGGLE_HIGHLIGHT:
            if (options.highlight_visible) {
                hide_highlight();
            } else {
                show_highlight();
            }
            options.highlight_visible = 1 - options.highlight_visible;
            break;

        case KEY_TOGGLE_AUTOHIDE_CURSOR:
            options.auto_hide_cursor = 1 - options.auto_hide_cursor;
            break;

        case KEY_TOGGLE_AUTOHIDE_HIGHLIGHT:
            options.auto_hide_highlight = 1 - options.auto_hide_highlight;
            break;
    }
}

static void main_loop() {
    XEvent ev;
    fd_set fds;
    int fd = ConnectionNumber(dpy);
    struct timeval timeout;
    int x, y, n;
    int total_radius = options.radius + options.outline;
    XGenericEventCookie* cookie;
#if TARGET_FPS > 0
    Time lasttime = 0;
#endif

    pipe(selfpipe);

    while (1) {
        XFlush(dpy);
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        FD_SET(selfpipe[0], &fds);
        timeout.tv_usec = 0;
        timeout.tv_sec = options.hide_timeout;
        n = select((fd > selfpipe[0] ? fd : selfpipe[0]) + 1, &fds, NULL, NULL, &timeout);
        if (n < 0) {
            if (errno != EINTR) {
                perror("select() failed");
            }
            break;
        }
        if (n > 0) {
            if (FD_ISSET(selfpipe[0], &fds)) {
                break;
            }
            while (XPending(dpy)) {
                XNextEvent(dpy, &ev);

                if (ev.type == GenericEvent) {
                    cookie = &ev.xcookie;
#if TARGET_FPS > 0
                    if (!XGetEventData(dpy, cookie)) {
                        continue;
                    }
                    const XIRawEvent* data = (const XIRawEvent*)cookie->data;
                    if (data->time - lasttime <= 1000 / TARGET_FPS) {
                        XFreeEventData(dpy, cookie);
                        continue;
                    }
                    lasttime = data->time;
                    XFreeEventData(dpy, cookie);
#endif
                    if (cookie->evtype == XI_RawMotion) {
                        if (options.auto_hide_cursor && options.cursor_visible && !cursor_visible) {
                            show_cursor();
                        }
                        if (options.auto_hide_highlight && options.highlight_visible && !highlight_visible) {
                            show_highlight();
                        } else if (highlight_visible) {
                            get_pointer_position(&x, &y);
                            XMoveWindow(dpy, win, x - total_radius - 1, y - total_radius - 1);
                            /* unfortunately, this causes increase of the X server's cpu usage */
                        }
                        continue;
                    }
                    if (cookie->evtype == XI_RawButtonPress) {
                        button_pressed = 1;
                        redraw();
                        continue;
                    }
                    if (cookie->evtype == XI_RawButtonRelease) {
                        button_pressed = 0;
                        redraw();
                        continue;
                    }
                    continue;
                }

                if (ev.type == KeyPress) {
                    KeySym keysym = XLookupKeysym(&ev.xkey, 0);
                    if (keysym != NoSymbol) {
                        handle_key(keysym, ev.xkey.state);
                    }
                    continue;
                }
                if (ev.type == Expose) {
                    if (ev.xexpose.count < 1) {
                        redraw();
                    }
                    continue;
                }
                if (ev.type == VisibilityNotify) {
                    /* needed to deal with menus, etc. overlapping the hightlight win */
                    XRaiseWindow(dpy, win);
                    continue;
                }
            }
        } else {
            if (options.auto_hide_cursor && cursor_visible) {
                hide_cursor();
            }
            if (options.auto_hide_highlight && highlight_visible) {
                hide_highlight();
            }
        }
    }
}

static int init_colors() {
    int res;

    Colormap colormap = DefaultColormap(dpy, screen);

    res = XAllocNamedColor(dpy, colormap, options.pressed_color_string, &pressed_color, &pressed_color);
    if (!res) {
        fprintf(stderr, "Can't allocate color: %s\n", options.pressed_color_string);
        return 1;
    }

    res = XAllocNamedColor(dpy, colormap, options.released_color_string, &released_color, &released_color);
    if (!res) {
        fprintf(stderr, "Can't allocate color: %s\n", options.released_color_string);
        return 1;
    }

    return 0;
}

static int grab_keys() {
    /* after https://git.suckless.org/dwm/file/dwm.c.html */
    numlockmask = 0;
    unsigned int numlockkeycode = XKeysymToKeycode(dpy, XK_Num_Lock);
    if (numlockkeycode) {
        XModifierKeymap* modmap = XGetModifierMapping(dpy);
        for (int i = 0; i < 8; ++i) {
            for (int j = 0; j < modmap->max_keypermod; ++j) {
                if (modmap->modifiermap[i * modmap->max_keypermod + j] == numlockkeycode) {
                    numlockmask = (1 << i);
                }
            }
        }
        XFreeModifiermap(modmap);
    }

    unsigned int modifiers[] = {0, LockMask, numlockmask, numlockmask | LockMask};
    for (int i = 0; i < KEY_ARRAY_SIZE; ++i) {
        if (keys[i].keysym != NoSymbol) {
            KeyCode c = XKeysymToKeycode(dpy, keys[i].keysym);
            if (!c) {
                fprintf(stderr, "Could not convert key to keycode\n");
                return 1;
            }
            for (int j = 0; j < 2; ++j) {
                XGrabKey(dpy, c, keys[i].modifiers | modifiers[j], root, 1, GrabModeAsync, GrabModeAsync);
            }
        }
    }
    return 0;
}

static void sig_handler(int sig) {
    (void)sig;
    quit();
}

static int parse_key(const char* s, int k) {
    keys[k].modifiers = 0;

    int i;
    while (s[0] != '\0' && s[1] == '-') {
        for (i = 0; i < KEY_MODMAP_SIZE; ++i) {
            if (key_modifier_mapping[i].symbol == s[0]) {
                keys[k].modifiers |= key_modifier_mapping[i].modifiers;
                break;
            }
        }
        if (i == KEY_MODMAP_SIZE) {
            return 1;
        }
        s += 2;
    }

    keys[k].keysym = XStringToKeysym(s);
    if (keys[k].keysym == NoSymbol) {
        return 1;
    }
    return 0;
}

static void print_usage(const char* name) {
    printf(
        "Usage:\n"
        "  %s [options]\n"
        "\n"
        "  -h, --help      show this help message\n"
        "\n"
        "DISPLAY OPTIONS\n"
        "  -c, --released-color COLOR  dot color when mouse button released [default: #d62728]\n"
        "  -p, --pressed-color COLOR   dot color when mouse button pressed [default: #1f77b4]\n"
        "  -o, --outline OUTLINE       line width of outline or 0 for filled dot [default: 0]\n"
        "  -r, --radius RADIUS         dot radius in pixels [default: 5]\n"
        "      --opacity OPACITY       window opacity (0.0 - 1.0) [default: 1.0]\n"
        "      --hide-highlight        start with highlighter hidden\n"
        "      --show-cursor           start with cursor shown\n"
        "\n"
        "TIMEOUT OPTIONS\n"
        "      --auto-hide-cursor      hide cursor when not moving after timeout\n"
        "      --auto-hide-highlight   hide highlighter when not moving after timeout\n"
        "  -t, --hide-timeout TIMEOUT  timeout for hiding when idle, in seconds [default: 3]\n"
        "\n"
        "HOTKEY OPTIONS\n"
        "      --key-quit KEY                        quit\n"
        "      --key-toggle-cursor KEY               toggle cursor visibility\n"
        "      --key-toggle-highlight KEY            toggle highlight visibility\n"
        "      --key-toggle-auto-hide-cursor KEY     toggle auto-hiding cursor when not moving\n"
        "      --key-toggle-auto-hide-highlight KEY  toggle auto-hiding highlight when not moving\n"
        "\n"
        "      Hotkeys are global and can only be used if not set yet by a different process.\n"
        "      Keys can be given with modifiers\n"
        "        'S' (shift key), 'C' (ctrl key), 'M' (alt/meta key), 'H' (super/\"windows\" key)\n"
        "      delimited by a '-'.\n"
        "      Keys themselves are parsed by X, so chars like a...z can be set directly,\n"
        "      special keys are named as in /usr/include/X11/keysymdef.h\n"
        "      or see, e.g. http://xahlee.info/linux/linux_show_keycode_keysym.html\n"
        "\n"
        "      Examples: 'H-Left', 'C-S-a'\n",
        name);
}

static struct option long_options[] = {{"auto-hide-cursor", no_argument, &options.auto_hide_cursor, 1},
                                       {"auto-hide-highlight", no_argument, &options.auto_hide_highlight, 1},
                                       {"help", no_argument, NULL, 'h'},
                                       {"hide-highlight", no_argument, &options.highlight_visible, 0},
                                       {"hide-timeout", required_argument, NULL, 't'},
                                       {"outline", required_argument, NULL, 'o'},
                                       {"pressed-color", required_argument, NULL, 'p'},
                                       {"opacity", required_argument, NULL, 'a'},
                                       {"radius", required_argument, NULL, 'r'},
                                       {"released-color", required_argument, NULL, 'c'},
                                       {"show-cursor", no_argument, &options.cursor_visible, 1},
                                       {"key-quit", required_argument, NULL, KEY_QUIT + KEY_OPTION_OFFSET},
                                       {"key-toggle-cursor", required_argument, NULL, KEY_TOGGLE_CURSOR + KEY_OPTION_OFFSET},
                                       {"key-toggle-highlight", required_argument, NULL, KEY_TOGGLE_HIGHLIGHT + KEY_OPTION_OFFSET},
                                       {"key-toggle-auto-hide-cursor", required_argument, NULL, KEY_TOGGLE_AUTOHIDE_CURSOR + KEY_OPTION_OFFSET},
                                       {"key-toggle-auto-hide-highlight", required_argument, NULL, KEY_TOGGLE_AUTOHIDE_HIGHLIGHT + KEY_OPTION_OFFSET},
                                       {NULL, 0, NULL, 0}};

static int set_options(int argc, char* argv[]) {
    options.auto_hide_cursor = 0;
    options.auto_hide_highlight = 0;
    options.cursor_visible = 0;
    options.highlight_visible = 1;
    options.opacity = 1.0;
    options.radius = 5;
    options.outline = 0;
    options.hide_timeout = 3;
    options.pressed_color_string = "#1f77b4";
    options.released_color_string = "#d62728";

    while (1) {
        int c = getopt_long(argc, argv, "c:ho:p:r:t:", long_options, NULL);
        if (c < 0) {
            break;
        }
        if (c >= KEY_OPTION_OFFSET && c < KEY_OPTION_OFFSET + KEY_ARRAY_SIZE) {
            int res = parse_key(optarg, c - KEY_OPTION_OFFSET);
            if (res) {
                fprintf(stderr, "Could not parse key value %s\n", optarg);
                return 1;
            }
            continue;
        }
        switch (c) {
            case 0:
                break;
            case 'a':
                options.opacity = atof(optarg);
                break;
            case 'c':
                options.released_color_string = optarg;
                break;

            case 'h':
                print_usage(argv[0]);
                return -1;

            case 'o':
                options.outline = atoi(optarg);
                if (options.outline < 0) {
                    fprintf(stderr, "Invalid outline value %s\n", optarg);
                    return 1;
                }
                break;

            case 'p':
                options.pressed_color_string = optarg;
                break;

            case 'r':
                options.radius = atoi(optarg);
                if (options.radius <= 0) {
                    fprintf(stderr, "Invalid radius value %s\n", optarg);
                    return 1;
                }
                break;

            case 't':
                options.hide_timeout = atoi(optarg);
                if (options.hide_timeout <= 0) {
                    fprintf(stderr, "Invalid timeout value %s\n", optarg);
                    return 1;
                }
                break;

            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    return 0;
}

static int xerror_handler(Display* dpy_p, XErrorEvent* err) {
    if (err->request_code == 33 /* XGrabKey */ && err->error_code == BadAccess) {
        fprintf(stderr, "Key combination already grabbed by a different process\n");
        exit(1);
    }
    if (err->error_code == BadAtom) {
        fprintf(stderr, "X warning: BadAtom for %d-%d\n", err->request_code, err->minor_code);
        return 0;
    }
    char buf[1024];
    XGetErrorText(dpy_p, err->error_code, buf, 1024);
    fprintf(stderr, "X error: %s\n", buf);
    exit(1);
}

int main(int argc, char* argv[]) {
    int res;

    res = set_options(argc, argv);
    if (res < 0) {
        return 0;
    } else if (res > 0) {
        return res;
    }

    dpy = XOpenDisplay(NULL); /* defaults to DISPLAY env var */
    if (!dpy) {
        fprintf(stderr, "Can't open display\n");
        return 1;
    }
    XSetErrorHandler(xerror_handler);
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    int event, error, opcode;
    if (!XShapeQueryExtension(dpy, &event, &error)) {
        fprintf(stderr, "XShape extension not supported\n");
        return 1;
    }

    if (!XQueryExtension(dpy, "XInputExtension", &opcode, &event, &error)) {
        fprintf(stderr, "XInput extension not supported\n");
        return 1;
    }

    int major_version = 2;
    int minor_version = 2;
    res = XIQueryVersion(dpy, &major_version, &minor_version);
    if (res == BadRequest) {
        fprintf(stderr, "XInput2 extension version 2.2 not supported\n");
        return 1;
    } else if (res != Success) {
        fprintf(stderr, "Can't query XInput version\n");
        return 1;
    }

    res = init_window();
    if (res) {
        return res;
    }

    res = init_events();
    if (res) {
        return res;
    }

    res = init_colors();
    if (res) {
        return res;
    }

    res = grab_keys();
    if (res) {
        return res;
    }

    XAllowEvents(dpy, SyncBoth, CurrentTime);
    XSync(dpy, False);

    if (options.highlight_visible) {
        show_highlight();
    }

    if (!options.cursor_visible) {
        hide_cursor();
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    main_loop();

    if (!cursor_visible) {
        show_cursor();
    }
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    XUnmapWindow(dpy, win);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    return 0;
}
