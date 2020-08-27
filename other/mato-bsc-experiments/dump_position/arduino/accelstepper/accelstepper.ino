// plink -sercfg 9600,8,n,1,X -serial /dev/ttyUSB0

#include <AccelStepper.h>

#define ENABLE_LEFT   10
#define ENABLE_RIGHT  11

#define LEFT_STEP  3
#define LEFT_DIR   4

#define RIGHT_STEP 5 
#define RIGHT_DIR  6

#define PRINT_PERIOD 200

#define MATO_SPEED 1000

AccelStepper left = AccelStepper(AccelStepper::DRIVER, LEFT_STEP, LEFT_DIR);
AccelStepper right = AccelStepper(AccelStepper::DRIVER, RIGHT_STEP, RIGHT_DIR);

unsigned long next_position;

int mato_speed;

void enable_steppers()
{
  digitalWrite(ENABLE_LEFT, HIGH);
  digitalWrite(ENABLE_RIGHT, HIGH);
}

void disable_steppers()
{
  digitalWrite(ENABLE_LEFT, LOW);
  digitalWrite(ENABLE_RIGHT, LOW);  
}

void setup() 
{
  mato_speed = MATO_SPEED;
  Serial.begin(9600);
  pinMode(ENABLE_LEFT, OUTPUT);
  pinMode(ENABLE_RIGHT, OUTPUT);
  disable_steppers();
  delay(5000);
  enable_steppers();
  delay(500);
  left.setMaxSpeed(5000);
  right.setMaxSpeed(5000);
  left.setAcceleration(20);
  right.setAcceleration(20);
  next_position = millis();
  //speed = 500;
}

void stepper_delay(unsigned long ms)
{
  unsigned long tm = millis() + ms;
  while (millis() < tm)
  {
    left.runSpeed();
    right.runSpeed();
  }  
}

void change_speed(int new_left_speed, int new_right_speed)
{
  int start_left_speed = left.speed();
  int start_right_speed = right.speed();
  int num_change_steps;

  if (abs(new_left_speed - start_left_speed) > 
      abs(new_right_speed - start_right_speed))
    num_change_steps = abs(new_left_speed - start_left_speed);
  else num_change_steps= abs(new_right_speed - start_right_speed);

  float delta_left = (new_left_speed - start_left_speed) / num_change_steps;
  float delta_right = (new_right_speed - start_right_speed) / num_change_steps;

  float current_left = start_left_speed;
  float current_right = start_right_speed;
  
  for (int s = 0; s < num_change_steps; s++)
  {
    left.setSpeed((int)(current_left + 0.5));
    right.setSpeed((int)(current_right + 0.5));
    current_left += delta_left;
    current_right += delta_right;
    stepper_delay(1);
  }

  left.setSpeed(new_left_speed);
  right.setSpeed(new_right_speed);
}

void mato_forward()
{
  enable_steppers();
  change_speed(mato_speed, mato_speed);
}

void mato_left()
{
  enable_steppers();
  change_speed(-mato_speed, mato_speed);
}

void mato_back()
{
  enable_steppers();
  change_speed(-mato_speed, -mato_speed);
}

void mato_right()
{
  enable_steppers();
  change_speed(mato_speed, -mato_speed);
}


void mato_stop()
{
  change_speed(0, 0);
  disable_steppers();  
}

void modify_speed(int delta)
{
  mato_speed += delta;
  Serial.print("new speed: ");
  Serial.println(mato_speed);  
}

void loop() {
  left.runSpeed();
  right.runSpeed();

  if (Serial.available())
  {
    char c = Serial.read();
    if (c == 'w') mato_forward();
    if (c == 'a') mato_left();
    if (c == 's') mato_back();
    if (c == 'd') mato_right();
    if (c == '+') modify_speed(50);
    if (c == '-') modify_speed(-50);
    if (c == 'x') mato_stop();
  }
  
  unsigned long tm = millis();
  
  if (tm >= next_position)
  {
    Serial.print(tm);
    Serial.print(" ");
    Serial.print(left.currentPosition());
    Serial.print(" ");
    Serial.println(right.currentPosition());
    next_position = tm + PRINT_PERIOD;
  }
  
}
