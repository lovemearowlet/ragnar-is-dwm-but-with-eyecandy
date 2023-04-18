#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xcursor/Xcursor.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <err.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"

#define CLIENT_WINDOW_CAP 256

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

typedef enum {
    WINDOW_LAYOUT_TILED_MASTER = 0,
    WINDOW_LAYOUT_FLOATING
} WindowLayout;

typedef struct {
    float x, y;
} Vec2;

typedef struct {
    Window win;
    Window frame;

    bool fullscreen;
    Vec2 fullscreen_revert_size;
    uint8_t monitor_index;

    bool in_layout;
} Client;

typedef struct {
    Display* display;
    Window root;
    uint32_t focused_monitor;

    bool running;

    Client client_windows[CLIENT_WINDOW_CAP];
    uint32_t clients_count;

    Vec2 cursor_start_pos, 
        cursor_start_frame_pos, 
        cursor_start_frame_size;

    WindowLayout current_layout;
    uint32_t layout_master_size_x[MONITOR_COUNT];
    uint32_t window_gap;
} XWM;


static bool wm_detected = false;
static XWM wm;

static Window   get_frame_window(Window win);
static void     handle_create_notify(XCreateWindowEvent e);
static void     handle_reparent_notify(XReparentEvent e);
static void     handle_map_notify(XMapEvent e);
static void     handle_unmap_notify(XUnmapEvent e);
static void     handle_configure_request(XConfigureRequestEvent e);
static void     handle_configure_notify(XConfigureEvent e);
static void     handle_motion_notify(XMotionEvent e);
static void     handle_map_request(XMapRequestEvent e);
static void     handle_destroy_notify(XDestroyWindowEvent e);
static int      handle_x_error(Display* display, XErrorEvent* e);
static int      handle_wm_detected(Display* display, XErrorEvent* e);
static void     handle_button_press(XButtonEvent e);
static void     handle_button_release(XButtonEvent e);
static void     handle_key_press(XKeyEvent e);
static void     handle_key_release(XKeyEvent e);
static void     grab_global_input();
static void     grab_window_input(Window win);

static Vec2     get_cursor_position();

static void     select_focused_monitor(uint32_t x_cursor);
static int32_t  get_monitor_index_by_x_pos(int32_t xpos);
static int32_t  get_focused_monitor_window_center_x(int32_t window_x);
static int32_t  get_monitor_start_x(int32_t monitor);

static void     set_fullscreen(Window win);
static void     unset_fullscreen(Window win);

static void     establish_window_layout();
static void     move_client_up_layout(Client* client);
static void     move_client_down_layout(Client* client);

static void     move_client(Client* client, Vec2 pos);
static void     resize_client(Client* client, Vec2 size);
static int32_t  get_client_index_window(Window win);
static int32_t  get_client_index(Client client);


XWM xwm_init() {
    XWM wm;

    wm.display = XOpenDisplay(NULL);
    if(!wm.display) {
        printf("Failed to open X Display.\n");
    }
    wm.root = DefaultRootWindow(wm.display);

    return wm;
}

void xwm_terminate() {
    XCloseDisplay(wm.display);
}

void xwm_window_frame(Window win, bool created_before_window_manager) {
    XWindowAttributes attribs;
    XGetWindowAttributes(wm.display, win, &attribs);
    
    // Do not frame 'daemon' windows
    if(created_before_window_manager) {
        if(attribs.override_redirect || attribs.map_state != IsViewable) {
            return;
        }
    }
    // Calculate X Position of window based on the currently focused monitor
    int32_t win_x = get_focused_monitor_window_center_x(attribs.width / 2);

    Window win_frame = XCreateSimpleWindow(wm.display, wm.root, 
        win_x, 
        (Monitors[wm.focused_monitor].height / 2) - (attribs.height / 2), attribs.width, attribs.height, 
        WINDOW_BORDER_WIDTH,  WINDOW_BORDER_COLOR, WINDOW_BG_COLOR);

    XSelectInput(wm.display, win_frame, SubstructureRedirectMask | SubstructureNotifyMask); 
    XReparentWindow(wm.display, win, win_frame, 0, 0);
    XMapWindow(wm.display, win_frame);

    // Adding the window to the clients 
    wm.client_windows[wm.clients_count++] = (Client){.frame = win_frame, .win =  win, 
        .fullscreen = attribs.width >= (int32_t)Monitors[wm.focused_monitor].width && attribs.height >= (int32_t)Monitors[wm.focused_monitor].height, 
        .monitor_index = wm.focused_monitor,
        .in_layout = true};
    
    grab_window_input(win);
    establish_window_layout();
}

