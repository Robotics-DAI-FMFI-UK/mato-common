#ifndef _MATO_BASE_MODULE_H
#define _MATO_BASE_MODULE_H

#include <inttypes.h>

/// set speed to the motors, expects 2 signed 8-bit integers (left, right), valid range: -99..99, 0 means stop and then power off
#define MATO_BASE_MSG_SET_SPEED      1

/// stop motors quickly (no data)
#define MATO_BASE_MSG_STOP_NOW       2

/// reset the left, right stepper counters to 0 at this moment (no data)
#define MATO_BASE_MSG_RESET_COUNTERS 3

/// disable or enable both motors, all motor commands will be ignored until enabled, expects single byte, 1 = block, 0 = unblock.
#define MATO_BASE_BLOCK_MOTORS       4

#define WHEEL_DIAMETER_IN_MM          285
#define WHEEL_PERIMETER_IN_MM         (int32_t)(WHEEL_DIAMETER_IN_MM * M_PI + 0.5)
#define COUNTER_TICKS_PER_REVOLUTION  6000
#define WHEELS_DISTANCE 675

/// convert counter ticks to mm
#define COUNTER2MM(COUNTER) ((COUNTER) * WHEEL_PERIMETER_IN_MM / COUNTER_TICKS_PER_REVOLUTION)

/// convert mm to counter ticks
#define MM2COUNTER(MM) ((MM) * COUNTER_TICKS_PER_REVOLUTION / WHEEL_PERIMETER_IN_MM)

/// base module output channel message format
typedef struct 
{
    /// milliseconds since arduino reset
    uint32_t timestamp;
    /// left wheel steps, 6000 ticks per revolution
    int32_t left;
    /// right wheel steps, 6000 ticks per revolution
    int32_t right;
    // US distance sensors: leftmost
    int16_t dist0;
    // US distance sensors: center-left
    int16_t dist1;
    // US distance sensors: center-right
    int16_t dist2; 
    // US distance sensors: rightmost
    int16_t dist3;
    // red switch pressed
    uint8_t red_switch;
    // obstacle blocking movement
    uint8_t obstacle;
} base_data_type;

// open serial communication with arduino base board
void init_mato_base_module();

// write base data to log file
void log_base_data(base_data_type* buffer);

#endif
