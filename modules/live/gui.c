#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo.h>
#include <cairo-xlib.h>

#include <rsvg.h>

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

#include "../../mato/mato.h"
#include "core/config_mato.h"
#include "gui.h"

#define MAX_GUI_CONTEXTS_COUNT 10
#define MAX_GUI_WINDOWS_COUNT  20

static Display *dsp;
static Screen *scr;
static int screen;
static unsigned char x_opened;

static char *context_names[MAX_GUI_CONTEXTS_COUNT];
static gui_key_callback key_callbacks[MAX_GUI_CONTEXTS_COUNT];
static volatile int contexts_count;
static volatile int current_context;

static gui_draw_callback draw_callbacks[MAX_GUI_WINDOWS_COUNT];
static int window_in_use[MAX_GUI_WINDOWS_COUNT];
static int window_exposed[MAX_GUI_WINDOWS_COUNT];
static gui_mouse_callback mouse_callbacks[MAX_GUI_WINDOWS_COUNT];

static cairo_t *windows[MAX_GUI_WINDOWS_COUNT];
static cairo_surface_t *surfaces[MAX_GUI_WINDOWS_COUNT];
static Window x11windows[MAX_GUI_WINDOWS_COUNT];
static char *window_names[MAX_GUI_WINDOWS_COUNT];
static int window_fixed_width[MAX_GUI_WINDOWS_COUNT];
static int window_fixed_height[MAX_GUI_WINDOWS_COUNT];

static int window_update_periods[MAX_GUI_WINDOWS_COUNT];
static long long next_window_update[MAX_GUI_WINDOWS_COUNT];
static int async_repaint_request[MAX_GUI_WINDOWS_COUNT];

static pthread_mutex_t gui_lock;

void repaint_window(int window_handle)
{
    pthread_mutex_unlock(&gui_lock);
      if (draw_callbacks[window_handle])
          draw_callbacks[window_handle](windows[window_handle]);
    pthread_mutex_lock(&gui_lock);
}

void gui_fullscreen(Display* dpy, Window win)
{
  Atom atoms[2] = { XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False), None };
  XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_STATE", False),
                  XA_ATOM, 32, PropModeReplace, (unsigned char*) atoms, 1);
}

void gui_clear_window(cairo_surface_t *surf)
{
    cairo_t *win = cairo_create(surf);
    cairo_push_group(win);

    cairo_set_source_rgb(win, 1, 1, 1);
    cairo_paint(win);

    cairo_pop_group_to_source(win);
    cairo_paint(win);
    cairo_destroy(win);
}

int gui_cairo_check_event(int *xclick, int *yclick, int *win)
{
   char keybuf[20];
   KeySym key;
   XEvent e;

   for (;;)
   {
      if (XPending(dsp)) XNextEvent(dsp, &e);
      else return 0;

      *win = -1;
      switch (e.type)
      {
         case ButtonPress:
            *xclick = e.xbutton.x;
            *yclick = e.xbutton.y;
            for (int i = 0; i < MAX_GUI_WINDOWS_COUNT; i++)
               if (x11windows[i] == e.xbutton.window) *win = i;
            if (*win == -1) break;
            return -e.xbutton.button;
         case KeyPress:
            for (int i = 0; i < MAX_GUI_WINDOWS_COUNT; i++)
               if (x11windows[i] == e.xkey.window) *win = i; 
            if (*win == -1) break;
            XLookupString(&e.xkey, keybuf, sizeof(keybuf), &key, NULL);
            return key;
         case Expose:
            for (int i = 0; i < MAX_GUI_WINDOWS_COUNT; i++)
               if (x11windows[i] == e.xexpose.window) 
               {
                 gui_clear_window(surfaces[i]);
                 if (window_update_periods[i] > 0)
                    next_window_update[i] = usec() + window_update_periods[i] * 1000;
                 else 
                 {
                    next_window_update[i] = 0;
                    async_repaint_request[i] = 1;
                 }
                 window_exposed[i] = 1;
               }
            break;
         case ResizeRequest:
            for (int i = 0; i < MAX_GUI_WINDOWS_COUNT; i++)
               if (x11windows[i] == e.xresizerequest.window) *win = i;
            if (*win == -1) break;
            if ((e.xresizerequest.width != window_fixed_width[*win]) || (e.xresizerequest.height != window_fixed_height[*win]))
               XResizeWindow(dsp, x11windows[*win], window_fixed_width[*win], window_fixed_height[*win]);
            break;
            
         //default:
         //   printf("Dropping unhandled XEevent.type = %d.\n", e.type);
      }
   }
}