void xwm_window_unframe(Window win) {
    Window frame_win = get_frame_window(win);

    // If master window of layout was unframed, reset master size
    if(wm.client_windows[get_client_index_window(win)].in_layout && get_client_index_window(win) == 0) {
        wm.layout_master_size_x[wm.focused_monitor] = 0;
    }
    XUnmapWindow(wm.display, frame_win);
    XDestroyWindow(wm.display, frame_win);
    
    // Removing the window from the clients
    for(uint32_t i = get_client_index_window(win); i < wm.clients_count - 1; i++)
        wm.client_windows[i] = wm.client_windows[i + 1];
    wm.clients_count--;

    XSetInputFocus(wm.display, wm.root, RevertToParent, CurrentTime);
    establish_window_layout();
}
void xwm_run() {
    // Setting variables to default state 
    wm.clients_count = 0;
    wm.cursor_start_frame_size = (Vec2){ .x = 0.0f, .y = 0.0f};
    wm.cursor_start_frame_pos = (Vec2){ .x = 0.0f, .y = 0.0f};
    wm.cursor_start_pos = (Vec2){ .x = 0.0f, .y = 0.0f}; 
    wm.running = true;
    wm.focused_monitor = MONITOR_COUNT - 1;
    wm.current_layout = WINDOW_LAYOUT_TILED_MASTER;
    wm.window_gap = 10;
    memset(wm.layout_master_size_x, 0, sizeof(wm.layout_master_size_x));
    
    XSetErrorHandler(handle_wm_detected);
    XSelectInput(wm.display, wm.root, SubstructureRedirectMask | SubstructureNotifyMask | KeyPressMask | ButtonPressMask); 
    XSync(wm.display, false);
    if(wm_detected) {
        printf("Another window manager is already running on this X display.\n");
        return;
    }
    
    Cursor cursor = XcursorLibraryLoadCursor(wm.display, "arrow");
    XDefineCursor(wm.display, wm.root, cursor);
    XSetErrorHandler(handle_x_error);

    grab_global_input();
     
    while(wm.running) {
        // Query mouse position to get focused monitor
        select_focused_monitor(get_cursor_position().x);

        XEvent e;
        XNextEvent(wm.display, &e);
        switch (e.type) {
            case MapNotify:
                handle_map_notify(e.xmap);
                break;
            case UnmapNotify:
                handle_unmap_notify(e.xunmap);
                break;
            case MapRequest:
                handle_map_request(e.xmaprequest);
                break;
            case ConfigureRequest:
                handle_configure_request(e.xconfigurerequest);
                break;
            case ConfigureNotify:
                handle_configure_notify(e.xconfigure);
                break;
            case CreateNotify:
                handle_create_notify(e.xcreatewindow);
                break;
            case ReparentNotify:
                handle_reparent_notify(e.xreparent);
                break;
            case DestroyNotify:
                handle_destroy_notify(e.xdestroywindow);
                break;
            case ButtonPress:
                handle_button_press(e.xbutton);
                break;
            case ButtonRelease:
                handle_button_release(e.xbutton);
                break;
            case KeyPress:
                handle_key_press(e.xkey);
                break;
            case KeyRelease:
                handle_key_release(e.xkey);
                break;
            case MotionNotify:
                while(XCheckTypedWindowEvent(wm.display, e.xmotion.window, MotionNotify, &e)) {}
                handle_motion_notify(e.xmotion);
                break;
        }
    }
}

