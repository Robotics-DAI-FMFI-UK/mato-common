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

// 17 => PC3 - PCINT11, 16 => PC2 - PCINT10, 8 => PB0 - PCINT0, 7 => PD7, PCINT23

uint8_t us_trig[4] = {14, 15, 9, 2};
uint8_t us_echo[4] = {17, 16, 8, 7};

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

volatile uint16_t us0_dist, us1_dist, us2_dist, us3_dist;

volatile uint8_t obstacle_blocking = 0;
volatile uint8_t motors_disabled = 0;

volatile uint8_t us_new_value = 0;

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
static uint32_t us0_start_pulse, us1_start_pulse;

static uint8_t pci0_pinb;
static uint8_t pci0_xored_pinb;
static uint8_t pci0_old_pinb = 0;
static uint32_t us2_start_pulse;

static uint8_t pci2_pind;
static uint8_t pci2_xored_pind;
static uint8_t pci2_old_pind = 0;
static uint32_t us3_start_pulse;

ISR(PCINT0_vect)
{
   // PB0 = arduino pin 8   - US2_ECHO
  pci0_pinb = PINB;
  pci0_xored_pinb = pci0_pinb ^ pci0_old_pinb;
  if (pci0_xored_pinb & 0b1)
  {
    if (pci0_pinb & 0b1)  // raising edge on ECHO of US2 
      us2_start_pulse = micros();
    else // falling edge on ECHO of US2
    {
      if (micros() > us2_start_pulse) 
      {
          us2_dist = (micros() - us2_start_pulse) / 58;
          us_new_value = 1;
      }
    }
  }
  pci0_old_pinb = pci0_pinb;
}

ISR(PCINT1_vect)
{
   // PC2 = arduino pin 16  - US1_ECHO
   // PC3 = arduino pin 17  - US0_ECHO
  pci1_pinc = PINC;
  pci1_xored_pinc = pci1_pinc ^ pci1_old_pinc;
  if (pci1_xored_pinc & 0b100)
  {
    if (pci1_pinc & 0b100)  // raising edge on ECHO of US0
      us1_start_pulse = micros();
    else // falling edge on ECHO of US1
    {
      if (micros() > us1_start_pulse) 
      {
          us1_dist = (micros() - us1_start_pulse) / 58;
          us_new_value = 1;          
      }
    }
  }
  if (pci1_xored_pinc & 0b1000)
  {
    if (pci1_pinc & 0b1000)  // raising edge on ECHO of US1 
      us0_start_pulse = micros();
    else // falling edge on ECHO of US0
    {
      if (micros() > us0_start_pulse)
      {
        us0_dist = (micros() - us0_start_pulse) / 58;
        us_new_value = 1;
      }
    }    
  }
  pci1_old_pinc = pci1_pinc;
}

ISR(PCINT2_vect)
{
   // PD7 = arduino pin 7   - US3_ECHO
  pci2_pind = PIND;
  pci2_xored_pind = pci2_pind ^ pci2_old_pind;
  if (pci2_xored_pind & 0b10000000)
  {
    if (pci2_pind & 0b10000000)  // raising edge on ECHO of US3 
      us3_start_pulse = micros();
    else // falling edge on ECHO of US3
    {
      if (micros() > us3_start_pulse) 
      {
          us3_dist = (micros() - us3_start_pulse) / 58;
          us_new_value = 1;
      }          
    }
  }
  pci2_old_pind = pci2_pind;
}

void all_us_send_pulse()
{
  digitalWrite(us_trig[0], HIGH);
  digitalWrite(us_trig[1], HIGH);
  digitalWrite(us_trig[2], HIGH);
  digitalWrite(us_trig[3], HIGH);  
  delayMicroseconds(10);
  digitalWrite(us_trig[0], LOW);
  digitalWrite(us_trig[1], LOW);
  digitalWrite(us_trig[2], LOW);
  digitalWrite(us_trig[3], LOW);
}

void us_send_pulse(uint8_t i)
{
  digitalWrite(us_trig[i], HIGH);
  delayMicroseconds(10);
  digitalWrite(us_trig[i], LOW);
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
  us0_dist = 255;
  us1_dist = 255;
  us2_dist = 255;
  us3_dist = 255;
  
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
  for (int i = 0; i < 4; i++)
  {
      pinMode(us_trig[i], OUTPUT);
      pinMode(us_echo[i], INPUT);    
  }

  Serial.begin(115200);
  
  // setup timer1 to call ISR every 4 usec
  TCCR1A = 0;
  TCCR1B = 0b0011001;
  TCCR1C = 0;
  TCNT1 = 0;
  ICR1 = 128;
  TIMSK1 = 0b10;

  // setup pin change interrupt on US echo pins
  PCMSK0 |= 1;
  PCMSK1 |= 0b1100;
  PCMSK2 |= 0b10000000;
  PCICR |= 0b111;
  Serial.println("Robot Mato.\nStartup delay 10s...");
  // delay(10000);
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
  uint32_t tm = millis();  
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
  all_us_send_pulse();
  delay(100);
  while (Serial.available()) Serial.read();
  
  while (!Serial.available())
  {
    all_us_send_pulse();
    Serial.print(us0_dist);
    Serial.print(" ");
    Serial.print(us1_dist);
    Serial.print(" ");
    Serial.print(us2_dist);
    Serial.print(" ");
    Serial.println(us3_dist);    
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
    all_us_send_pulse();
    last_us_measurement = tm;
    return;
  }
  if (!us_new_value) return;
  us_new_value = 0;
  
  int u0 = us0_dist;
  int u1 = us1_dist;
  int u2 = us2_dist;
  int u3 = us3_dist;

  if (u2 < CRITICAL_DISTANCE)
//  if ((u0 < CRITICAL_DISTANCE) || (u1 < CRITICAL_DISTANCE) || (u2 < CRITICAL_DISTANCE) || (u3 < CRITICAL_DISTANCE))

 // if ((u0 < CRITICAL_DISTANCE) || (u1 < CRITICAL_DISTANCE) || (u2 < CRITICAL_DISTANCE) || (u3 < CRITICAL_DISTANCE))
  {
    if (obstacle_counter < 3) 
    {
      Serial.print("oo ");
      Serial.println(obstacle_counter);
      obstacle_counter++;
    }
    else if (!obstacle_blocking)
    {
      mato_stop_fast();
      Serial.print("obstacle ");
      Serial.print(u0);
      Serial.print(" ");
      Serial.print(u1);
      Serial.print(" ");
      Serial.print(u2);
      Serial.print(" ");
      Serial.println(u3);
      obstacle_blocking = 1;
    }
  }
  else 
  {
    if (obstacle_counter) 
    {
      obstacle_counter--;
      Serial.print("nooooo ");
      Serial.println(obstacle_counter);
    }
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
