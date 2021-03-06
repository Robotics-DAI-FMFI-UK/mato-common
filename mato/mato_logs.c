/// \file mato_logs.c
/// Mato control framework - debug logging to a file/console - implementation.

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

/// the init will generate a pointer to the logfile filename here
static char *log_filename;

/// all messages are leading with the time elapsed since the init was called
static long long start_time;

/// all log messages are passed to the log-writing thread through this pipe (a pointer to a character string)
static int log_queue[2];

/// character string representations of the severity levels
static char *log_type_str[4] = { "INFO", "WARN", " ERR", "DEBG" };

/// before each burst of log messages to be saved to log file, the file is opened (and then closed again)
/// so that no messages are lost in cache in case of crash
static FILE *try_opening_log()
{
    FILE *f = fopen(log_filename, "a+");
    if (!f)
        perror("mato:logs try_opening_log");
    return f;
}

/// retrieve the chracter string passed from logging functions to the log-saving stream through a pipe
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

    return logmsg;
}

/// mato logs thread that writes the messages sequentially to the output logfile
static void *mato_logs_thread(void *arg)
{
    mato_inc_system_thread_count("logs");
    while (1) // logs terminate only through their own shutdown
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
    close(log_queue[0]);
    mato_dec_system_thread_count();
    return 0;
}

