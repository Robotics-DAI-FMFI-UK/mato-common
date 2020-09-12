#include "../../bites/mikes.h"
#include "base_module.h"
#include "../passive/mikes_logs.h"
#include "../passive/pose.h"
#include "../../bites/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <math.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>

#define MAX_PACKET_LENGTH 200

#define MAX_BASE_CALLBACKS 20

static base_receive_data_callback callbacks[MAX_BASE_CALLBACKS];
static int callbacks_count;

static int current_azimuth;

static pthread_mutex_t base_module_lock;

static int fdR[2];
static int fdW[2];

static pid_t plink_child;

static volatile unsigned char new_base_data_arrived;
static unsigned char base_initialized;

static int base_motor_blocked = 0;

void connect_base_module()
{
    if (pipe(fdR) < 0)
    {
        mikes_log(ML_ERR, "base: pipe2()");
        base_initialized = 0;
        return;
    }
    if (pipe(fdW) < 0)
    {
        mikes_log(ML_ERR, "base: pipe2()");
        base_initialized = 0;
        return;
    }

    if ((plink_child = fork()) == 0)
    {
        /* child */

        close(0);
        close(1);
        dup2(fdR[0], 0);
        dup2(fdW[1], 1);
        close(fdR[0]);
        close(fdR[1]);
        close(fdW[0]);
        close(fdW[1]);

        if (execl("/usr/bin/plink", "/usr/bin/plink", "/dev/base",
                  "-serial", "-sercfg", "115200,N,n,8,1", NULL) < 0)
        {
            mikes_log(ML_ERR, "base: child execl()");
            base_initialized = 0;
            return;
        }
    }

    if (plink_child < 0)
    {
        mikes_log(ML_ERR, "base: child execl()");
        base_initialized = 0;
        return;
    }

    close(fdR[0]);
    close(fdW[1]);
    if (fcntl( fdW[0], F_SETFL, fcntl(fdW[0], F_GETFL) | O_NONBLOCK) < 0)
    {
        mikes_log(ML_ERR, "base: setting nonblock on read pipe end");
        base_initialized = 0;
    }

    mikes_log(ML_INFO, "base module connected");
    current_azimuth = NO_AZIMUTH;
    base_initialized = 1;
}

#define BASE_LOGSTR_LEN 1024

void log_base_data(base_data_type* buffer)
{
    char str[BASE_LOGSTR_LEN];

    sprintf(str, "[main] base_module::log_base_data(): timestamp=%lu, counterA=%ld, counterB=%ld, velocityA=%d, velocityB=%d, dist1=%d, dist2=%d, dist3=%d, cube=%d, heading=%d, ax=%d, ay=%d, az=%d, gx=%d, gy=%d, gz=%d",
        buffer->timestamp, buffer->counterA, buffer->counterB, buffer->velocityA, buffer->velocityB,
        buffer->dist1, buffer->dist2, buffer->dist3, buffer->cube, buffer->heading,
        buffer->ax, buffer->ay, buffer->az, buffer->gx, buffer->gy, buffer->gz);

    mikes_log(ML_DEBUG, str);
}

base_data_type local_data;

