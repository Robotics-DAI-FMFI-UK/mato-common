/// \file mato_config.c
/// Mato control framework - public interface for dealing with var:val style config files.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <malloc.h>
#include <string.h>
#include <ctype.h>

#include "mato_config.h"

/// Internal representation of list of var:val data.
typedef struct mato_config_var_val_pair_str {
    char *var;
    char *val;
    struct mato_config_var_val_pair_str *next;
} mato_config_var_val_pair;

/// A structure that holds the configuration file loaded to memory.
typedef mato_config_var_val_pair *config_data;

char *mato_config_get_strval(void *config, char *var_name, char *default_value)
{
  config_data browse = config;
  while (browse)
  {
     if (strcmp(var_name, browse->var) == 0)
       return browse->val;
     browse = browse->next;
  }
  return default_value;
}

char *mato_config_get_alloc_strval(void *config, char *var_name, char *default_value)
{
  char *val = mato_config_get_strval(config, var_name, default_value);
  char *newval = (char *) malloc(strlen(val) + 1);
  strcpy(newval, val);
  return newval;
}

int mato_config_get_intval(void *config, char *var_name, int default_value)
{
  int x = default_value;
  char *val = mato_config_get_strval(config, var_name, 0);
  if (val != 0) sscanf(val, "%d", &x);
  return x;
}

double mato_config_get_doubleval(void *config, char *var_name, double default_value)
{
  double x = default_value;
  char *val = mato_config_get_strval(config, var_name, 0);
  if (val != 0) sscanf(val, "%lf", &x);
  return x;
}

void mato_config_dispose(void *config)
{
  mato_config_var_val_pair *cfg = config;
  mato_config_var_val_pair *browse = config;
  while (cfg)
  {
    browse = cfg->next;
    free(cfg);
    cfg = browse;
  }
}

/// Store the specified variable/value pair to the internal representation, create copies of those strings in the memory.
static void add_var_val_pair(config_data *first, char *var_name, int var_len, char *val_start, char *val_last)
{
   mato_config_var_val_pair *inserted = (mato_config_var_val_pair *)malloc(sizeof(mato_config_var_val_pair));
   inserted->var = (char *)malloc(var_len + 1);
   strncpy(inserted->var, var_name, var_len);
   *(inserted->var + var_len) = 0;
   int val_len = val_last - val_start + 1;
   inserted->val = (char *)malloc(val_len + 1);
   strncpy(inserted->val, val_start, val_len);
   *(inserted->val + val_len) = 0;
   inserted->next = *first;
   *first = inserted;
}

/// constants of the parser state machine
#define EXPECT_LINE_START 0
#define LOADS_VARIABLE 1
#define EXPECT_COLON 2
#define EXPECT_VALUE 3
#define LOADS_VALUE 4
#define EXPECT_LINE_END 5

/// Proceed with the parser state machine ine step further
static void parse_char(config_data *first, char **ch, unsigned char *state)
{
    static int ln = 1;
    static char *var_start, *val_start, *val_last;
    static int var_len;
  
    if (**ch == '\n') ln++;
   
    switch (*state) {
    case EXPECT_LINE_START:
          if (**ch == '#')
              *state = EXPECT_LINE_END;
  	      else if (!isspace(**ch))
          {
              *state = LOADS_VARIABLE;
              var_start = *ch;
              var_len = 1;
          }
          break;
  
    case LOADS_VARIABLE:
          if (**ch == '\n') printf("unexpected end of line at ln. %d\n", ln - 1);
          else if (isspace(**ch)) 
              *state = EXPECT_COLON;
          else if (**ch == ':')
  	          *state = EXPECT_VALUE;
          else var_len++;
          break;
  
    case EXPECT_COLON:
          if (**ch == '\n') printf("unexpected end of line at ln. %d\n", ln - 1);
  	      if (**ch == ':') *state = EXPECT_VALUE;
          break;
  
    case EXPECT_VALUE:
          if (!isspace(**ch))
          {
              val_start = *ch;
              val_last = *ch;
              *state = LOADS_VALUE;
          }
          break;
  
    case LOADS_VALUE:
  	    if ((**ch == '\n')  || (**ch == '#'))
          {
              add_var_val_pair(first, var_start, var_len, val_start, val_last);
              *state = (**ch == '\n') ? EXPECT_LINE_START : EXPECT_LINE_END;
          } 
          else if (!isspace(**ch)) val_last = *ch;
          break;
  
    case EXPECT_LINE_END:
          if (**ch == '\n') *state = EXPECT_LINE_START;
          break;
    }
    (*ch)++;
}

/// parses the complete config after it has been read from a file to a memory buffer pointed to by the buf argument.
static void parse_config(config_data *first, char *buf, long config_size)
{
    char *stop_at_char = buf + config_size;
    unsigned char state = EXPECT_LINE_START;
    while (buf < stop_at_char)
        parse_char(first, &buf, &state); 
}

void *mato_config_read(char *filename)
{
    mato_config_var_val_pair *first;

    do {
        first = 0;
        struct stat sb;
        if (stat(filename, &sb) == -1) break;
      
        long config_size = (long)sb.st_size;
        char *config_buf = (char *)malloc(config_size + 1);
        if (config_buf == 0) break;
    
        do {
      
            int f = open(filename, O_RDONLY);
            if (f < 0) break;
            int nread = read(f, config_buf, config_size);
            if (nread < config_size)
            {
              close(f);
              break;
            }
            close(f);
            *(config_buf + config_size) = '\n';
            parse_config(&first, config_buf, config_size + 1);
    
        } while(0);
        free(config_buf);
    } while (0);
    config_data retval = first;
    return retval;
}

