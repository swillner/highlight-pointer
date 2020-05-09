/*
  highlight-ponter

  Highlight mouse pointer/cursor using a dot - useful for
  presentations, screen sharing, ...

  MIT License

  Copyright (c) 2020 Sven Willner <sven.willner@gmail.com>

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
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#define TARGET_FPS 0

static Display* dpy;
static GC gc = 0;
static Window win;
static int screen;
static int selfpipe[2]; /* for self-pipe trick to cancel select() call */

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
} options;

static void redraw();
static int get_pointer_position(int* x, int* y);

void show_cursor() {
    XFixesShowCursor(dpy, win);
    cursor_visible = 1;
}

void hide_cursor() {
    XFixesHideCursor(dpy, win);
    cursor_visible = 0;
}

void show_highlight() {
    int x, y;
    get_pointer_position(&x, &y);
    XMoveWindow(dpy, win, x - options.radius - 1, y - options.radius - 1);
    XMapWindow(dpy, win);
    redraw();
    highlight_visible = 1;
}

void hide_highlight() {
    XUnmapWindow(dpy, win);
    highlight_visible = 0;
}

int init_events() {
    XIEventMask events;
    unsigned char mask[(XI_LASTEVENT + 7) / 8];
    memset(mask, 0, sizeof(mask));

    XISetMask(mask, XI_RawButtonPress);
    XISetMask(mask, XI_RawButtonRelease);
    XISetMask(mask, XI_RawMotion);

    events.deviceid = XIAllMasterDevices;
    events.mask = mask;
    events.mask_len = sizeof(mask);

    XISelectEvents(dpy, RootWindow(dpy, screen), &events, 1);

    return 0;
}

int get_pointer_position(int* x, int* y) {
    Window w;
    int i;
    unsigned int ui;
    return XQueryPointer(dpy, XRootWindow(dpy, screen), &w, &w, x, y, &i, &i, &ui);
}

void set_window_mask() {
    XGCValues gc_values;
    Pixmap mask = XCreatePixmap(dpy, win, 2 * options.radius + 2, 2 * options.radius + 2, 1);
    GC mask_gc = XCreateGC(dpy, mask, 0, &gc_values);
    XSetForeground(dpy, mask_gc, 0);
    XFillRectangle(dpy, mask, mask_gc, 0, 0, 2 * options.radius + 2, 2 * options.radius + 2);

    XSetForeground(dpy, mask_gc, 1);
    if (options.outline) {
        XSetLineAttributes(dpy, mask_gc, options.outline, LineSolid, CapButt, JoinBevel);
        XDrawArc(dpy, mask, mask_gc, 0, 0, 2 * options.radius + 1, 2 * options.radius + 1, 0, 360 * 64);
    } else {
        XFillArc(dpy, mask, mask_gc, 0, 0, 2 * options.radius + 1, 2 * options.radius + 1, 0, 360 * 64);
    }

    XShapeCombineMask(dpy, win, ShapeBounding, 0, 0, mask, ShapeSet);

    XFreeGC(dpy, mask_gc);
    XFreePixmap(dpy, mask);
}

int init_window() {
    XSetWindowAttributes win_attributes;
    win_attributes.event_mask = ExposureMask | VisibilityChangeMask;
    win_attributes.override_redirect = True;

    win = XCreateWindow(dpy, XRootWindow(dpy, screen), 0, 0, 2 * options.radius + 2, 2 * options.radius + 2, 0, DefaultDepth(dpy, screen), InputOutput,
                        DefaultVisual(dpy, screen), CWEventMask | CWOverrideRedirect, &win_attributes);
    if (!win) {
        fprintf(stderr, "Can't create highlight window\n");
        return 1;
    }

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
    XSendEvent(dpy, XRootWindow(dpy, screen), False, SubstructureRedirectMask | SubstructureNotifyMask, (XEvent*)&xclient);

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

void redraw() {
    XSetForeground(dpy, gc, button_pressed ? pressed_color.pixel : released_color.pixel);
    if (options.outline) {
        XSetLineAttributes(dpy, gc, options.outline, LineSolid, CapButt, JoinBevel);
        XDrawArc(dpy, win, gc, 0, 0, 2 * options.radius + 1, 2 * options.radius + 1, 0, 360 * 64);
    } else {
        XFillArc(dpy, win, gc, 0, 0, 2 * options.radius + 1, 2 * options.radius + 1, 0, 360 * 64);
    }
}

void quit() { write(selfpipe[1], "\0", 1); }

void main_loop() {
    XEvent ev;
    fd_set fds;
    int fd = ConnectionNumber(dpy);
    struct timeval timeout;
    int x, y, n;
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
                        if (options.auto_hide_cursor && !cursor_visible) {
                            show_cursor();
                        }
                        if (options.auto_hide_highlight && !highlight_visible) {
                            show_highlight();
                        } else if (highlight_visible) {
                            get_pointer_position(&x, &y);
                            XMoveWindow(dpy, win, x - options.radius - 1, y - options.radius - 1);
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

                if (ev.type == Expose) {
                    redraw();
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

int init_colors() {
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

void sig_handler(int sig) {
    (void)sig;
    fprintf(stderr, "Quitting...\n");
    quit();
}

int main(int argc, char* argv[]) {
    int res;


    // TODO parse arguments
    options.radius = 5;
    options.outline = 0;
    options.cursor_visible = 1;
    options.highlight_visible = 1;
    options.auto_hide_cursor = 0;
    options.auto_hide_highlight = 1;
    options.hide_timeout = 3;
    options.pressed_color_string = "#0000ff";
    options.released_color_string = "#ff0000";

    dpy = XOpenDisplay(NULL); /* defaults to DISPLAY env var */
    if (!dpy) {
        fprintf(stderr, "Can't open display");
        return 1;
    }
    screen = DefaultScreen(dpy);

    int event, error, opcode;
    if (!XShapeQueryExtension(dpy, &event, &error)) {
        fprintf(stderr, "XShape extension not supported");
        return 1;
    }

    if (!XQueryExtension(dpy, "XInputExtension", &opcode, &event, &error)) {
        fprintf(stderr, "XInput extension not supported");
        return 1;
    }

    int major_version = 2;
    int minor_version = 2;
    res = XIQueryVersion(dpy, &major_version, &minor_version);
    if (res == BadRequest) {
        fprintf(stderr, "XInput2 extension version 2.2 not supported");
        return 1;
    } else if (res != Success) {
        fprintf(stderr, "Can't query XInput version");
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
    XUnmapWindow(dpy, win);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    return 0;
}