Window get_frame_window(Window win) {
    for(uint32_t i = 0; i < wm.clients_count; i++) {
        if(wm.client_windows[i].win == win || wm.client_windows[i].frame == win) {
            return wm.client_windows[i].frame;
        }
    }
    return 0;
}
void handle_create_notify(XCreateWindowEvent e) {
   (void)e; 
}
void handle_configure_request(XConfigureRequestEvent e) {
    XWindowChanges changes;
    changes.x = e.x;
    changes.y = e.y;
    changes.width = e.width;
    changes.height = e.height;
    changes.border_width = e.border_width;
    changes.sibling = e.above;
    changes.stack_mode = e.detail;
    if(get_client_index_window(e.window) == -1) {
        Window frame_win = get_frame_window(e.window);
        XConfigureWindow(wm.display,frame_win, e.value_mask, &changes);
    }
    XConfigureWindow(wm.display, e.window, e.value_mask, &changes);
}

void handle_configure_notify(XConfigureEvent e) {
    (void)e;
}
void handle_motion_notify(XMotionEvent e) {
    if(get_client_index_window(e.window) == -1) return;
    select_focused_monitor(e.x_root);

    Vec2 drag_pos = (Vec2){.x = (float)e.x_root, .y = (float)e.y_root};
    Vec2 delta_drag = (Vec2){.x = drag_pos.x - wm.cursor_start_pos.x, .y = drag_pos.y - wm.cursor_start_pos.y};
    
    Client* client = &wm.client_windows[get_client_index_window(e.window)];
    if(e.state & Button1Mask) {
        Vec2 drag_dest = (Vec2){.x = (float)(wm.cursor_start_frame_pos.x + delta_drag.x), 
            .y = (float)(wm.cursor_start_frame_pos.y + delta_drag.y)};
        // Unset fullscreen of the client
        if(client->fullscreen) {
            client->fullscreen = false;
            XSetWindowBorderWidth(wm.display, client->frame, WINDOW_BORDER_WIDTH);
        } 
        move_client(client, drag_dest);
        // Remove the client from the layout 
        if(client->in_layout) {
            client->in_layout = false;
            establish_window_layout();
        }
        client->monitor_index = get_monitor_index_by_x_pos(drag_dest.x); 
    } else if(e.state & Button3Mask) {
        Vec2 resize_delta = (Vec2){.x = MAX(delta_drag.x, -wm.cursor_start_frame_size.x),
                                    .y = MAX(delta_drag.y, -wm.cursor_start_frame_size.y)};
        Vec2 resize_dest = (Vec2){.x = wm.cursor_start_frame_size.x + resize_delta.x, 
                                    .y = wm.cursor_start_frame_size.y + resize_delta.y};
        resize_client(client, resize_dest);
        // Remove the client from the layout
        if(client->in_layout) {
            client->in_layout = false;
            establish_window_layout();
        }
    }
}

void handle_map_request(XMapRequestEvent e) {
    xwm_window_frame(e.window, false);
    XMapWindow(wm.display, e.window);
}
int handle_x_error(Display* display, XErrorEvent* e) {
    (void)display;
    char err_msg[1024];
    XGetErrorText(display, e->error_code, err_msg, sizeof(err_msg));
    printf("X Error:\n\tRequest: %i\n\tError Code: %i - %s\n\tResource ID: %i\n", 
           e->request_code, e->error_code, err_msg, (int)e->resourceid); 
    return 0;  
}

int handle_wm_detected(Display* display, XErrorEvent* e) {
    (void)display;
    wm_detected = ((int32_t)e->error_code != BadAccess);
    return 0;
}

void handle_reparent_notify(XReparentEvent e) {
    (void)e;
}

void handle_destroy_notify(XDestroyWindowEvent e) {
    (void)e;
}
void handle_map_notify(XMapEvent e) {
    (void)e;
}
void handle_unmap_notify(XUnmapEvent e) {
    if(get_client_index_window(e.window) == -1) {
        return;
    }
    xwm_window_unframe(e.window);
}

