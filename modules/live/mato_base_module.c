#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <math.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <math.h>

#include "../../mato/mato.h"
#include "mato_base_module.h"
#include "core/config_mato.h"

#define MAX_PACKET_LENGTH 200

/// packets from the arduino have the following form:
/// ~~~~
/// @timestamp left_steps right_steps us0 us1 us2 us3 red_switch obstacle
///
/// example:
///   @43540 164569 164569 123 129 102 116 0 0
///
/// timestamp: uint32_t [ms] since arduino reset
/// left_steps, right_steps: int32_t, 6000 per revolution
/// us0-us3: int16_t [cm]
/// red_switch: 0/1
/// obstacle: 0/1

typedef struct {
    int module_id;
    pthread_mutex_t base_module_lock;
    int fdR[2];
    int fdW[2];
    pid_t plink_child;
    volatile unsigned char base_initialized;
    volatile int base_motor_blocked;
} mato_base_instance_data;

static void connect_base_module(mato_base_instance_data *data)
{
    if (pipe(data->fdR) < 0)
    {
        mato_log_val(ML_ERR, "base: pipe()", errno);
        data->base_initialized = 0;
        return;
    }
    if (pipe(data->fdW) < 0)
    {
        mato_log_val(ML_ERR, "base: pipe()", errno);
        data->base_initialized = 0;
        return;
    }

    mato_log_str2(ML_DEBUG, "starting plink with", mato_config.base_device, mato_config.base_serial_config);

    if ((data->plink_child = fork()) == 0)
    {
        /* child */

        close(0);
        close(1);
        dup2(data->fdR[0], 0);
        dup2(data->fdW[1], 1);
        close(data->fdR[0]);
        close(data->fdR[1]);
        close(data->fdW[0]);
        close(data->fdW[1]);

        if (execl("/usr/bin/plink", "/usr/bin/plink", mato_config.base_device,
                  "-serial", "-sercfg", mato_config.base_serial_config, NULL) < 0)
        {
            mato_log_val(ML_ERR, "base: child execl(), errno:", errno);
            data->base_initialized = 0;
            return;
        }
    }

    if (data->plink_child < 0)
    {
        mato_log(ML_ERR, "base: child execl()");
        data->base_initialized = 0;
        return;
    }

    close(data->fdR[0]);
    close(data->fdW[1]);
    if (fcntl( data->fdW[0], F_SETFL, fcntl(data->fdW[0], F_GETFL) | O_NONBLOCK) < 0)
    {
        mato_log(ML_ERR, "base: setting nonblock on read pipe end");
        data->base_initialized = 0;
    }

    mato_log(ML_INFO, "base module connected");
    data->base_initialized = 1;
}

#define BASE_LOGSTR_LEN 1024

void log_base_data(base_data_type* buffer)
{
    char str[BASE_LOGSTR_LEN];

    sprintf(str, "base tm=%" PRIu32 ", left=%" PRIi32 ", right=%" PRIi32 ", dist0=%" PRIi16 ", dist1=%" PRIi16 "dist2=%" PRIi16 ", dist3=%" PRIi16 ", switch=%" PRIu8 ", obstacle=%" PRIu8,
        buffer->timestamp, buffer->left, buffer->right, buffer->dist0, buffer->dist1, buffer->dist2, buffer->dist3, buffer->red_switch, buffer->obstacle);

    mato_log(ML_DEBUG, str);
}

static void flush_read_buffer(mato_base_instance_data *data)
{
    unsigned char ch = 0;
    int cnt = 0;
    while (read(data->fdW[0], &ch, 1) > 0) cnt++;
    mato_log_val(ML_DEBUG, "flushed input chars", cnt);
}

static int read_base_packet(mato_base_instance_data *data, base_data_type *packet)
{
    unsigned char ch;
    int numRead;

    do {
		ch = 0;
        if ((numRead = read(data->fdW[0], &ch, 1)) < 0)
        {
            if (errno != EAGAIN)
            {
                mato_log_val(ML_WARN, "could not read from base - terminating?, errno:", errno);
                return 0;
            }
            else usleep(2000);
        }
    } while (program_runs && (ch != '@'));

    unsigned char more_packets_in_queue = 0;
    char line[1024];
    do {
        int lnptr = 0;
        do {
            if ((numRead = read(data->fdW[0], line + lnptr, 1)) < 0)
            {
                if (errno != EAGAIN)
                {
                    mato_log_val(ML_ERR, "read from base - terminating?", errno);
                    return 0;
                }
                else { usleep(2000); continue; }
            }
            lnptr += numRead;
            if (lnptr > 1023) break;
            if (lnptr == 0) continue;
        } while (program_runs && (line[lnptr - 1] != '\n'));

        while ((lnptr > 0) && ((line[lnptr - 1] == 13) || (line[lnptr - 1] == 10))) line[--lnptr] = 0;

        more_packets_in_queue = 0;
        while (program_runs)
        {
            ch = 0;
            if (read(data->fdW[0], &ch, 1) < 0)
            {
                if (errno == EAGAIN) break;
                else
                {
                    mato_log_val(ML_ERR, "read from base - termination?", errno);
                    return 0;
                }
            }
            if (ch == '$')
            {
                more_packets_in_queue = 1;
                break;
            }
        }
    } while (program_runs && more_packets_in_queue);

    pthread_mutex_lock(&data->base_module_lock);
    sscanf(line, "%" SCNu32 "%" SCNi32 "%" SCNi32 "%" SCNi16 "%" SCNi16 "%" SCNi16 "%" SCNi16 "%" SCNu8 "%" SCNu8,
                                  &(packet->timestamp),
                                  &(packet->left), &(packet->right), 
                                  &(packet->dist0), &(packet->dist1), 
                                  &(packet->dist2), &(packet->dist3),
                                  &(packet->red_switch),
                                  &(packet->obstacle));

    //printf("%s\n", line);
    //printf("CA: %ld,  CB: %ld\n", local_data.counterA, local_data.counterB);
    pthread_mutex_unlock(&data->base_module_lock);
    mato_log(ML_DEBUG, line);
    return 1;
}

