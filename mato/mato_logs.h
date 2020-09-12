/// \file mato_logs.h
/// Mato control framework - debug logging to a file/console.
/// The listed functions are also part of the mato framework public interface.

#ifndef _MATO_LOGS_H_
#define _MATO_LOGS_H_

#define ML_MAX_TYPE 3
/// Each log message must contain a severity level: 
///   DEBUG = most detailed level of logs, can be specificaly disabled in init
#define ML_DEBUG 3
///   ERR = serious error occured
#define ML_ERR 2
///   WARN = unusual situation that needs to have attention
#define ML_WARN 1
///   INFO = documenting a usual processes flow
#define ML_INFO 0

/// Init must be called before the first message is logged. The file name is generated based on the time
/// the init is called (time in seconds since epoch), with the specified suffix.
/// \param print_all_logs_to_console when this flag is 0, nothing is printed out to the terminal, only to the file.
/// \param print_debug_logs 0 specifies that messages with the level ML_DEBUG are ignored
/// \param log_path points to a folder where the log file will be created, should not end with '/'
/// \param log_filename_suffix is the last part of the log filename, specify 0 for the default 'mato.log'.
void mato_logs_init(int print_all_logs_to_console, int print_debug_logs, const char *log_path, const char *log_filename_suffix);

/// Can be called to release some resources used by the logs when they are not going to be used anymore.
void mato_logs_shutdown();

/// Append a specified character string message with the specified severity level to the log.
void mato_log(unsigned int log_type, char *log_msg);

/// Append a specified character string followed by another character string 
/// with the specified severity level to the log.
void mato_log_str(unsigned int log_type, char *log_msg, const char *log_msg2);

/// Append the specified character string followed by a space and an integer to the log and with the
/// specified severity level string representation.
void mato_log_val(unsigned int log_type, char *log_msg, int val);

/// Append the specified two character strings followed by a space and an integer to the log, the 
/// message is prepended as all other messages with the current time to the precision of milliseconds, 
/// severity level, and thread name.
void mato_log_str_val(unsigned int log_type, char *log_msg, const char *log_msg2, int val);

/// Append the specified character string followed by two space-separated integer values to the log.
void mato_log_val2(unsigned int log_type, char *log_msg, int val, int val2);

/// Append the specified character string followed by a floating point value (in format %16.8G) to the log.
void mato_log_double(unsigned int log_type, char *log_msg, double val);

/// Append the specified character string followed by two space-separated floating point values to the log.
void mato_log_double2(unsigned int log_type, char *log_msg, double val, double val2);

#endif