void read_base_packet()
{
    unsigned char ch;
    int numRead;

    do {
		ch = 0;
        if ((numRead = read(fdW[0], &ch, 1)) < 0)
        {
            if (errno != EAGAIN)
            {
                perror("read()");
                mikes_log(ML_ERR, "read from base err");
                exit(-1);
            }
            else usleep(2000);
        }
    } while (program_runs && (ch != '$'));

    unsigned char more_packets_in_queue = 0;
    char line[1024];
    do {
        int lnptr = 0;
        do {
          if ((numRead = read(fdW[0], line + lnptr, 1)) < 0)
          {
              if (errno != EAGAIN)
              {
                  perror("read()");
                  mikes_log(ML_ERR, "read from base err2");
                  exit(-1);
              }
              else { usleep(2000); continue; }
          }
          lnptr += numRead;
          if (lnptr > 1023) break;
          if (lnptr == 0) continue;
        } while (program_runs && (line[lnptr - 1] != '\n'));

        if (lnptr > 0) line[lnptr - 1] = 0;

        more_packets_in_queue = 0;
        while (program_runs)
        {
			ch = 0;
            if (read(fdW[0], &ch, 1) < 0)
            {
                if (errno == EAGAIN) break;
                else
                {
                    perror("read()");
                    exit(-1);
                }
            }
            if (ch == '$')
            {
                more_packets_in_queue = 1;
                break;
            }
        }
    } while (program_runs && more_packets_in_queue);

    pthread_mutex_lock(&base_module_lock);
    sscanf(line, "%lu%ld%ld%hd%hd%hd%hd%hd%hd%hd%hd%hd%hd%hd%hd%hd",
                                  &(local_data.timestamp),
                                  &(local_data.counterA), &(local_data.counterB), &(local_data.velocityA),
                                  &(local_data.velocityB), &(local_data.dist1), &(local_data.dist2),
                                  &(local_data.dist3), &(local_data.cube), &(local_data.heading ),
                                  &(local_data.ax), &(local_data.ay), &(local_data.az),
                                  &(local_data.gx), &(local_data.gy), &(local_data.gz));

    //printf("%s\n", line);
    new_base_data_arrived = 1;
    //printf("CA: %ld,  CB: %ld\n", local_data.counterA, local_data.counterB);
    pthread_mutex_unlock(&base_module_lock);
    //mikes_log(ML_INFO, line);

    for (int i = 0; i < callbacks_count; i++)
      callbacks[i](&local_data);
}

void wait_for_new_base_data()
{
    new_base_data_arrived = 0;
    while (!new_base_data_arrived) usleep(1000);
}

void set_motor_speeds_ex(int left_motor, int right_motor)
{
    char cmd[40];
    int lm = abs(left_motor);
    int rm = abs(right_motor);
    sprintf(cmd, "@M%c%1d%1d%c%1d%1d", ((left_motor > 0)?' ':'-'),
                                     ((lm / 10) % 10),
                                     (lm % 10),
                                     ((right_motor > 0)?' ':'-'),
                                     (rm / 10) % 10,
                                     (rm % 10));
    if (write(fdR[1], cmd, strlen(cmd)) < strlen(cmd))
    {
       perror("mikes:base");
       mikes_log(ML_ERR, "base: could not send command");
    }
}

void set_motor_speeds(int left_motor, int right_motor)
{
    if (base_motor_blocked)  return;   
    
    set_motor_speeds_ex(left_motor, right_motor);
}

#define BASE_ESCAPE_MOTOR_SPEED 22

void escape_now_and_quick()
{
    set_motor_speeds_ex(-BASE_ESCAPE_MOTOR_SPEED, -BASE_ESCAPE_MOTOR_SPEED*6/10);
    usleep(4000000);
    stop_now();
}

void stop_now()
{
    if (write(fdR[1], "@S", 2) < 2)
    {
       perror("mikes:base");
       mikes_log(ML_ERR, "base: could not send command");
    }
    current_azimuth = NO_AZIMUTH;
}

void follow_azimuth(int azimuth)
{
    if (base_motor_blocked) return;

    char cmd[20];
    current_azimuth = azimuth;
    sprintf(cmd, "@A%d%d%d", azimuth / 100, (azimuth % 100) / 10, azimuth % 10);
    if (write(fdR[1], cmd, strlen(cmd)) < strlen(cmd))
    {
      perror("mikes:base");
      mikes_log(ML_ERR, "base: could not send azimuth");
    }
}

int get_current_azimuth()
{
    return current_azimuth;
}

void reset_counters()
{
    if (write(fdR[1], "@R", 2) < 2)
    {
      perror("mikes:base");
      mikes_log(ML_ERR, "base: could not reset counters");
    }
    wait_for_new_base_data();
    wait_for_new_base_data();
}

void regulated_speed(int left_motor, int right_motor)
{
    if (base_motor_blocked) return;

    char cmd[40];
    int lm = abs(left_motor);
    int rm = abs(right_motor);

    sprintf(cmd, "@V%c%d%d%d%c%d%d%d", ((left_motor > 0)?' ':'-'),
                                     (lm / 100) % 10,
                                     (lm / 10) % 10,
                                     (lm % 10),
                                     (right_motor > 0)?' ':'-',
                                     (rm / 100) % 10,
                                     (rm / 10) % 10,
                                     (rm % 10));
    if (write(fdR[1], cmd, strlen(cmd)) < strlen(cmd))
    {
      perror("mikes:base");
      mikes_log(ML_ERR, "base: could not send regulated speed");
    }
    current_azimuth = NO_AZIMUTH;
}