cairo_surface_t *gui_cairo_create_x11_surface(int *x, int *y, int win)
{
    Drawable da;
    cairo_surface_t *sfc;

    if (!x_opened)
    {
        if ((dsp = XOpenDisplay(0)) == 0)
        {
            mato_log(ML_WARN, "could not open X display, will not use graphics");
            return 0;
        }
        screen = DefaultScreen(dsp);
        scr = DefaultScreenOfDisplay(dsp);
    }
    if (!*x || !*y)
    {
         *x = WidthOfScreen(scr), *y = HeightOfScreen(scr);
         da = XCreateSimpleWindow(dsp, DefaultRootWindow(dsp), 0, 0, *x, *y, 0, 0, 0);
         gui_fullscreen (dsp, da);
    }
   else
      da = XCreateSimpleWindow(dsp, DefaultRootWindow(dsp), 0, 0, *x, *y, 0, 0, 0);
   XSelectInput(dsp, da, ButtonPressMask | KeyPressMask | ExposureMask | ResizeRedirectMask);
   XMapWindow(dsp, da);
   window_fixed_width[win] = *x;
   window_fixed_height[win] = *y;

   sfc = cairo_xlib_surface_create(dsp, da, DefaultVisual(dsp, screen), *x, *y);
   cairo_xlib_surface_set_size(sfc, *x, *y);       
   x11windows[win] = da;
   x_opened = 1;
   
   return sfc;
}

void draw_windows_title(int window_handle)
{
    char fullname[100];

    if (window_names[window_handle] == 0)
       sprintf(fullname, "Mikes - %d - [%s]", window_handle, context_names[current_context]);    
    else 
       sprintf(fullname, "Mikes - %s - [%s]", window_names[window_handle], context_names[current_context]);
    XStoreName(dsp, x11windows[window_handle], fullname);
}

void write_to_png(int window_handle, char *png_filename)
{
  cairo_surface_write_to_png(surfaces[window_handle], png_filename);
}

void gui_set_window_title(int window_handle, char *title)
{
    pthread_mutex_lock(&gui_lock);
    
    if (!window_in_use[window_handle]) 
    {
        pthread_mutex_unlock(&gui_lock);
        return;
    }
    
    if (title && strlen(title) > 40)
    {
        mato_log(ML_ERR, "windows title too long:");
        mato_log(ML_ERR, title);
    }
    window_names[window_handle] = title;
    draw_windows_title(window_handle);
    
    pthread_mutex_unlock(&gui_lock);
}

int gui_open_window(gui_draw_callback paint, int width, int height, int update_period_in_ms)
{
    int window_handle = -1;

    pthread_mutex_lock(&gui_lock);

    for (int i = 0; i < MAX_GUI_WINDOWS_COUNT; i++)
        if (!window_in_use[i])
        {
            window_handle = i;
            break;
        }
    if (window_handle == -1) 
    {
        pthread_mutex_unlock(&gui_lock);
        return -1;
    }
    surfaces[window_handle] = gui_cairo_create_x11_surface(&width, &height, window_handle);
    windows[window_handle] = cairo_create(surfaces[window_handle]);
    
    mouse_callbacks[window_handle] = 0;
    draw_callbacks[window_handle] = paint;    
    window_update_periods[window_handle] = update_period_in_ms;
    next_window_update[window_handle] = 0;
    window_names[window_handle] = 0;    
    draw_windows_title(window_handle);
    window_in_use[window_handle] = 1;
    
    pthread_mutex_unlock(&gui_lock);

    if ((update_period_in_ms == 0) && (paint != 0)) paint(windows[window_handle]);
    return window_handle;
}