void handle_button_press(XButtonEvent e) {
    Window frame = get_frame_window(e.window);
    wm.cursor_start_pos = (Vec2){.x = (float)e.x_root, .y = (float)e.y_root};
    Window root;
    int32_t x, y;
    unsigned width, height, border_width, depth;
    XGetGeometry(wm.display, frame, &root, &x, &y, &width, &height, &border_width, &depth);
    wm.cursor_start_frame_pos = (Vec2){.x = (float)x, .y = (float)y};
    wm.cursor_start_frame_size = (Vec2){.x = (float)width, .y = (float)height};

    XRaiseWindow(wm.display, frame);
    XSetInputFocus(wm.display, e.window, RevertToParent, CurrentTime);
}
void handle_button_release(XButtonEvent e) {
    (void)e;
}

void handle_key_press(XKeyEvent e) {
    if(e.state & MASTER_KEY && e.keycode == XKeysymToKeycode(wm.display, WINDOW_CLOSE_KEY)) {
        XEvent msg;
        memset(&msg, 0, sizeof(msg));
        msg.xclient.type = ClientMessage;
        msg.xclient.message_type =  XInternAtom(wm.display, "WM_PROTOCOLS", false);
        msg.xclient.window = e.window;
        msg.xclient.format = 32;
        msg.xclient.data.l[0] = XInternAtom(wm.display, "WM_DELETE_WINDOW", false);
        XSendEvent(wm.display, e.window, false, 0, &msg);
    } else if(e.state & MASTER_KEY && e.keycode == XKeysymToKeycode(wm.display, WINDOW_CYCLE_KEY)) {
        Client client = { 0 };
        // Get the next client in the cycle
        for(uint32_t i = 0; i < wm.clients_count; i++) {
            if(wm.client_windows[i].win == e.window ||
                wm.client_windows[i].frame == e.window) {
                if(i + 1 >= wm.clients_count) {
                    client = wm.client_windows[0];
                } else {
                    client = wm.client_windows[i + 1];
                }
                break;
            }
        }
        XRaiseWindow(wm.display, client.frame);
        XSetInputFocus(wm.display, client.win, RevertToParent, CurrentTime);
    } else if(e.state & MASTER_KEY && e.keycode == XKeysymToKeycode(wm.display, WM_TERMINATE_KEY)) {
        wm.running = false;
    } else if(e.state & MASTER_KEY && e.keycode == XKeysymToKeycode(wm.display, TERMINAL_OPEN_KEY)) {
        system(TERMINAL_CMD);  
    } else if(e.state & MASTER_KEY && e.keycode == XKeysymToKeycode(wm.display, WEB_BROWSER_OPEN_KEY)) {
        system(WEB_BROWSER_CMD);   
    } else if(e.state & MASTER_KEY && e.keycode == XKeysymToKeycode(wm.display, WINDOW_FULLSCREEN_KEY)) {
        if(get_client_index_window(e.window) == -1) return;
        if(!wm.client_windows[get_client_index_window(e.window)].fullscreen) {
            set_fullscreen(e.window);
        } else {
            unset_fullscreen(e.window);
            establish_window_layout();
        }
    } else if(e.state & MASTER_KEY && e.keycode == XKeysymToKeycode(wm.display, WINDOW_ADD_TO_LAYOUT_KEY)) {
        wm.client_windows[get_client_index_window(e.window)].in_layout = true;
        establish_window_layout();
    } else if(e.state & MASTER_KEY && e.keycode == XKeysymToKeycode(wm.display, WINDOW_LAYOUT_MOVE_UP_KEY)) {
        move_client_up_layout(&wm.client_windows[get_client_index_window(e.window)]);
    } else if(e.state & MASTER_KEY && e.keycode == XKeysymToKeycode(wm.display, WINDOW_LAYOUT_MOVE_DOWN_KEY)) {
        move_client_down_layout(&wm.client_windows[get_client_index_window(e.window)]);
    } else if(e.state & MASTER_KEY && e.keycode == XKeysymToKeycode(wm.display, WINDOW_LAYOUT_INCREASE_MASTER_X_KEY)) {
        if(wm.layout_master_size_x[wm.focused_monitor] <= (uint32_t)(Monitors[wm.focused_monitor].width / 1.5)) 
            wm.layout_master_size_x[wm.focused_monitor] += 20;
        establish_window_layout();
        
    } else if(e.state & MASTER_KEY && e.keycode == XKeysymToKeycode(wm.display, WINDOW_LAYOUT_DECREASE_MASTER_X_KEY)) {
        if(wm.layout_master_size_x[wm.focused_monitor] >= (uint32_t)(Monitors[wm.focused_monitor].width / 6)) 
            wm.layout_master_size_x[wm.focused_monitor] -= 20;
        establish_window_layout();
    } else if(e.state & (MASTER_KEY | ShiftMask) && e.keycode == XKeysymToKeycode(wm.display, WINDOW_LAYOUT_TILED_MASTER_KEY)) {
        wm.current_layout = WINDOW_LAYOUT_TILED_MASTER;
    } else if(e.state & (MASTER_KEY | ShiftMask) && e.keycode == XKeysymToKeycode(wm.display, WINDOW_LAYOUT_FLOATING_KEY)) {
        wm.current_layout = WINDOW_LAYOUT_FLOATING;
    }else if(e.state & (MASTER_KEY) && e.keycode == XKeysymToKeycode(wm.display, WINDOW_GAP_INCREASE_KEY)) {
        wm.window_gap += 2;
        establish_window_layout();
    }else if(e.state & (MASTER_KEY) && e.keycode == XKeysymToKeycode(wm.display, WINDOW_GAP_DECREASE_KEY)) {
        wm.window_gap -= 2;
        establish_window_layout();
    }


}
void handle_key_release(XKeyEvent e) {
    (void)e;
}