void *base_module_thread(void *args)
{
	//int i = 0;
	//pose_type pose;

    while (program_runs)
    {
        read_base_packet();
        //printf("new packet: %ld %ld\n", local_data.counterA, local_data.counterB);
        update_pose(&local_data);
        //i++;
        //if (i == 50) {

			//get_pose(&pose);
			//get_grid_location(pose);
			//i = 0;
		//}
        usleep(10000);
    }

    mikes_log(ML_INFO, "base quits.");
    stop_now();
    usleep(100000);
    kill(plink_child, SIGTERM);
    threads_running_add(-1);
    return 0;
}

void init_base_module()
{
    pthread_t t;
    base_initialized = 0;
    connect_base_module();
    if (!base_initialized) return;
    pthread_mutex_init(&base_module_lock, 0);
    if (pthread_create(&t, 0, base_module_thread, 0) != 0)
    {
      perror("mikes:base");
      mikes_log(ML_ERR, "creating thread for base module");
      base_initialized = 0;
    }
    else threads_running_add(1);
}


void get_base_data(base_data_type* buffer)
{
    pthread_mutex_lock(&base_module_lock);
    memcpy(buffer, &local_data, sizeof(base_data_type));
    pthread_mutex_unlock(&base_module_lock);
}

void cancel_azimuth_mode()
{
    if (write(fdR[1], "@X", 2) < 2)
    {
      perror("mikes:base");
      mikes_log(ML_ERR, "base: could not cancel azimuth mode");
    }
    current_azimuth = NO_AZIMUTH;
}

void pause_status_reporting()
{
    if (write(fdR[1], "@-", 2) < 2)
    {
      perror("mikes:base");
      mikes_log(ML_ERR, "base: could not pause reporting");
    }
}

void resume_status_reporting()
{
    if (write(fdR[1], "@+", 2) < 2)
    {
      perror("mikes:base");
      mikes_log(ML_ERR, "base: could not resume reporting");
    }
}

void set_laziness(unsigned char laziness)
{
    char cmd[10];
    sprintf(cmd, "@L %d%d", laziness / 10, laziness % 10);
    if (write(fdR[1], cmd, strlen(cmd)) < strlen(cmd))
    {
      perror("mikes:base");
      mikes_log(ML_ERR, "base: could not set laziness");
    }
}

void register_base_callback(base_receive_data_callback callback)
{
  if (callbacks_count == MAX_BASE_CALLBACKS)
  {
     mikes_log(ML_ERR, "too many base callbacks");
     return;
  }
  callbacks[callbacks_count] = callback;
  callbacks_count++;
}

void unregister_base_callback(base_receive_data_callback callback)
{
  for (int i = 0; i < callbacks_count; i++)
    if (callbacks[i] == callback)
    {
       callbacks[i] = callbacks[callbacks_count - 1];
       callbacks_count--;
    }
}

void unloading_shake()
{
  for (int i = 0; i < 15; i++)
  {
    set_motor_speeds(-30, 30);
    usleep(80000);
    set_motor_speeds(30, -30);
    usleep(80000);
    set_motor_speeds(0, 0);
    usleep(160000);
  }
}

void set_motor_blocked(int blocked)
{
    if (!base_motor_blocked && blocked) {
        stop_now();
    }
    base_motor_blocked = blocked;

    char str[BASE_LOGSTR_LEN];
    sprintf(str, "[main] base_module::set_motor_blocked(): blocked=%d", blocked);
    mikes_log(ML_INFO, str);
}

int get_motor_blocked(void)
{
    return base_motor_blocked;
}

void set_max_ticks(int max_ticks)
{
    char cmd[10];
    max_ticks = abs(max_ticks);
    sprintf(cmd, "@D%d%d%d", max_ticks / 100, (max_ticks % 100) / 10, max_ticks % 10);
    if (write(fdR[1], cmd, strlen(cmd)) < strlen(cmd))
    {
      perror("mikes:base");
      mikes_log(ML_ERR, "base: could not send maxticks");
    }
}