void gui_close_window(int window_handle)
{
    if (!mato_config.with_gui) return;
    
    pthread_mutex_lock(&gui_lock);
    
    if (!window_in_use[window_handle]) 
    {
        pthread_mutex_unlock(&gui_lock);
        return;
    }
    
    cairo_destroy(windows[window_handle]);
    cairo_surface_destroy(surfaces[window_handle]);
    window_in_use[window_handle] = 0;
    int no_more_windows = 1;
    for (int i = 0; i < MAX_GUI_WINDOWS_COUNT; i++)
       if (window_in_use[i]) 
       {
           no_more_windows = 0;
           break;
       }
    if (no_more_windows) 
    {
        XCloseDisplay(dsp);
        x_opened = 0;
    }
    
    pthread_mutex_unlock(&gui_lock);
}

cairo_t *get_cairo_t(int window_handle)
{
    if (!window_in_use[window_handle]) return 0;
    return windows[window_handle];
}

void gui_add_key_listener(char *context, gui_key_callback callback)
{
    if (strlen(context) > 40) 
    {
        mato_log(ML_ERR, "context name too long:");
        mato_log(ML_ERR, context);
        return;
    }

    if (contexts_count == MAX_GUI_CONTEXTS_COUNT)
    {
        mato_log(ML_ERR, "gui contexts full!");
        return;
    }
    
    pthread_mutex_lock(&gui_lock);
    
    context_names[contexts_count] = context;
    key_callbacks[contexts_count] = callback;
    contexts_count++;
    
    pthread_mutex_unlock(&gui_lock);
}

void gui_remove_key_listener(char *context)
{
    pthread_mutex_lock(&gui_lock);
    
    for (int i = 1; i < contexts_count; i++)
      if (strcmp(context_names[i], context) == 0)
      {
        context_names[i] = context_names[contexts_count - 1];
        key_callbacks[i] = key_callbacks[contexts_count - 1];
        contexts_count--;
        if (current_context == i) current_context = 0;
        else if (current_context > i) current_context--;
        break;
      }
      
    pthread_mutex_unlock(&gui_lock);
}

void gui_add_mouse_listener(int window_handle, gui_mouse_callback callback)
{
    if (!window_in_use[window_handle]) return;
    mouse_callbacks[window_handle] = callback;
}

void system_gui_context_callback(int win, int key)
{
    switch(key) {
       case GUI_ESC_KEY: program_runs = 0;
                         mato_log(ML_INFO, "quit by ESC");
                         break;
    }
}
    
void shutdown_gui()
{
    if (!mato_config.with_gui) return;
 
    pthread_mutex_lock(&gui_lock);
    
    for (int i = 0; i < MAX_GUI_WINDOWS_COUNT; i++)
      if (window_in_use[i]) 
      {
        pthread_mutex_unlock(&gui_lock);
        gui_close_window(i);
        pthread_mutex_lock(&gui_lock);
      }
        
    pthread_mutex_unlock(&gui_lock);
    pthread_mutex_destroy(&gui_lock);
}

void redraw_windows_titles()
{
    for (int i = 0; i < MAX_GUI_WINDOWS_COUNT; i++)
      if (window_in_use[i])
        draw_windows_title(i);
}

void gui_switch_context(int win, int ch)
{
    if (ch == GUI_TAB_KEY) current_context++; else current_context--;
    if (current_context == contexts_count) current_context = 0;
    if (current_context < 0) current_context = contexts_count - 1;

    redraw_windows_titles();
    
    pthread_mutex_unlock(&gui_lock);
    for (int i = 0; i < contexts_count; i++)
      key_callbacks[i](win, ch);
    pthread_mutex_lock(&gui_lock);
}

void repaint_async_requests()
{
    for (int i = 0; i < MAX_GUI_WINDOWS_COUNT; i++)
        if (async_repaint_request[i])
        {
           //pthread_mutex_unlock(&gui_lock);
             async_repaint_request[i] = 0; 
             repaint_window(i);
          // pthread_mutex_lock(&gui_lock);
        }
}

long long repaint_expired_windows()
{
   long long tm = usec();
   long long next_update = tm + 100000;
   
   for (int i = 0; i < MAX_GUI_WINDOWS_COUNT; i++)
       if (window_in_use[i] && next_window_update[i])
       {
           if (next_window_update[i] <= tm)
           {
               repaint_window(i);
               next_window_update[i] = tm + window_update_periods[i] * 1000;
               if (next_window_update[i] < next_update) next_update = next_window_update[i];
           }
           else if (next_window_update[i] < next_update) next_update = next_window_update[i];
       }
   return next_update;
}