int32_t get_client_index_window(Window win) {
    for(uint32_t i = 0; i < wm.clients_count; i++) {
        if(wm.client_windows[i].win == win || wm.client_windows[i].frame == win) return i;
    }
    return -1;
}

int32_t get_client_index(Client client) {
    for(uint32_t i = 0; i < wm.clients_count; i++) {
        if(wm.client_windows[i].win == client.win && wm.client_windows[i].frame == client.frame) return i;
    }
    return -1;
}

static void grab_global_input() {     
    XGrabKey(wm.display,XKeysymToKeycode(wm.display, WM_TERMINATE_KEY),MASTER_KEY, wm.root,false, GrabModeAsync,GrabModeAsync);
    XGrabKey(wm.display,XKeysymToKeycode(wm.display, TERMINAL_OPEN_KEY),MASTER_KEY, wm.root,false, GrabModeAsync,GrabModeAsync);
    XGrabKey(wm.display,XKeysymToKeycode(wm.display, WEB_BROWSER_OPEN_KEY),MASTER_KEY, wm.root, false, GrabModeAsync,GrabModeAsync);

}
static void grab_window_input(Window win) {
    XGrabButton(wm.display,Button1,MASTER_KEY,win,false,ButtonPressMask | ButtonReleaseMask | ButtonMotionMask,
                GrabModeAsync,GrabModeAsync,None,None);
    XGrabButton(wm.display,Button3,MASTER_KEY,win,false,ButtonPressMask | ButtonReleaseMask | ButtonMotionMask,
                GrabModeAsync,GrabModeAsync,None,None);
    XGrabButton(wm.display,Button2,MASTER_KEY,win,false,ButtonPressMask | ButtonReleaseMask | ButtonMotionMask,
                GrabModeAsync,GrabModeAsync,None,None);
    XGrabKey(wm.display, XKeysymToKeycode(wm.display, WINDOW_CLOSE_KEY),MASTER_KEY, win,false, GrabModeAsync, GrabModeAsync);
    XGrabKey(wm.display,XKeysymToKeycode(wm.display, WINDOW_CYCLE_KEY),MASTER_KEY,win,false, GrabModeAsync,GrabModeAsync);
    XGrabKey(wm.display,XKeysymToKeycode(wm.display, WINDOW_FULLSCREEN_KEY),MASTER_KEY,win,false, GrabModeAsync,GrabModeAsync);
    XGrabKey(wm.display,XKeysymToKeycode(wm.display, WINDOW_ADD_TO_LAYOUT_KEY),MASTER_KEY,win,false, GrabModeAsync,GrabModeAsync);
    XGrabKey(wm.display,XKeysymToKeycode(wm.display, WINDOW_LAYOUT_MOVE_UP_KEY),MASTER_KEY,win,false, GrabModeAsync,GrabModeAsync);
    XGrabKey(wm.display,XKeysymToKeycode(wm.display, WINDOW_LAYOUT_MOVE_DOWN_KEY),MASTER_KEY,win,false, GrabModeAsync,GrabModeAsync);
    XGrabKey(wm.display,XKeysymToKeycode(wm.display, WINDOW_LAYOUT_DECREASE_MASTER_X_KEY),MASTER_KEY,win,false, GrabModeAsync,GrabModeAsync);
    XGrabKey(wm.display,XKeysymToKeycode(wm.display, WINDOW_LAYOUT_INCREASE_MASTER_X_KEY),MASTER_KEY,win,false, GrabModeAsync,GrabModeAsync);
    XGrabKey(wm.display,XKeysymToKeycode(wm.display, WINDOW_LAYOUT_TILED_MASTER_KEY),MASTER_KEY | ShiftMask,win,false, GrabModeAsync,GrabModeAsync);
    XGrabKey(wm.display,XKeysymToKeycode(wm.display, WINDOW_LAYOUT_FLOATING_KEY),MASTER_KEY | ShiftMask,win,false, GrabModeAsync,GrabModeAsync);
    XGrabKey(wm.display,XKeysymToKeycode(wm.display, WINDOW_GAP_INCREASE_KEY),MASTER_KEY,win,false, GrabModeAsync,GrabModeAsync);
    XGrabKey(wm.display,XKeysymToKeycode(wm.display, WINDOW_GAP_DECREASE_KEY),MASTER_KEY,win,false, GrabModeAsync,GrabModeAsync);
}

