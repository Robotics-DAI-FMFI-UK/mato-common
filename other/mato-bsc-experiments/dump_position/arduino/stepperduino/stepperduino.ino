#include <limits.h>

// logical motor ids444
#define MOTOR_LEFT 1
#define MOTOR_RIGHT 2

// arduino pins for connecting stepper driver MB450A
#define ENABLE_LEFT   10  
#define ENABLE_RIGHT  11

#define LEFT_STEP  3
#define LEFT_DIR   4

#define RIGHT_STEP 5
#define RIGHT_DIR  6

#define RED_SWITCH 12

#define US1_TRIG 14  
#define US1_ECHO 17   // => PCINT11

#define US2_TRIG 15  
#define US2_ECHO 16   // => PCINT10

// instead of digitalWrite() we access the ports directly
// from the ISR for efficiency reasons, so again:

#define LEFT_STEP_PORT  PORTD
#define LEFT_STEP_PIN   3

#define RIGHT_STEP_PORT PORTD 
#define RIGHT_STEP_PIN 5 

// red switch press to fullstop time [ms]
#define STOP_FAST_MAX_MS 300

// when obstacle detected in front of ultrasonic sensors, stop [cm]
#define CRITICAL_DISTANCE 50

// how often to report steps counts while moving (can be removed)
#define PRINT_PERIOD 200

#define US_MEASUREMENT_PERIOD 50

// change of speed on single key-press
int ONE_PRESS_DELTA = 50;

// next time we print steps counts
unsigned long next_position;

// rotation counters for both motors (steps) can be positive and negative
volatile int32_t left_position;
volatile int32_t right_position;

// timer counter for generating pusles on left and right motors
volatile uint16_t left_counter;
volatile uint16_t right_counter;

// are we now in pulse high? (for left and right motors)
volatile uint8_t left_in_high_pulse;
volatile uint8_t right_in_high_pulse;

// current motor directions
volatile int8_t left_dir;
volatile int8_t right_dir;

// number of ticks of timer to wait between two pulses for each motor
volatile uint16_t left_wait_for_0_to_1;
volatile uint16_t right_wait_for_0_to_1;

// last requested speed for each motor
int16_t left_speed;
int16_t right_speed;

volatile uint16_t us1_dist, us2_dist;

volatile uint8_t obstacle_blocking = 0;
volatile uint8_t motors_disabled = 0;

void enable_steppers()
{
  digitalWrite(ENABLE_LEFT, HIGH);
  digitalWrite(ENABLE_RIGHT, HIGH);
}

void  disable_steppers()
{
  digitalWrite(ENABLE_LEFT, LOW);
  digitalWrite(ENABLE_RIGHT, LOW);  
}

// timer1 compare match occurs every 4 usec
ISR(TIMER1_COMPA_vect) 
{  
  left_counter--;
  
  if (left_counter == 0)
  {
    if (left_in_high_pulse)
    {  
               
      LEFT_STEP_PORT &= ~(1 << LEFT_STEP_PIN);  //digitalWrite(LEFT_STEP, LOW);     
      left_in_high_pulse = 0;
      left_counter = left_wait_for_0_to_1;
      left_position += left_dir;
    }
    else
    {
      LEFT_STEP_PORT |= (1 << LEFT_STEP_PIN);   //digitalWrite(LEFT_STEP, HIGH);
      left_in_high_pulse = 1;
      left_counter = 1;
    } 
  }

  right_counter--;
  if (right_counter == 0)
  {
    if (right_in_high_pulse)
    {
      RIGHT_STEP_PORT &= ~(1 << RIGHT_STEP_PIN);  //digitalWrite(RIGHT_STEP, LOW);       
      right_in_high_pulse = 0;
      right_counter = right_wait_for_0_to_1;
      right_position += right_dir;
    }
    else
    {
      RIGHT_STEP_PORT |= (1 << RIGHT_STEP_PIN);  //digitalWrite(RIGHT_STEP, HIGH);
      right_in_high_pulse = 1;
      right_counter = 1;
    }
  }
}

static uint8_t pci1_pinc;
static uint8_t pci1_xored_pinc;
static uint8_t pci1_old_pinc = 0;
static uint32_t us1_start_pulse, us2_start_pulse;

ISR(PCINT1_vect)
{
   // PC2 = arduino pin 16  - US2_ECHO
   // PC3 = arduino pin 17  - US1_ECHO
  pci1_pinc = PINC;
  pci1_xored_pinc = pci1_pinc ^ pci1_old_pinc;
  if (pci1_xored_pinc & 0b100)
  {
    if (pci1_pinc & 0b100)  // raising edge on ECHO of US2 
      us2_start_pulse = micros();
    else // falling edge on ECHO of US2
    {
      if (micros() > us2_start_pulse) 
          //us2_dist = 255;
          us2_dist = (micros() - us2_start_pulse) / 58;
    }
  }
  if (pci1_xored_pinc & 0b1000)
  {
    if (pci1_pinc & 0b1000)  // raising edge on ECHO of US1 
      us1_start_pulse = micros();
    else // falling edge on ECHO of US1
    {
      if (micros() > us1_start_pulse)
        us1_dist = (micros() - us1_start_pulse) / 58;
    }    
  }
  pci1_old_pinc = pci1_pinc;
}

