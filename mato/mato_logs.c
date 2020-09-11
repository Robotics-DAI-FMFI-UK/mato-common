#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <malloc.h>
#include <stdlib.h>
#include <sys/time.h>
#include <poll.h>

#include "mato_core.h"
#include "mato_logs.h"

static char *log_filename;
static long long start_time;

static int print_to_console = 0;
static int print_debug = 0;

static int log_queue[2];

static char *log_type_str[4] = { "INFO", "WARN", " ERR", "DEBG" };

static FILE *try_opening_log()
{
    FILE *f = fopen(log_filename, "a+");
    if (!f)
        perror("mato:logs try_opening_log");
    return f;
}

char *read_next_line_from_pipe()
{
    char *logmsg;
    int retval = read(log_queue[0], &logmsg, sizeof(char *));
    if (retval < 0)
    {
        perror("error reading from log pipe");
        return 0;
    }
    if (retval == 0) // pipe write has closed, framework terminates
        return 0;
}

/// mato logs thread that writes the messages sequentially to the output logfile
static void *mato_logs_thread(void *arg)
{
    mato_inc_system_thread_count();
    while (program_runs)
    {
        char *ln = read_next_line_from_pipe();
        if (!ln) break;

        FILE *f = try_opening_log();
        if (f)
        {
            do {
                fprintf(f, "%s", ln);
                free(ln);
                if (poll(&(struct pollfd){ .fd = log_queue[0], .events = POLLIN }, 1, 0) == 1) 
                {
                    ln = read_next_line_from_pipe();
                    if (ln) continue;
                }    
                break;
            } while (1);
            fclose(f);
        }        
    }
    mato_dec_system_thread_count();
}

//  /usr/local/logs/mato/
void mato_logs_init(int print_all_logs_to_console, int print_debug_logs, const char *log_path)
{
    char *filename_str = "%s/%ld_%s";
    char *lastlog;
    char *filename_base = "mato.log";
    char *log_filename;

    lastlog = (char *)malloc(strlen(log_path) + 6);
    sprintf(lastlog, "%s/last", log_path);

    start_time = msec();
    print_to_console = print_all_logs_to_console;
    print_debug = print_debug_logs;
  
    log_filename = (char *)malloc(strlen(log_path) + strlen(filename_str) + 20 + strlen(filename_base));
    if (log_filename == 0)
    {
        perror("mato:logs malloc");
        exit(1);
    }
  
    time_t tm;
    time(&tm);
    sprintf(log_filename, filename_str, log_path, tm, filename_base);
  
    FILE *f = fopen(log_filename, "w+");
    if (f == 0)
    {
        perror("mato:logs");
        printf("Could not open log file %s\n", log_filename);
        exit(1);
    }
    fclose(f);
  
    unlink(lastlog);
    symlink(log_filename, lastlog);
  
    if (pipe(log_queue) < 0)
    {
        perror("init_mato_logs: pipe");
        exit(-1);
    }
  
    pthread_t t;
    if (pthread_create(&t, 0, mato_logs_thread, 0) != 0)
        perror("could not create thread for logs");
  
    if (print_to_console) mato_log(ML_INFO, "printing all logs to console");
    char ctm[40];
    sprintf(ctm, "%s", ctime(&tm));
    ctm[strlen(ctm) - 1] = 0;
    mato_log(ML_INFO, ctm);
}

void mato_logs_shutdown()
{
    free(log_filename);
    log_filename = 0;
}

long get_run_time()
{
    return (long)(msec() - start_time);
}

static void log_pipe_error()
{
    perror("writing to log pipe");
}

int check_log_type(int *log_type)
{
    if ((*log_type < 0) || (*log_type > ML_MAX_TYPE))
    {
        printf("WARN: unrecognized log type %d\n", log_type);
        *log_type = ML_ERR;
    }
    else if ((*log_type == ML_DEBUG) && !print_debug) return 0;
    return 1;
}