void process_gui_events()
{
    int xclick, yclick, win;
    do {
      int event = gui_cairo_check_event(&xclick, &yclick, &win);
      if (event == 0) break;
      if (win == -1) continue;
      if (!window_in_use[win]) continue;
      
      if (event < 0)
      {
          if (mouse_callbacks[win])
          {
                pthread_mutex_unlock(&gui_lock);
                   mouse_callbacks[win](xclick, yclick, event);
                pthread_mutex_lock(&gui_lock);
           }
      }
      else if ((event == GUI_TAB_KEY) || (event == GUI_SHIFT_TAB_KEY))        
            gui_switch_context(win, event);
      else 
      {
            pthread_mutex_unlock(&gui_lock);
              key_callbacks[current_context](win, event);
            pthread_mutex_lock(&gui_lock);
      }
    } while (1);
}

void *gui_thread(void *arg)
{
    while (program_runs)
    {
        if (dsp == 0) 
        {
            usleep(1000);
            continue;
        }
        
        pthread_mutex_lock(&gui_lock);
        
        repaint_async_requests();
        long long next_update = repaint_expired_windows();
        process_gui_events();

        pthread_mutex_unlock(&gui_lock);

        long long tm = usec();
        if (next_update > tm)
            usleep((long)(next_update - tm));     
    }

    mato_log(ML_INFO, "gui quits.");
    mato_dec_thread_count();
    return 0;
}

void init_gui()
{
    if (!mato_config.with_gui)
    {
        mato_log(ML_INFO, "gui supressed by config.");
        return;
    }
    x_opened = 0;
    dsp = 0;
    for (int i = 0; i < MAX_GUI_WINDOWS_COUNT; i++)
    {
        window_in_use[i] = 0;
        window_exposed[i] = 0;
        x11windows[i] = 0;
        async_repaint_request[i] = 0;
    }
    contexts_count = 1;
    current_context = 0;
    context_names[0] = "system";
    key_callbacks[0] = system_gui_context_callback;
        
    pthread_mutex_init(&gui_lock, 0);

    // if the following line is not here, the app crashes sometimes, not sure why :(
    XInitThreads();
    
    pthread_t t;
    if (pthread_create(&t, 0, gui_thread, 0) != 0)
    {
        perror("mato:gui");
        mato_log(ML_ERR, "creating gui thread");
    }
    else mato_inc_thread_count("gui");
}

char *get_current_gui_context()
{
    return context_names[current_context];
}

void set_current_gui_context(char *context)
{
    pthread_mutex_lock(&gui_lock);
    
    for (int i = 0; i < contexts_count; i++)
        if (strcmp(context_names[i], context) == 0)
        {
           current_context = i;
           redraw_windows_titles();
           break;
        }
    
    pthread_mutex_unlock(&gui_lock);
}

double draw_svg_to_cairo(cairo_t *w, int x, int y, char *filename, int max_width, int max_height)
{
    GError *gerror;
    RsvgHandle *rsvg_handle = rsvg_handle_new_from_file(filename, &gerror);
    if (rsvg_handle == 0)
    {
      mato_log_str(ML_ERR, "could not load svg file: ", filename);
      return -1;
    }
    RsvgDimensionData dimensions;
    rsvg_handle_get_dimensions(rsvg_handle, &dimensions);
    double scale_x = max_width / (double)dimensions.width;
    double scale_y = max_height / (double)dimensions.height;
    double scale = scale_x;
    if (scale_y < scale_x) scale = scale_y;
    cairo_translate(w, x, y + max_height);
    cairo_scale(w, scale, -scale);

    if (!rsvg_handle_render_cairo(rsvg_handle, w))
    {
      mato_log_str(ML_ERR, "could not render svg file: ", filename);
      g_object_unref(rsvg_handle);
      return -1;
    }

    cairo_stroke(w);
    //g_object_unref(rsvg_handle);
    return scale;
}

void request_async_repaint(int win)
{
    if (window_in_use[win]) async_repaint_request[win] = 1;
}