static void set_motor_speeds_ex(mato_base_instance_data *data, int left_motor, int right_motor)
{
    char cmd[40];
    int lm = abs(left_motor);
    int rm = abs(right_motor);
    cmd[0] = 'S';
    int i = 2;
    if (left_motor < 0) cmd[1] = '-';
    else i = 1;
    sprintf(cmd + i, "%1d%1d", ((lm / 10) % 10), (lm % 10));
    i += 2;
    if (right_motor < 0) 
    {
        cmd[i] = '-'; 
        i++;
    }
    sprintf(cmd + i, "%1d%1d", ((rm / 10) % 10), (rm % 10));

    if (write(data->fdR[1], cmd, strlen(cmd)) < strlen(cmd))
    {
        mato_log_val(ML_ERR, "base: could not send command", errno);
    }
}

static void set_motor_speeds(mato_base_instance_data *data, int left_motor, int right_motor)
{
    if (data->base_motor_blocked)  return;   
    mato_log_val2(ML_DEBUG, "set speed", left_motor, right_motor);
    set_motor_speeds_ex(data, left_motor, right_motor);
}

static void stop_now(mato_base_instance_data *data)
{
    if (write(data->fdR[1], "x", 1) < 1)
    {
        mato_log_val(ML_ERR, "base: could not send command", errno);
    }
    else mato_log(ML_DEBUG, "stop now");
}

static void reset_counters(mato_base_instance_data *data)
{
    if (write(data->fdR[1], "r", 1) < 1)
    {
      mato_log_val(ML_ERR, "base: could not reset counters", errno);
    }
    else mato_log(ML_DEBUG, "reset");
}

static void set_motor_blocked(mato_base_instance_data *data, int blocked)
{
    if (!data->base_motor_blocked && blocked) {
        stop_now(data);
    }
    data->base_motor_blocked = blocked;

    char str[BASE_LOGSTR_LEN];
    sprintf(str, "[main] base_module::set_motor_blocked(): blocked=%d", blocked);
    mato_log(ML_INFO, str);
}

static void *base_module_thread(void *instance_data)
{
	//int i = 0;
	//pose_type pose;
    mato_base_instance_data *data = (mato_base_instance_data *)instance_data;
    mato_inc_thread_count("base"); 
    usleep(1600000);
    flush_read_buffer(data);

    while (program_runs)
    {
        base_data_type *packet = (base_data_type *)mato_get_data_buffer(sizeof(base_data_type));
        if (!read_base_packet(data, packet)) break;
        mato_post_data(data->module_id, 0, sizeof(base_data_type), packet);
    }

    mato_log(ML_INFO, "base quits.");
    stop_now(data);
    usleep(100000);
    kill(data->plink_child, SIGTERM);
    close(data->fdR[1]);
    data->base_initialized = 0;
    mato_dec_thread_count();
    return 0;
}

static void *mato_base_create_instance(int module_id)
{
    mato_base_instance_data *data = (mato_base_instance_data *)malloc(sizeof(mato_base_instance_data));
    data->module_id = module_id;
    data->base_motor_blocked = 0;
    return data;
}

static void mato_base_delete(void *instance_data)
{
    mato_base_instance_data *data = (mato_base_instance_data *)instance_data;
    if (data->base_initialized) 
    {
        close(data->fdW[0]);
        data->base_initialized = 2;
        while (data->base_initialized) usleep(1000);
    }
    free(instance_data);
    mato_log(ML_DEBUG, "mato base deleted");
}

static void mato_base_message(void *instance_data, int module_id_sender, int message_id, int msg_length, void *message_data)
{
    mato_base_instance_data *data = (mato_base_instance_data *)instance_data;
    if (!data->base_initialized) return;

    switch (message_id) {
    case MATO_BASE_MSG_SET_SPEED:
        if (msg_length != 2) return;
        int8_t left = *((int8_t *)message_data);
        int8_t right = *((int8_t *)message_data + 1);
        set_motor_speeds(data, left, right);
        break;
    case MATO_BASE_MSG_STOP_NOW:
        stop_now(data);
        break;
    case MATO_BASE_MSG_RESET_COUNTERS:
        reset_counters(data);
        break;
    case MATO_BASE_BLOCK_MOTORS:
        if (msg_length != 1) return;
        uint8_t block = *((uint8_t *)message_data);
        set_motor_blocked(data, block);
        break;
    default:
        mato_log_val(ML_WARN, "unkown message id", message_id);
    }
}

static void mato_base_start(void *instance_data)
{
    mato_base_instance_data *data = (mato_base_instance_data *)instance_data;
    pthread_t t;
    data->base_initialized = 0;
    connect_base_module(data);
    if (!data->base_initialized) return;
    pthread_mutex_init(&data->base_module_lock, 0);
    if (pthread_create(&t, 0, base_module_thread, instance_data) != 0)
    {
        mato_log_val(ML_ERR, "creating thread for base module", errno);
        data->base_initialized = 0;
    }
    mato_log(ML_DEBUG, "mato base started");
}

module_specification mato_base_specification = { mato_base_create_instance, mato_base_start, mato_base_delete, mato_base_message, 1 };

void init_mato_base_module()
{
    mato_register_new_type_of_module("mato_base", &mato_base_specification);
}

