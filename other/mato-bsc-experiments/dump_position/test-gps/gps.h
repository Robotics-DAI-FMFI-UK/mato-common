#ifndef GPS_H
#define GPS_H

#include <fcntl.h>
#include <termios.h>

//#define dev_name "/dev/navilock"
#define dev_name "/dev/gm3n"

extern int program_runs;
extern int threads_started;

typedef struct gps_pos 
{
	double latitude;
	double longitude;
} gps_position;


void init_gps();
void get_gps_data(gps_position *current_position);


#endif 

