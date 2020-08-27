#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

#include "gps.h"


int program_runs;
int threads_started;
gps_position gps;

char log_file_name[50];

void append_to_log(time_t t)
{
	FILE *f;
	f = fopen(log_file_name, "a+");
	fprintf(f, "%ld: longitude: %.8lf, latitude: %.8lf\n", t,
				gps.longitude, gps.latitude);
	fclose(f);
}

int main()
{
	time_t t;
	time(&t);
	sprintf(log_file_name, "gps_%lu.log", (unsigned long)t);

	program_runs = 1;
	threads_started = 0;

	init_gps();
	while (1)
	{
		get_gps_data(&gps);
		time(&t);
		printf("%ld: longitude: %.8lf, latitude: %.8lf\n", t,
				gps.longitude, gps.latitude);
		append_to_log(t);
		sleep(1);
	}

	program_runs = 0;
	while (threads_started) usleep(100000);
	printf("all threads terminated\n");
	return 0;
}

