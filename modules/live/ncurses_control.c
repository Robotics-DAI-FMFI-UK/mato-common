#include <ncurses.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

#include "../../mato/mato.h"
#include "core/config_mato.h"
#include "ncurses_control.h"

#define KEY_TAB             9
#define KEY_SHIFT_TAB     353
#define KEY_ALT_LEFT     543
#define KEY_ALT_RIGHT    558
#define KEY_ALT_UP       564
#define KEY_ALT_DOWN     523
#define KEY_CTRL_UP      566
#define KEY_CTRL_DOWN    525
#define KEY_CTRL_PGUP    555
#define KEY_CTRL_PGDN    550
#define MAX_CONTEXTS_COUNT 10
#define KEY_ALT_DELETE   517
#define KEY_F2           266

#define SCREEN_WIDTH 122
#define SCREEN_HEIGHT 40
#define SYSTEM_PANEL_HEIGHT 3
#define VERTICAL_WINDOWS_COUNT 3
#define HORIZONTAL_WINDOWS_COUNT 4
#define MAX_WINDOWS_COUNT (HORIZONTAL_WINDOWS_COUNT * VERTICAL_WINDOWS_COUNT)

#define MAX_LINES_IN_WINDOWS_BUFFERS 2000

static WINDOW *mainwin;

static char *context_names[MAX_CONTEXTS_COUNT];
static ncurses_control_callback callbacks[MAX_CONTEXTS_COUNT];
static volatile int contexts_count;
static volatile int current_context;

static char *window_title[MAX_WINDOWS_COUNT];
static int window_in_use[MAX_WINDOWS_COUNT];  

static int window_width;
static int window_height;

static int active_window;

static char windows_buffers[MAX_WINDOWS_COUNT][MAX_LINES_IN_WINDOWS_BUFFERS][(SCREEN_WIDTH - HORIZONTAL_WINDOWS_COUNT + 1) / HORIZONTAL_WINDOWS_COUNT + 1];
static int lines_in_windows_buffer[MAX_WINDOWS_COUNT];
static int window_buffer_next_free_line[MAX_WINDOWS_COUNT];
static int window_scroll_position[MAX_WINDOWS_COUNT];

static volatile int ncurses_thread_started;

void add_key_listener(char *context, ncurses_control_callback callback)
{
  if (contexts_count == MAX_CONTEXTS_COUNT)
  {
    mvprintw(0, 60, "contexts full!");
    return;
  }
  context_names[contexts_count] = context;
  callbacks[contexts_count] = callback;
  contexts_count++;
}

void remove_key_listener(char *context)
{
  for (int i = 1; i < contexts_count; i++)
    if (strcmp(context_names[i], context) == 0)
    {
      context_names[i] = context_names[contexts_count - 1];
      callbacks[i] = callbacks[contexts_count - 1];
      contexts_count--;
      if (current_context == i) current_context = 0;
      else if (current_context > i) current_context--;
      break;
    }
}

char *get_current_context()
{
  return context_names[current_context];
}

void set_current_context(char *context)
{
  for (int i = 0; i < contexts_count; i++)
    if (strcmp(context_names[i], context) == 0)
    {
       current_context = i;
       break;
    } 
}

void print_current_context()
{
    mvprintw(0, 43, "                    ");
    mvprintw(0, 43, context_names[current_context]);
    refresh();
}

int initialize_curses()
{
    if ((mainwin = initscr()) == 0)
    {
      mato_log(ML_WARN, "Error initializing ncurses, not using ncurses");
      return 0;
    }
    noecho();                  //  Turn off key echoing
    keypad(mainwin, TRUE);     //  Enable the keypad for non-char keys
    curs_set(0);               //  no cursor
    return 1;
}

void print_system_message(char *msg)
{
    mvprintw(2, 55, "                                    ");
    mvprintw(2, 55, msg);
}

void draw_system_screen()
{
    mvaddstr(0, 0, "Mikes");
    mvprintw(0, 10, "TAB switches context. Context: ");
    mvprintw(0, 63, "ALT-arrows switch active window, CTRL-UP/CTRL-DOWN scroll");
    mvprintw(2, 0, "ALT-Delete clear window, F2 save all buffers to file");
    move(1, 0);
    hline(ACS_HLINE, SCREEN_WIDTH);
    print_current_context();
}

int win_top(int window_handle)
{
    return SYSTEM_PANEL_HEIGHT + (window_handle / HORIZONTAL_WINDOWS_COUNT) * (window_height + 1) + 1;
}