void mato_log(unsigned int log_type, char *log_msg)
{
    if (!check_log_type(&log_type)) return;
    long run_time = get_run_time();
  
    char *log_full_msg = (char *)malloc(strlen(log_msg) + 20);
    sprintf(log_full_msg, "%05ld.%03d %s: %s\n", run_time / 1000L, (int)(run_time % 1000L), log_type_str[log_type], log_msg);
    if (write(log_queue[1], &log_full_msg, sizeof(char *)) < 0)
        log_pipe_error();
  
    if (print_to_console)
        printf("%s: %s\n", log_type_str[log_type], log_msg);
}

void mato_log_str(unsigned int log_type, char *log_msg, const char *log_msg2)
{
    if (!check_log_type(&log_type)) return;
    long run_time = get_run_time();
  
    char *log_full_msg = (char *)malloc(strlen(log_msg) + strlen(log_msg2) + 20);
    sprintf(log_full_msg, "%05ld.%03d %s: %s%s\n", run_time / 1000L, (int)(run_time % 1000L), log_type_str[log_type], log_msg, log_msg2);
    if (write(log_queue[1], &log_full_msg, sizeof(char *)) < 0)
        log_pipe_error();
  
    if (print_to_console)
        printf("%s: %s%s\n", log_type_str[log_type], log_msg, log_msg2);
}

void mato_log_val2(unsigned int log_type, char *log_msg, int val, int val2)
{
    if (!check_log_type(&log_type)) return;
    long run_time = get_run_time();

    char *log_full_msg = (char *)malloc(strlen(log_msg) + 40);
    sprintf(log_full_msg, "%05ld.%03d %s: %s %d %d\n", run_time / 1000L, (int)(run_time % 1000L), log_type_str[log_type], log_msg, val, val2);
    if (write(log_queue[1], &log_full_msg, sizeof(char *)) < 0)
        log_pipe_error();

    if (print_to_console)
        printf("%s: %s %d %d\n", log_type_str[log_type], log_msg, val, val2);
}

void mato_log_double2(unsigned int log_type, char *log_msg, double val, double val2)
{
    if (!check_log_type(&log_type)) return;
    long run_time = get_run_time();

    char *log_full_msg = (char *)malloc(strlen(log_msg) + 100);
    sprintf(log_full_msg, "%05ld.%03d %s: %s %16.8G %16.6G\n", run_time / 1000L, (int)(run_time % 1000L), log_type_str[log_type], log_msg, val, val2);
    if (write(log_queue[1], &log_full_msg, sizeof(char *)) < 0)
        log_pipe_error();

    if (print_to_console)
        printf("%s: %s %e %e\n", log_type_str[log_type], log_msg, val, val2);
}

void mato_log_val(unsigned int log_type, char *log_msg, int val)
{
    if (!check_log_type(&log_type)) return;
    long run_time = get_run_time();

    char *log_full_msg = (char *)malloc(strlen(log_msg) + 30);
    sprintf(log_full_msg, "%05ld.%03d %s: %s %d\n", run_time / 1000L, (int)(run_time % 1000L), log_type_str[log_type], log_msg, val);
    if (write(log_queue[1], &log_full_msg, sizeof(char *)) < 0)
        log_pipe_error();

    if (print_to_console)
        printf("%s: %s %d\n", log_type_str[log_type], log_msg, val);
}

void mato_log_double(unsigned int log_type, char *log_msg, double val)
{
    if (!check_log_type(&log_type)) return;
    long run_time = get_run_time();
  
    char *log_full_msg = (char *)malloc(strlen(log_msg) + 50);
    sprintf(log_full_msg, "%05ld.%03d %s: %s %16.8G\n", run_time / 1000L, (int)(run_time % 1000L), log_type_str[log_type], log_msg, val);
    if (write(log_queue[1], &log_full_msg, sizeof(char *)) < 0)
        log_pipe_error();
  
    if (print_to_console)
        printf("%s: %s %e\n", log_type_str[log_type], log_msg, val);
}