void select_focused_monitor(uint32_t x_cursor) {
    uint32_t x_offset = 0;
    for(uint32_t i = 0; i < MONITOR_COUNT; i++) {
        if(x_cursor >= x_offset && x_cursor <= Monitors[i].width + x_offset) {
            wm.focused_monitor = i;
            break;
        }
        x_offset += Monitors[i].width;
    }
}

int32_t get_monitor_index_by_x_pos(int32_t xpos) {
    for(int32_t i = 0; i < MONITOR_COUNT; i++) {
        if(xpos >= get_monitor_start_x(i) && xpos <= (int32_t)(get_monitor_start_x(i) + Monitors[i].width)) {
            return i;
        }
    }
    return -1;
}

Vec2 get_cursor_position() {
    Window root_return, child_return;
    int win_x_return, win_y_return, root_x_return, root_y_return; 
    uint32_t mask_return;
    XQueryPointer(wm.display, wm.root, &root_return, &child_return, &root_x_return, &root_y_return, 
        &win_x_return, &win_y_return, &mask_return);
    return (Vec2){.x = root_x_return, .y = root_y_return};  
}

int32_t get_focused_monitor_window_center_x(int32_t window_x) {
    int32_t x = 0;
    for(uint32_t i = 0; i < wm.focused_monitor + 1; i++) {
        if(i > 0) 
            x += Monitors[i - 1].width;
        if(i == wm.focused_monitor)
            x += Monitors[wm.focused_monitor].width / 2;
    }
    x -= window_x;
    return x;
}

int32_t get_monitor_start_x(int32_t monitor) {
    int32_t x = 0;
    for(int32_t i = 0; i < monitor; i++) {
        x += Monitors[i].width;    
    }
    return x;

}
static void set_fullscreen(Window win) {
    uint32_t client_index = get_client_index_window(win);
    XWindowAttributes attribs;
    XGetWindowAttributes(wm.display, wm.client_windows[client_index].win, &attribs);
    wm.client_windows[client_index].fullscreen_revert_size = (Vec2){.x = attribs.width, .y = attribs.height};
    wm.client_windows[client_index].fullscreen = true;
    XSetWindowBorderWidth(wm.display, wm.client_windows[client_index].frame, 0);

    resize_client(&wm.client_windows[client_index], (Vec2){. x = Monitors[wm.focused_monitor].width, .y = Monitors[wm.focused_monitor].height});
    move_client(&wm.client_windows[client_index], (Vec2){.x = get_monitor_start_x(wm.focused_monitor), 0});
}
static void unset_fullscreen(Window win)  {
    uint32_t client_index = get_client_index_window(win);
    resize_client(&wm.client_windows[client_index], wm.client_windows[client_index].fullscreen_revert_size);
    move_client(&wm.client_windows[client_index], (Vec2){get_monitor_start_x(wm.focused_monitor), 0});
    wm.client_windows[client_index].fullscreen = false;
    XSetWindowBorderWidth(wm.display, wm.client_windows[client_index].frame, WINDOW_BORDER_WIDTH);
}