int win_left(int window_handle)
{
    return (window_handle % HORIZONTAL_WINDOWS_COUNT) * (window_width + 1);
}

void draw_windows()
{
    move(SYSTEM_PANEL_HEIGHT, 0);
    hline(ACS_HLINE, SCREEN_WIDTH);
    window_height = (SCREEN_HEIGHT - SYSTEM_PANEL_HEIGHT) / VERTICAL_WINDOWS_COUNT - 1;
    window_width = (SCREEN_WIDTH + 1) / HORIZONTAL_WINDOWS_COUNT - 1;

    for (int i = 0; i < VERTICAL_WINDOWS_COUNT; i++)
    {
        move(SYSTEM_PANEL_HEIGHT + i * (window_height + 1), 0);
        hline(ACS_HLINE, SCREEN_WIDTH);
    }
    for (int i = 1; i < HORIZONTAL_WINDOWS_COUNT; i++)
    {
        move(SYSTEM_PANEL_HEIGHT + 1, i * (window_width + 1) - 1);
        vline(ACS_VLINE, SCREEN_HEIGHT - SYSTEM_PANEL_HEIGHT - 1);
    }
    for (int j = 1; j < HORIZONTAL_WINDOWS_COUNT; j++)
    {
         move(SYSTEM_PANEL_HEIGHT, (window_width + 1) * j - 1);
         addch(ACS_TTEE);
    }

    for (int i = 1; i < VERTICAL_WINDOWS_COUNT; i++)
      for (int j = 1; j < HORIZONTAL_WINDOWS_COUNT; j++)
      {
         move(SYSTEM_PANEL_HEIGHT + (window_height + 1) * i, (window_width + 1) * j - 1);
         addch(ACS_PLUS);
      }

    for (int i = 0; i < MAX_WINDOWS_COUNT; i++)
      if (window_in_use[i])
      { 
         move(win_top(i) - 1, win_left(i) + window_width / 2 - strlen(window_title[i]) / 2 - 1);
         if (i == active_window) addch('['); else addch(' ');
         addstr(window_title[i]);
         if (i == active_window) addch(']'); else addch(' ');
      }
}

void switch_context(int ch)
{
    if (ch == KEY_TAB) current_context++; else current_context--;
    if (current_context == contexts_count) current_context = 0;
    if (current_context < 0) current_context = contexts_count - 1;
    print_current_context();
    for (int i = 0; i < contexts_count; i++)
      callbacks[i](ch);
} 

int switch_active_window(int ch)
{
  do {
    if (ch == KEY_ALT_LEFT)
    {
      if (active_window >= 0)
      {
        do {
          active_window--;
          if (active_window < 0) active_window = MAX_WINDOWS_COUNT - 1;
        } while (window_in_use[active_window] == 0);
      }
      break;
    }
    else if (ch == KEY_ALT_RIGHT)
    {
      if (active_window >= 0)
      {
        do {
          active_window++;
          if (active_window == MAX_WINDOWS_COUNT) active_window = 0;
        } while (window_in_use[active_window] == 0);
      }
      break;
    }
    else if (ch == KEY_ALT_UP)
    {
      if (active_window >= 0)
      {
        do {
          active_window -= HORIZONTAL_WINDOWS_COUNT;
          if (active_window < 0) active_window += MAX_WINDOWS_COUNT;
        } while (window_in_use[active_window] == 0);
      }
      break;
    }
    else if (ch == KEY_ALT_DOWN)
    {
      if (active_window >= 0)
      {
        do {
          active_window += HORIZONTAL_WINDOWS_COUNT;
          if (active_window > MAX_WINDOWS_COUNT) active_window -= MAX_WINDOWS_COUNT;
        } while (window_in_use[active_window] == 0);
      }
      break;
    }
    return 0;
  } while(0);
  draw_windows();
  return 1;
}

void print_window_content(int win)
{
    int row = win_top(win);
    int col = win_left(win);
    for (int r = row; r < row + window_height; r++)
    {
       move(r, col);
       hline(' ', window_width);
    }
    int lines_to_print = window_height;
    if (lines_to_print > lines_in_windows_buffer[win]) 
      lines_to_print = lines_in_windows_buffer[win];
    int buf_row = window_scroll_position[win] - lines_to_print;
    if (buf_row < 0) buf_row += MAX_LINES_IN_WINDOWS_BUFFERS;
    row = win_top(win);

    while (lines_to_print)
    {
      mvprintw(row, col, windows_buffers[win][buf_row]);
      lines_to_print--;
      row++;
      buf_row++;
      if (buf_row == MAX_LINES_IN_WINDOWS_BUFFERS) buf_row = 0;
    }
    refresh();
}

