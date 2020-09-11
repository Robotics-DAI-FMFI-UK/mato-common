#ifndef _MATO_LOGS_H_
#define _MATO_LOGS_H_

#define ML_MAX_TYPE 3
#define ML_DEBUG 3
#define ML_ERR 2
#define ML_WARN 1
#define ML_INFO 0

void mato_logs_init(int print_all_logs_to_console, int print_debug_logs, const char *log_path);
void mato_logs_shutdown();
void mato_log(unsigned int log_type, char *log_msg);
void mato_log_str(unsigned int log_type, char *log_msg, const char *log_msg2);
void mato_log_val(unsigned int log_type, char *log_msg, int val);
void mato_log_str_val(unsigned int log_type, char *log_msg, const char *log_msg2, int val);
void mato_log_val2(unsigned int log_type, char *log_msg, int val, int val2);
void mato_log_double(unsigned int log_type, char *log_msg, double val);
void mato_log_double2(unsigned int log_type, char *log_msg, double val, double val2);

#endif