void both_us_send_pulse()
{
  digitalWrite(US1_TRIG, HIGH);
  digitalWrite(US2_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(US1_TRIG, LOW);
  digitalWrite(US2_TRIG, LOW);
}

void us1_send_pulse()
{
  digitalWrite(US1_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(US1_TRIG, LOW);
}

void us2_send_pulse()
{
  digitalWrite(US2_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(US2_TRIG, LOW);
}

void stepper_speed(uint8_t motor, int speed)
{
  if (motor == MOTOR_LEFT)
  {
    if (speed > 0)
    {
      left_dir = 1;
      digitalWrite(LEFT_DIR, HIGH);
      digitalWrite(ENABLE_LEFT, HIGH);
    }
    else if (speed < 0)
    { 
      left_dir = -1;      
      digitalWrite(LEFT_DIR, LOW);
      digitalWrite(ENABLE_LEFT, HIGH);
      speed = -speed;
    }
    else digitalWrite(ENABLE_LEFT, LOW);    
    left_wait_for_0_to_1 = (250000 / speed) - 1;
  }
  else if (motor == MOTOR_RIGHT)
  {
    if (speed > 0)
    {
      right_dir = 1;
      digitalWrite(RIGHT_DIR, HIGH);
      digitalWrite(ENABLE_RIGHT, HIGH);
    }
    else if (speed < 0)
    { 
      right_dir = -1;
      digitalWrite(RIGHT_DIR, LOW);
      digitalWrite(ENABLE_RIGHT, HIGH);
      speed = -speed;
    }
    else digitalWrite(ENABLE_RIGHT, LOW);
    right_wait_for_0_to_1 = (250000 / speed) - 1;
  }
}

void setup() 
{
  left_speed = 0;
  right_speed = 0;
  left_counter = 0;
  right_counter = 0;
  left_in_high_pulse = 0;
  right_in_high_pulse = 0;
  left_wait_for_0_to_1 = 65535;
  right_wait_for_0_to_1 = 65535;
  left_position = 0;
  right_position = 0;
  us1_dist = 255;
  us2_dist = 255;
  
  Serial.begin(115200);
  
  pinMode(LEFT_STEP, OUTPUT);
  pinMode(LEFT_DIR, OUTPUT);
  pinMode(RIGHT_STEP, OUTPUT);
  pinMode(RIGHT_DIR, OUTPUT);
  pinMode(ENABLE_LEFT, OUTPUT);
  pinMode(ENABLE_RIGHT, OUTPUT);
  pinMode(RED_SWITCH, INPUT);
  digitalWrite(RED_SWITCH, HIGH);  // pull-up on switch
  disable_steppers();
  digitalWrite(LEFT_STEP, HIGH);
  digitalWrite(LEFT_DIR, HIGH);
  digitalWrite(RIGHT_STEP, HIGH);
  digitalWrite(RIGHT_DIR, HIGH);
  pinMode(US1_TRIG, OUTPUT);
  pinMode(US1_ECHO, INPUT);
  pinMode(US2_TRIG, OUTPUT);
  pinMode(US2_ECHO, INPUT);
  
  // setup timer1 to call ISR every 4 usec
  TCCR1A = 0;
  TCCR1B = 0b0011001;
  TCCR1C = 0;
  TCNT1 = 0;
  ICR1 = 128;
  TIMSK1 = 0b10;

  // setup pin change interrupt on US echo pins
  PCMSK1 |= 0b1100;
  PCICR |= 0b10;
  Serial.println("Robot Mato.\nStartup delay 10s...");
  delay(10000);
}

void change_speed(int new_left_speed, int new_right_speed, int max_duration_in_ms)
{
  if (obstacle_blocking || motors_disabled) return;
  
  int start_left_speed = left_speed;
  int start_right_speed = right_speed;
  int num_change_steps;

  if (abs(new_left_speed - start_left_speed) > 
      abs(new_right_speed - start_right_speed))
    num_change_steps = abs(new_left_speed - start_left_speed);
  else num_change_steps= abs(new_right_speed - start_right_speed);

  if (num_change_steps > max_duration_in_ms) num_change_steps = max_duration_in_ms;

  float delta_left = (new_left_speed - start_left_speed) / (float)num_change_steps;
  float delta_right = (new_right_speed - start_right_speed) / (float)num_change_steps;

  float current_left = start_left_speed;
  float current_right = start_right_speed;

  Serial.print("CHANGE START ");
  Serial.print(new_left_speed);
  Serial.print(" ");
  Serial.println(new_right_speed);  
  print_position();
  
  for (int s = 0; s < num_change_steps; s++)
  {
    stepper_speed(MOTOR_LEFT, (int)(current_left + 0.5));
    stepper_speed(MOTOR_RIGHT, (int)(current_right + 0.5));
    current_left += delta_left;
    current_right += delta_right;
    delay(1);
  }

  stepper_speed(MOTOR_LEFT, new_left_speed);
  stepper_speed(MOTOR_RIGHT, new_right_speed);

  left_speed = new_left_speed;
  right_speed = new_right_speed;
  
  Serial.println("CHANGE STOP");
  print_position();
}

void mato_forward()
{
  enable_steppers();
  change_speed(left_speed + ONE_PRESS_DELTA, right_speed + ONE_PRESS_DELTA, INT_MAX);
}

void mato_left()
{
  enable_steppers();
  change_speed(left_speed - ONE_PRESS_DELTA, right_speed + ONE_PRESS_DELTA, INT_MAX);
}

void mato_back()
{
  enable_steppers();
  change_speed(left_speed - ONE_PRESS_DELTA, right_speed - ONE_PRESS_DELTA, INT_MAX);
}

void mato_right()
{
  enable_steppers();
  change_speed(left_speed + ONE_PRESS_DELTA, right_speed - ONE_PRESS_DELTA, INT_MAX);
}

void mato_stop()
{
  change_speed(0, 0, INT_MAX);
  disable_steppers();  
}

void mato_stop_fast()
{
  change_speed(0, 0, STOP_FAST_MAX_MS);
  disable_steppers();  
}

void print_position()
{
  unsigned long tm = millis();  
  Serial.print(tm);
  Serial.print(" ");
  Serial.print(left_position);
  Serial.print(" ");
  Serial.println(right_position);  
}

void handle_red_switch()
{
  static uint8_t red_switch_counter = 0;
  
  if (digitalRead(RED_SWITCH))
  {
    if (red_switch_counter < 255) red_switch_counter++;
    else if (!motors_disabled)
    {
      mato_stop_fast();
      Serial.println("emergency stop");
      motors_disabled = 1;
    }
  }
  else
  {
    if (red_switch_counter) 
      red_switch_counter--;
    else if (motors_disabled)
    {
      Serial.println("stop released");
      motors_disabled = 0;
    }
  }
}

void test_ultrasonic()
{
  both_us_send_pulse();
  delay(100);
  while (Serial.available()) Serial.read();
  
  while (!Serial.available())
  {
    both_us_send_pulse();
    Serial.print(us1_dist);
    Serial.print(" ");
    Serial.println(us2_dist);
    delay(100);
  }
  Serial.read();
}

void handle_serial_port_direct_control()
{
  if (Serial.available())
  {
    char c = Serial.read();
    if (c == 'w') mato_forward();
    else if (c == 'a') mato_left();
    else if (c == 's') mato_back();
    else if (c == 'd') mato_right();
    else if (c == 'x') mato_stop();
    else if (c == 'c') mato_stop_fast();
    else if (c == '+') ONE_PRESS_DELTA *= 2;
    else if (c == '-') ONE_PRESS_DELTA /= 2;
    else if (c == 'u') test_ultrasonic();
    else if (c == '*') Serial.println("***");
  }
}

void regularly_print_position()
{
  unsigned long tm = millis();
  
  if (tm >= next_position)
  {
    print_position();
    next_position = tm + PRINT_PERIOD;
  }
}

static uint32_t last_us_measurement = 0;
static uint8_t obstacle_counter = 0;
void measure_distance()
{
  unsigned long tm = millis();
  
  if (tm - last_us_measurement > US_MEASUREMENT_PERIOD)
  {
    both_us_send_pulse();
  }
  int u1 = us1_dist;
  int u2 = us2_dist;
  
  if ((u1 < CRITICAL_DISTANCE) || (u2 < CRITICAL_DISTANCE))
  {
    if (obstacle_counter < 255) 
    {
      obstacle_counter++;
    }
    else if (!obstacle_blocking)
    {
      mato_stop_fast();
      Serial.print("obstacle ");
      Serial.print(u1);
      Serial.print(" ");
      Serial.println(u2);
      obstacle_blocking = 1;
    }
  }
  else 
  {
    if (obstacle_counter) obstacle_counter--;
    else if (obstacle_blocking)
    {
      Serial.println("obstacle freed");
      obstacle_blocking = 0;
    }
  }
}

static uint16_t to_start_moving = 0;
void get_moving_again()
{
  if (obstacle_blocking == 0)
  {
    if (to_start_moving < 20000) 
    {
      to_start_moving++;
      if (to_start_moving == 20000)
        change_speed(1500, 1500, INT_MAX);
    }
  }
  else to_start_moving = 0;
}


void loop() 
{
  handle_red_switch();
  handle_serial_port_direct_control();
  regularly_print_position();
  measure_distance();
  //get_moving_again();
}