void scroll_one_line_up()
{
    window_scroll_position[active_window]--;
    if (window_scroll_position[active_window] < 0) 
       window_scroll_position[active_window] += MAX_LINES_IN_WINDOWS_BUFFERS;
}

void scroll_one_line_down()
{
    window_scroll_position[active_window]++;
    if (window_scroll_position[active_window] == MAX_LINES_IN_WINDOWS_BUFFERS)
       window_scroll_position[active_window] = 0;
}

int scroll_active_window(int ch)
{
    int lines_to_scroll = 1;
    if ((ch == KEY_CTRL_UP) || (ch == KEY_CTRL_PGUP))
    {
      if (active_window >= 0)
      {
        if (lines_in_windows_buffer[active_window] > window_height)
        {
          int topmost_pos = window_buffer_next_free_line[active_window] - lines_in_windows_buffer[active_window] + window_height;
          if (topmost_pos < 0) topmost_pos += MAX_LINES_IN_WINDOWS_BUFFERS;
          if (ch == KEY_CTRL_PGUP) lines_to_scroll = window_height;
          while (lines_to_scroll--)
            if (window_scroll_position[active_window] != topmost_pos)
            {
              scroll_one_line_up(); 
              print_window_content(active_window);
            }
            else break;
        }
      }
      return 1;
    }
    else if ((ch == KEY_CTRL_DOWN) || (ch == KEY_CTRL_PGDN))
    {
      if (active_window >= 0)
      {
        if (ch == KEY_CTRL_PGDN) lines_to_scroll = window_height;
        while (lines_to_scroll--)
          if (window_scroll_position[active_window] != window_buffer_next_free_line[active_window])
          {
            scroll_one_line_down();
            print_window_content(active_window);
          }
          else break;
      }
      return 1;
    }
    return 0;
}
 

int save_buffers_request(int ch)
{
    char save_file_name[30];

    if (ch != KEY_F2) return 0;
    time_t tm;
    time(&tm);
    sprintf(save_file_name, "%ld_windows.txt", tm);
    FILE *f = fopen(save_file_name, "w+");
    if (f == 0)
    {
       print_system_message("could not save windows to file");
       return 1;
    }
    for (int i = 0; i < MAX_WINDOWS_COUNT; i++)
    {
       if (window_in_use[i])
       {
          fprintf(f, "%d - %s\n\n", i, window_title[i]);
          int ln = window_buffer_next_free_line[i];
          ln -= lines_in_windows_buffer[i];
          if (ln < 0) ln += MAX_LINES_IN_WINDOWS_BUFFERS;
          while (ln != window_buffer_next_free_line[i])
          {
             fprintf(f, "%s\n", windows_buffers[i][ln]);
             ln++;
             if (ln == MAX_LINES_IN_WINDOWS_BUFFERS) ln = 0;
          }
          fprintf(f,  "\n");
       }
    }
    fclose(f);
    print_system_message("All buffers saved.");
    return 1;
}

int clear_window_request(int ch)
{
    if (ch != KEY_ALT_DELETE) return 0;
    clear_window(active_window); 
    return 1;
}

void *ncurses_control_thread(void *arg)
{
    if (!initialize_curses()) 
    {
       ncurses_thread_started = 1;
       return 0;
    }
    
    draw_system_screen();
    draw_windows();

    ncurses_thread_started = 1;

//Control Kocur Mikes with arrow keys, space to backup, 'a' to toggle autopilot, ESC to quit");

    while (program_runs)
    {
        int ch = getch();
        if ((ch == KEY_TAB) || (ch == KEY_SHIFT_TAB))
          switch_context(ch);
        else if (!switch_active_window(ch))
        if (!scroll_active_window(ch))
        if (!save_buffers_request(ch))
        if (!clear_window_request(ch))
          callbacks[current_context](ch);
    }
            
    delwin(mainwin);
    endwin();
    refresh();

    mato_log(ML_INFO, "ncurses quits.");
    mato_dec_thread_count(-1);

    return 0;
}

void system_context_callback(int key)
{
    switch(key) {
       case KEY_ESC: program_runs = 0;
                     mato_log(ML_INFO, "quit by ESC");
                     break;
    }
}