void move_client(Client* client, Vec2 pos) {
    XMoveWindow(wm.display, client->frame, pos.x, pos.y);
}
void resize_client(Client* client, Vec2 size) {
    XResizeWindow(wm.display, client->win, size.x, size.y);
    XResizeWindow(wm.display, client->frame, size.x, size.y);
}

void establish_window_layout() {
    if(wm.current_layout == WINDOW_LAYOUT_FLOATING) return;

    // Retrieving the clients on the focused monitor
    Client* clients[CLIENT_WINDOW_CAP]; 
    uint32_t client_count = 0;
    for(uint32_t i = 0; i < wm.clients_count; i++) {
        if(wm.client_windows[i].monitor_index == wm.focused_monitor && wm.client_windows[i].in_layout) {
            clients[client_count++] = &wm.client_windows[i];
        }
    }
    if(client_count == 0) return;

    if(wm.current_layout == WINDOW_LAYOUT_TILED_MASTER) {
        // Default master size is half of the screen
        if(wm.layout_master_size_x[wm.focused_monitor] == 0) {
            wm.layout_master_size_x[wm.focused_monitor] = Monitors[wm.focused_monitor].width / 2;
        }
        Client* master = clients[0];

        // set master fullscreen if no slaves
        if(client_count == 1) {
            set_fullscreen(master->frame);
            return;
        }
            
        // set master
        move_client(master, (Vec2){get_monitor_start_x(wm.focused_monitor) + wm.window_gap, wm.window_gap});
        resize_client(master, (Vec2){
            .x = wm.layout_master_size_x[wm.focused_monitor] - wm.window_gap,
            .y = Monitors[wm.focused_monitor].height - (wm.window_gap * 2)});
        master->fullscreen = false;
        XSetWindowBorderWidth(wm.display, master->frame, WINDOW_BORDER_WIDTH);

        // set slaves
        for(uint32_t i = 1; i < client_count; i++) {
            if(clients[i]->monitor_index != wm.focused_monitor) continue;
            resize_client(clients[i], (Vec2){
                (Monitors[wm.focused_monitor].width - wm.layout_master_size_x[wm.focused_monitor]) - wm.window_gap * 2.5f,
                (int32_t)(Monitors[wm.focused_monitor].height / (client_count - 1)) - wm.window_gap * 2
            });

            move_client(clients[i], (Vec2){
                (get_monitor_start_x(wm.focused_monitor) + wm.layout_master_size_x[wm.focused_monitor]) + wm.window_gap,
                (Monitors[wm.focused_monitor].height - (int32_t)((Monitors[wm.focused_monitor].height / (client_count - 1) * i))) + wm.window_gap
            });
        }
    }    
}


void move_client_up_layout(Client* client) {
    int32_t client_index = get_client_index(*client);

    uint32_t upper_index = client_index + 1;
    if(client_index >= (int32_t)wm.clients_count - 1) {
        upper_index = 0;
    }

    Client tmp = wm.client_windows[client_index];
    wm.client_windows[client_index] = wm.client_windows[upper_index];
    wm.client_windows[upper_index] = tmp;
    establish_window_layout();
}

void move_client_down_layout(Client* client) {
    int32_t client_index = get_client_index(*client);
    if(client_index == -1) return;

    uint32_t lower_index = client_index - 1;
    if(client_index == 0) {
        lower_index = wm.clients_count - 1;
    }

    Client tmp = wm.client_windows[client_index];
    wm.client_windows[client_index] = wm.client_windows[lower_index];
    wm.client_windows[lower_index] = tmp;
    establish_window_layout();
}

int main(void) {
    wm = xwm_init();
    xwm_run();
    xwm_terminate();
}