void mato_logs_init()
{
    char *filename_str = "%s/%ld_%s";
    char *lastlog;

    lastlog = (char *)malloc(strlen(mato_core_config.logs_path) + 6);
    sprintf(lastlog, "%s/last", mato_core_config.logs_path);

    start_time = msec();
  
    log_filename = (char *)malloc(strlen(mato_core_config.logs_path) + strlen(filename_str) + 20 + strlen(mato_core_config.log_filename_suffix));
    if (log_filename == 0)
    {
        perror("mato:logs malloc");
        exit(1);
    }
  
    time_t tm;
    time(&tm);
    sprintf(log_filename, filename_str, mato_core_config.logs_path, tm, mato_core_config.log_filename_suffix);
  
    FILE *f = try_opening_log();
    if (f == 0)
    {
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
  
    if (mato_core_config.print_all_logs_to_console) mato_log(ML_INFO, "printing all logs to console");
    char ctm[40];
    sprintf(ctm, "%s", ctime(&tm));
    ctm[strlen(ctm) - 1] = 0;
    mato_log(ML_INFO, ctm);
}

void mato_logs_shutdown()
{
    free(log_filename);
    log_filename = 0;
    close(log_queue[1]);
}

/// utility function to calculate the time since the init was called
static long get_run_time()
{
    return (long)(msec() - start_time);
}

/// report error with pipe
static void log_pipe_error()
{
    perror("writing to log pipe");
}

/// filter out disabled or illegal log_types
int check_log_type(int *log_type)
{
    if ((*log_type < 0) || (*log_type > ML_MAX_TYPE))
    {
        printf("WARN: unrecognized log type %d\n", *log_type);
        *log_type = ML_ERR;
    }
    else if ((*log_type == ML_DEBUG) && !mato_core_config.print_debug_logs) return 0;
    return 1;
}

void mato_log(int log_type, char *log_msg)
{
    if (!check_log_type(&log_type)) return;
    long run_time = get_run_time();
  
    char *thread_name = this_thread_name();
    char *log_full_msg = (char *)malloc(strlen(log_msg) + strlen(thread_name) + 20);
    sprintf(log_full_msg, "%05ld.%03d %s %s: %s\n", run_time / 1000L, (int)(run_time % 1000L), log_type_str[log_type], thread_name, log_msg);
    if (write(log_queue[1], &log_full_msg, sizeof(char *)) < 0)
        log_pipe_error();
  
    if (mato_core_config.print_all_logs_to_console)
        printf("%s %s: %s\n", log_type_str[log_type], thread_name, log_msg);
}

void mato_log_str2(int log_type, char *log_msg, const char *log_msg2, const char *log_msg3)
{
    if (!check_log_type(&log_type)) return;
    long run_time = get_run_time();
  
    char *thread_name = this_thread_name();
    char *log_full_msg = (char *)malloc(strlen(log_msg) + strlen(log_msg2) + strlen(log_msg3) + strlen(thread_name) + 20);
    sprintf(log_full_msg, "%05ld.%03d %s %s: %s %s %s\n", run_time / 1000L, (int)(run_time % 1000L), log_type_str[log_type], thread_name, log_msg, log_msg2, log_msg3);
    if (write(log_queue[1], &log_full_msg, sizeof(char *)) < 0)
        log_pipe_error();
  
    if (mato_core_config.print_all_logs_to_console)
        printf("%s %s: %s %s %s\n", log_type_str[log_type], thread_name, log_msg, log_msg2, log_msg3);
}

void mato_log_str(int log_type, char *log_msg, const char *log_msg2)
{
    if (!check_log_type(&log_type)) return;
    long run_time = get_run_time();
  
    char *thread_name = this_thread_name();
    char *log_full_msg = (char *)malloc(strlen(log_msg) + strlen(log_msg2) + strlen(thread_name) + 20);
    sprintf(log_full_msg, "%05ld.%03d %s %s: %s %s\n", run_time / 1000L, (int)(run_time % 1000L), log_type_str[log_type], thread_name, log_msg, log_msg2);
    if (write(log_queue[1], &log_full_msg, sizeof(char *)) < 0)
        log_pipe_error();
  
    if (mato_core_config.print_all_logs_to_console)
        printf("%s %s: %s %s\n", log_type_str[log_type], thread_name, log_msg, log_msg2);
}

void mato_log_str_val(int log_type, char *log_msg, const char *log_msg2, int val)
{
    if (!check_log_type(&log_type)) return;
    long run_time = get_run_time();
  
    char *thread_name = this_thread_name();
    char *log_full_msg = (char *)malloc(strlen(log_msg) + strlen(log_msg2) + strlen(thread_name) + 40);
    sprintf(log_full_msg, "%05ld.%03d %s %s: %s%s %d\n", run_time / 1000L, (int)(run_time % 1000L), log_type_str[log_type], thread_name, log_msg, log_msg2, val);
    if (write(log_queue[1], &log_full_msg, sizeof(char *)) < 0)
        log_pipe_error();
  
    if (mato_core_config.print_all_logs_to_console)
        printf("%s: %s %s%s %d\n", log_type_str[log_type], thread_name, log_msg, log_msg2, val);
}

void mato_log_val2(int log_type, char *log_msg, int val, int val2)
{
    if (!check_log_type(&log_type)) return;
    long run_time = get_run_time();

    char *thread_name = this_thread_name();
    char *log_full_msg = (char *)malloc(strlen(log_msg) + strlen(thread_name) + 40);
    sprintf(log_full_msg, "%05ld.%03d %s %s: %s %d %d\n", run_time / 1000L, (int)(run_time % 1000L), log_type_str[log_type], thread_name, log_msg, val, val2);
    if (write(log_queue[1], &log_full_msg, sizeof(char *)) < 0)
        log_pipe_error();

    if (mato_core_config.print_all_logs_to_console)
        printf("%s %s: %s %d %d\n", log_type_str[log_type], thread_name, log_msg, val, val2);
}

void mato_log_double2(int log_type, char *log_msg, double val, double val2)
{
    if (!check_log_type(&log_type)) return;
    long run_time = get_run_time();

    char *thread_name = this_thread_name();
    char *log_full_msg = (char *)malloc(strlen(log_msg) + strlen(thread_name) + 100);
    sprintf(log_full_msg, "%05ld.%03d %s %s: %s %16.8G %16.6G\n", run_time / 1000L, (int)(run_time % 1000L), log_type_str[log_type], thread_name, log_msg, val, val2);
    if (write(log_queue[1], &log_full_msg, sizeof(char *)) < 0)
        log_pipe_error();

    if (mato_core_config.print_all_logs_to_console)
        printf("%s %s: %s %e %e\n", log_type_str[log_type], thread_name, log_msg, val, val2);
}

void mato_log_val(int log_type, char *log_msg, int val)
{
    if (!check_log_type(&log_type)) return;
    long run_time = get_run_time();

    char *thread_name = this_thread_name();
    char *log_full_msg = (char *)malloc(strlen(log_msg) + strlen(thread_name) + 30);
    sprintf(log_full_msg, "%05ld.%03d %s %s: %s %d\n", run_time / 1000L, (int)(run_time % 1000L), log_type_str[log_type], thread_name, log_msg, val);
    if (write(log_queue[1], &log_full_msg, sizeof(char *)) < 0)
        log_pipe_error();

    if (mato_core_config.print_all_logs_to_console)
        printf("%s %s: %s %d\n", log_type_str[log_type], thread_name, log_msg, val);
}

void mato_log_double(int log_type, char *log_msg, double val)
{
    if (!check_log_type(&log_type)) return;
    long run_time = get_run_time();
  
    char *thread_name = this_thread_name();
    char *log_full_msg = (char *)malloc(strlen(log_msg) + strlen(thread_name) + 50);
    sprintf(log_full_msg, "%05ld.%03d %s %s: %s %16.8G\n", run_time / 1000L, (int)(run_time % 1000L), log_type_str[log_type], thread_name, log_msg, val);
    if (write(log_queue[1], &log_full_msg, sizeof(char *)) < 0)
        log_pipe_error();
  
    if (mato_core_config.print_all_logs_to_console)
        printf("%s %s: %s %e\n", log_type_str[log_type], thread_name, log_msg, val);
}

long long msec()
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return 1000L * tv.tv_sec + tv.tv_usec / 1000L;
}

long long usec()
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (1000000L * (long long)tv.tv_sec) + tv.tv_usec;
}