void init_ncurses_control()
{
    contexts_count = 1;
    current_context = 0;
    context_names[0] = "system";
    callbacks[0] = system_context_callback;    
    for (int i = 0; i < MAX_WINDOWS_COUNT; i++)
       window_in_use[i] = 0;
    active_window = -1;

    if (!mato_config.use_ncurses_control) return;

    ncurses_thread_started = 0;
    pthread_t t;
    if (pthread_create(&t, 0, ncurses_control_thread, 0) != 0)
    {
        perror("mato:ncurses");
        mato_log(ML_ERR, "creating ncurses thread");
        while (ncurses_thread_started == 0) usleep(10000);
    }
    else mato_inc_thread_count("ncurses");
}


int open_window()
{
  for (int i = 0; i < MAX_WINDOWS_COUNT; i++)
    if (!window_in_use[i])
    {
      window_in_use[i] = 1;
      window_title[i] = "a window";
      if (active_window == -1) active_window = i; 
      lines_in_windows_buffer[i] = 0;
      window_buffer_next_free_line[i] = 0;
      window_scroll_position[i] = 0;
      draw_windows();
      return i;
    }
  return -1;
}

void close_window(int window_handle)
{
  if (!window_in_use[window_handle]) return;
  window_in_use[window_handle] = 0;
  if (active_window == window_handle) active_window = -1;
  draw_windows();
}

void clear_window(int window_handle)
{
  if (!window_in_use[window_handle]) return;
  lines_in_windows_buffer[window_handle] = 0;
  window_buffer_next_free_line[window_handle] = 0;
  window_scroll_position[window_handle] = 0;
  print_window_content(window_handle);
}

void set_window_title(int window_handle, char *title)
{
  if (!window_in_use[window_handle]) return;
  window_title[window_handle] = title;
  draw_windows();
}

void scroll_window_up_if_needed(int window_handle)
{
    if (lines_in_windows_buffer[window_handle] < MAX_LINES_IN_WINDOWS_BUFFERS)
      lines_in_windows_buffer[window_handle]++;

    if (window_scroll_position[window_handle] == window_buffer_next_free_line[window_handle]) 
    {
      window_scroll_position[window_handle]++;
      if (window_scroll_position[window_handle] == MAX_LINES_IN_WINDOWS_BUFFERS) 
        window_scroll_position[window_handle] = 0;
    }
    window_buffer_next_free_line[window_handle]++;
    if (window_buffer_next_free_line[window_handle] == MAX_LINES_IN_WINDOWS_BUFFERS)
      window_buffer_next_free_line[window_handle] = 0;

    print_window_content(window_handle);
    refresh();
}

void println_to_window(int window_handle, char *message)
{
    if (!window_in_use[window_handle]) return;
    char *mem = windows_buffers[window_handle][window_buffer_next_free_line[window_handle]];

    strncpy(mem, message, window_width);
    mem[window_width] = 0;

    scroll_window_up_if_needed(window_handle);
}

void print_int_to_window(int window_handle, int value)
{
    if (!window_in_use[window_handle]) return;
    char *mem = windows_buffers[window_handle][window_buffer_next_free_line[window_handle]];

    int width = 1;
    int val = value / 10;
    while (val > 0)
    {
        val /= 10;
        width ++;
    }

    if (window_width < width) { *mem = '*'; *(mem + 1) = 0; }
    else sprintf(mem, "%d", value);

    scroll_window_up_if_needed(window_handle);
}

void print_double_to_window(int window_handle, double value)
{
    if (!window_in_use[window_handle]) return;
    char *mem = windows_buffers[window_handle][window_buffer_next_free_line[window_handle]];

    if (window_width < 10) { *mem = '*'; *(mem + 1) = 0; }
    else sprintf(mem, "%10.5lf", value);

    scroll_window_up_if_needed(window_handle);
}

void print_intmsg_to_window(int window_handle, char *message, int value)
{
    if (!window_in_use[window_handle]) return;
    char *mem = windows_buffers[window_handle][window_buffer_next_free_line[window_handle]];

    int width = 1;
    int val = value / 10;
    while (val > 0)
    {
        val /= 10;
        width ++;
    }

    if (window_width < width + strlen(message)) { *mem = '*'; *(mem + 1) = 0; }
    else sprintf(mem, "%s%d", message, value);

    scroll_window_up_if_needed(window_handle);
}

void print_doublemsg_to_window(int window_handle, char *message, double value)
{
    if (!window_in_use[window_handle]) return;
    char *mem = windows_buffers[window_handle][window_buffer_next_free_line[window_handle]];

    if (window_width < 10 + strlen(message)) { *mem = '*'; *(mem + 1) = 0; }
    else sprintf(mem, "%s%10.5lf", message, value);

    scroll_window_up_if_needed(window_handle);
}

