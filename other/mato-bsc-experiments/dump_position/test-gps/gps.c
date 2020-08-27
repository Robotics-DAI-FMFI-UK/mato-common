#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "gps.h"


static int gps;
static int bufp;
static char b[1024];
static char b2[1024];

static gps_position last_valid_position;

void get_gps_data(gps_position *current_position)
{
	*current_position = last_valid_position;
}


double nmea_to_decimal(char *position) 
{
    double pos;
    sscanf(position, "%lf,", &pos);

    double deg = floor(pos / 100);
    double dec_pos = deg + ((pos - (deg * 100)) / 60);

    return dec_pos;
}

int is_GPGGA(char *sentence) 
{
    if ((strncmp(sentence, "$GPGGA", 6) != 0) &&
	(strncmp(sentence, "$GNGGA", 6) != 0))
       return 0;

    int commas = 0;
    while (*sentence)
    {
	    if (*(sentence++) == ',') 
	    {
		    commas++;
		    if (commas == 6) break;
	    }
    }
    if (commas == 6) 
	    if (*sentence != '1') return 0;
    return 1;
}

char *after_n_commas(char *s, int n)
{
  while (*s)
  {
    if (*(s++) == ',') 
    {
	    n--;
	    if  (n == 0) return s;
    }
  }
  return 0;
}


void parse_gps_line() 
{
    if (is_GPGGA(b2)) 
    {
       // printf("%s\n", b2);
	char *latitude = after_n_commas(b2, 2);
	char *longitude = after_n_commas(b2, 4);
        last_valid_position.latitude = nmea_to_decimal(latitude);
        last_valid_position.longitude = nmea_to_decimal(longitude);
    }
}

void *gps_thread(void *arg) 
{
    threads_started++;
    while (program_runs) {
        ssize_t nread;
        if (!(nread = read(gps, b + bufp, 1))) {
            printf("gps read() returned 0, gps disconnected?\n");
	    close(gps);
            break;
        }
        if (nread < 0) {
            usleep(3000);
	    continue;
        }
        if (b[bufp] == '\n') {
            b[bufp] = '\0';
            bufp = 0;
            strncpy(b2, b, 1023);
            b2[1023] = '\0';

            parse_gps_line();
        } else if (bufp < 1023) {
            bufp++;
        } else {
            bufp = 0;
        }
    }
    threads_started--;
}


void init_gps() 
{
    struct termios oldtio, newtio;

    gps = open(dev_name, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (gps < 0) {
        printf("GPS not found at %s", dev_name);
        return;
    }

    tcgetattr(gps, &oldtio); /* save current port settings */

    bzero(&newtio, sizeof(newtio));
    newtio.c_cflag = B4800 | CS8 | CLOCAL | CREAD; // B57600
    newtio.c_iflag = IGNPAR;
    newtio.c_oflag = 0;

    /* set input mode (non-canonical, no echo,...) */
    newtio.c_lflag = 0;

    newtio.c_cc[VTIME] = 0; /* inter-character timer unused */
    newtio.c_cc[VMIN] = 1;  /* blocking read until 1 char received */

    tcflush(gps, TCIFLUSH);
    tcsetattr(gps, TCSANOW, &newtio);

    do {
        if (!read(gps, b, 1))
            break;
    } while (b[0] != '\n');

    pthread_t t;
    if (pthread_create(&t, 0, gps_thread, 0) != 0)
    {
       perror("could not create gps thread");
    }
}


