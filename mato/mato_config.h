/// \file mato_config.h
/// Mato control framework - public interface for dealing with var:val style config files.

#ifndef _MATO_CONFIG_H_
#define _MATO_CONFIG_H_

/// Read configuration file to memory and parse it.
void *mato_config_read(char *filename);

/// Retrieve a string value for the specified variable - a pointer to a character string 
/// that will be deallocated when mato_config_dispose() function is called.
/// If such a variable is not present in the config file, the provided default value will be returned.
char *mato_config_get_strval(void *config, char *var_name, char *default_value);

/// Retrieve a string value for the specified variable in a newly allocated string that can be released using free() function.
/// If such a variable is not present in the config file, the provided default value will be returned.
char *mato_config_get_alloc_strval(void *config, char *var_name, char *default_value);

/// Retrieve an int value for the specified variable.
/// If such a variable is not present in the config file, the provided default value will be returned.
int mato_config_get_intval(void *config, char *var_name, int default_value);

/// Retrieve a double value for the specified variable.
/// If such a variable is not present in the config file, the provided default value will be returned.
double mato_config_get_doubleval(void *config, char *var_name, double default_value);

/// Release config data from the memory.
void mato_config_dispose(void *config);

#endif

