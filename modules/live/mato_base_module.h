#ifndef _BASE_MODULE_H
#define _BASE_MODULE_H

#include <pthread.h>
#include <math.h>

#define WHEEL_DIAMETER_IN_MM          157
#define WHEEL_PERIMETER_IN_MM         (int)(WHEEL_DIAMETER_IN_MM * M_PI)
#define COUNTER_TICKS_PER_REVOLUTION  144
#define WHEELS_DISTANCE 325

#define NO_AZIMUTH 400

// convert counter ticks to mm
#define COUNTER2MM(COUNTER) ((COUNTER) * WHEEL_PERIMETER_IN_MM / COUNTER_TICKS_PER_REVOLUTION)

// convert mm to counter ticks
#define MM2COUNTER(MM) ((MM) * COUNTER_TICKS_PER_REVOLUTION / WHEEL_PERIMETER_IN_MM)

typedef struct astruct {
  // microseconds since machine started
  unsigned long timestamp;
  // 144 ticks per revolution A=left, B=right
  long counterA, counterB;
  // current angular velocity in deg/s
  short velocityA, velocityB;
  // IR distance sensors
  short dist1, dist2, dist3;
  // cube presence IR sensor
  short cube;
  // last compass reading 0-360
  short heading;
  // normalized accelerometer (ms^-2)
  short ax, ay, az;
  // normalized gyroscope
  short gx, gy, gz;
} base_data_type;

typedef void (*base_receive_data_callback)(base_data_type *);

// open serial communication with arduino base board
void init_base_module();

// retrieve the latest sensor data from base board
void get_base_data(base_data_type *buffer);

// write base data to log file
void log_base_data(base_data_type* buffer);

// waits until the next sensor data from the base board have not been received
// since the serial line is full duplex, one packet could have been already on the way,
// therefore you need to call this twice, to be sure the next status reflects the last
// output packet
void wait_for_new_base_data();

// direct motor power control mode: start moving with the specified speeds
void set_motor_speeds(int left_motor, int right_motor);

// escape away and quick
void escape_now_and_quick();

// stop moving in every mode, also cancels azimuth and velocity modes
void stop_now();

// azimuth follow mode: start moving towards the specified azimuth (0-360), use the last specified speed of direct motor power control mode
void follow_azimuth(int azimuth);

// reset counterA, counterB counters in the base board at this moment
void reset_counters();

// velocity control mode: start moving so that the
void regulated_speed(int left_motor, int right_motor);

// cancel azimuth mode
void cancel_azimuth_mode();

// pause status reporting (sensory data will not be updated)
void pause_status_reporting();

// resume status reporting (start updating sensory data again)
void resume_status_reporting();

// configure laziness for velocity regulation mode
void set_laziness(unsigned char laziness);

// utility function to convert the angular counter to travelled distance in mm
short counter2mm(short counter);

// utility function to return the current followed azimuth or NO_AZIMUTH, if none
int get_current_azimuth();

// register for getting fresh data after received from base (copy quick!)
void register_base_callback(base_receive_data_callback callback);

// remove previously registered callback
void unregister_base_callback(base_receive_data_callback callback);

// shaking movement while unloading
void unloading_shake();

// block motors - afterwards all motor commands will be ignored
void set_motor_blocked(int blocked);

// get motor blocked state 
int get_motor_blocked(void);

// set the number of encoder ticks for automatic stop, call this before set_motor_speed(). applies once.
void set_max_ticks(int max_ticks);


#endif
