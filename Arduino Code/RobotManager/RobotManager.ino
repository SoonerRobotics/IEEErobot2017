#include "I2Cdev.h"
#include "MPU6050_6Axis_MotionApps20.h"
#include "Wire.h"
#include "SPI.h"
#include "PIDController.h"

//Units to trigger obstacle detection (0-1023 mapped from 0 to 5 Volts)
#define OBJECT_THRESHOLD 512 

//Message Debug
int MSG_LED = 13;

double FORWARD_DIST = 20;
float LEFT_TURN  = -90;
float RIGHT_TURN = 90;
float FULL_TURN = 180;

byte xyz[3];

enum State {
	MAIN_STATE, IDLE_STATE, SEND
};

State stateMachine = IDLE_STATE;

//Debug flags
bool is_debug = false;
bool is_nav_debug = true;

//Navigation mode flags
bool isTurnInPlace = false;
bool turnComplete = false;
bool driveComplete = false;

bool stateJustChanged = false;
int driveState = 0;

float startYaw = 0;

//DIO Map Communication OUT
int NAV_READY = 35; //B is MSB, D is LSB

//DIO Map Communication IN
int E = 34, F = 36, G = 38, INSTRUCT = 40;
int PR = 30; //pin to indicate that the package has been recieved

//Message Output Interrupt Indicator
int I = 42;
int lastIValue = LOW;

//Object sensor
int sharpAnalogPin = 0;
float sharpValue;
bool isPathBlocked = false;

//Metal Detector Stuff
int metalDetectorPin = 28;

//The device itself
MPU6050 mpu;

//Variables required to have a gyro reset
bool firstRun = true;
bool progFirstRun = true;
float reference[3]; //[yaw, pitch, roll]

// MPU control/status vars
bool dmpReady = false;  // set true if DMP init was successful
uint8_t mpuIntStatus;   // holds actual interrupt status byte from MPU
uint8_t devStatus;      // return status after each device operation (0 = success, !0 = error)
uint16_t packetSize;    // expected DMP packet size (default is 42 bytes)
uint16_t fifoCount;     // count of all bytes currently in FIFO
uint8_t fifoBuffer[64]; // FIFO storage buffer

// orientation/motion vars
Quaternion q;           // [w, x, y, z]         quaternion container
VectorInt16 aa;         // [x, y, z]            accel sensor measurements
VectorInt16 aaReal;     // [x, y, z]            gravity-free accel sensor measurements
VectorInt16 aaWorld;    // [x, y, z]            world-frame accel sensor measurements
VectorFloat gravity;    // [x, y, z]            gravity vector
float euler[3];         // [psi, theta, phi]    Euler angle container
float ypr[3];           // [yaw, pitch, roll]   yaw/pitch/roll container and gravity vector

//Encoder Pins
int leftEncoderA = 18;
int leftEncoderB = 17;
int rightEncoderA = 3;
int rightEncoderB = 4;

int leftEncoderPos = 0;
int rightEncoderPos = 0;
float yaw = 0;

int n = LOW, m = LOW;
int leftEncoderALast = LOW;
int rightEncoderALast = LOW;

//Navigation variables
int left_motor_pin = 6, left_in_1 = 9, left_in_2 = 10;
int right_motor_pin = 44, right_in_1 = 48, right_in_2 = 46;


double distance = 0.0, gyro_error;
double kR = -0.00395, kL = 0.00418;//Encoder constants to convert to inches
double Y, X;

//Targets
float targetYaw = 0;
double distance_target = 0;

//Stores the PID constants for driving a distance and turning. [kP, kI, kD]
float turnPID[3] = {0.15, 0.0001, 0.0002};
float distPID[3] = {0.30, 0.0001, 0.0010}; 

PIDController turningPID(0, turnPID);
PIDController distancePID(0, distPID);


/*
	Reset Functions
*/
void resetEncoders()
{
	leftEncoderPos = 0;
	rightEncoderPos = 0;
}

void resetGyro()
{
  reference[0] = ypr[0];
  reference[1] = ypr[1];
  reference[2] = ypr[2];
}
/*
	End Reset Functions
*/

/*
	Interrupt Functions
*/

//We need interrupts to make sure we have the most accurate data
volatile bool mpuInterrupt = false;// indicates whether MPU interrupt pin has gone high
void dmpDataReady() 
{
    mpuInterrupt = true;
}

//If the signals are the same, the encoder is rotating forward, else backwards
void doLeftEncoder()
{
  if(digitalRead(leftEncoderA) == digitalRead(leftEncoderB))
  {
    ++leftEncoderPos;
  }
  else
  {
    --leftEncoderPos;
  }
}
void doRightEncoder()
{
  if(digitalRead(rightEncoderA) == digitalRead(rightEncoderB))
  {
    ++rightEncoderPos;
  }
  else
  {
    --rightEncoderPos;
  }
}

void getMessage()
{
    int x = digitalRead(E);
    int y = digitalRead(F);
    int z = digitalRead(G);
	
	driveState = (x<<2)|(y<<1)|z;
	
	Serial.print("DS: ");
	Serial.println(driveState);
			
	driveComplete = false;
	turnComplete = false;
	
	arcadeDrive(0, 0); //Stop
	delay(100); //Make sure we are really stopped
	
	//Reset
	resetGyro();
	resetEncoders();
	
	if(driveState == 0)
	{
		idle(50);
	}
	else if(driveState == 1)
	{
		forwardOne();
	}
	else if(driveState == 2)
	{
		leftTurn();
	}
	else if(driveState == 3)
	{
		rightTurn();
	}
	else if(driveState == 4)
	{
		fullTurn();
	}
	else if(driveState == 5)
	{
		cacheSequenceClosedLoop();
	}
	else if(driveState == 6)
	{
		//waitForInstructions();
		idle(1000);
	}
	else
	{
		idle(100);
	}
	
	digitalWrite(MSG_LED, LOW);
	
	digitalWrite(NAV_READY, LOW);
	stateMachine = MAIN_STATE;
}

void sendMessage()
{
	digitalWrite(NAV_READY, HIGH);
	
	stateMachine = IDLE_STATE;
}
/*
	End Interrupt Functions
*/


/*
	Driving functions
	Arcade Drive and filtering
*/
double filter(double p)
{
  if(abs(p) < 0.1)
  {
    return 0;
  }
  return p;
}

void arcadeDrive(float forward_power, float turn_power)
{
  float right, left;
  
  if(forward_power > 0)
  {
    if(turn_power > 0)
    {
      right = forward_power - turn_power;
      left = max(forward_power, turn_power);
    }
    else
    {
      right = max(forward_power, -turn_power);
      left = forward_power + turn_power;
    }
  }
  else
  {
    if(turn_power > 0)
    {
      right = (-1) * max(-forward_power, turn_power);
      left = forward_power + turn_power;
    }
    else
    {
      right = forward_power - turn_power;
      left = (-1) * max(-turn_power, -forward_power);
    }
  }
  
//Configure motors for directional driving
  if(left < 0)
  {
	digitalWrite(left_in_1, HIGH);
	digitalWrite(left_in_2, LOW); 
  }
  else if(left == 0)
  {
	digitalWrite(left_in_1, LOW);
	digitalWrite(left_in_2, LOW); 
  }
  else
  {
	digitalWrite(left_in_1, LOW);
	digitalWrite(left_in_2, HIGH); 
  }
  
  if(right < 0)
  {
	digitalWrite(right_in_1, HIGH);
	digitalWrite(right_in_2, LOW);
  }
  else if(right == 0)
  {
	digitalWrite(right_in_1, LOW);
	digitalWrite(right_in_2, LOW);
  }
  else
  {
	digitalWrite(right_in_1, LOW);
	digitalWrite(right_in_2, HIGH);
  }
  
  //Output to motors
  analogWrite(left_motor_pin, abs(left)  * 255);
  analogWrite(right_motor_pin, abs(right) * 255);
}
/*
	End Driving Functions
*/

/*
	Driving Routines
	These functions set targets once a state change takes place
*/
void forwardOne()
{
	isTurnInPlace = false;
	
	distance_target = FORWARD_DIST;
	targetYaw = 0;
}

void leftTurn()
{
	isTurnInPlace = true;
	
	distance_target = 0;
	targetYaw = LEFT_TURN;
}

void rightTurn()
{
	isTurnInPlace = true;
	
	distance_target = 0;
	targetYaw = RIGHT_TURN;
}

void fullTurn()
{
	isTurnInPlace = true;
	
	distance_target = 0;
	targetYaw = FULL_TURN;
}

void cacheSequenceClosedLoop()
{
	
}

void idle(int ms)
{
	distance_target = 0;
	targetYaw = 0;
	delay(ms);
	
	driveComplete = true;
	turnComplete = true;
}

void waitForInstructions()
{
	while(!stateJustChanged)
	{
		arcadeDrive(0, 0);
		distance_target = 0;
		targetYaw = 0;
	}
}
/*
	End Driving Routines
*/

/*
	Control Loops
*/
void mainControlLoop()
{

	if((!driveComplete || !turnComplete))
	{	

		//If the robot is within a half inch of distance target, stop
		if(abs(distance - distance_target) < 0.5)
		{
			X = 0;
			driveComplete = true;
		}
	
		//Get the output variables based on PID Control
		if(!isTurnInPlace)
		{
			X = distancePID.GetOutput(distance_target, distance); //Calculate the forward power of the motors
		}
		else
		{
			driveComplete = true;
			X = 0;
		}
		
		Y = turningPID.GetOutput(0, gyro_error); //Calculate the turning power of the motors
	
		//Don't output if the output won't move the robot (save power)
		X = filter(X);
		Y = filter(Y);
		
		if(is_nav_debug)
		{
			//Serial.print(X);
			//Serial.print(" ");
			//Serial.println(Y);
		}
	
		//if we are only off by 1.5 degrees, dont turn
		if(abs(gyro_error) < 1.5)
		{
			Y = 0;
			turnComplete = true;
		}
		else
		{
			turnComplete = false;
		}
		
		arcadeDrive(X,Y);
		
		if(is_nav_debug)
		{
			//Serial.print("X: ");
			//Serial.print(X);
			//Serial.print("\tY: ");
			//Serial.print(Y);
			//Serial.print("\tYAW: \t");
			//Serial.print(yaw);
			//Serial.print("\tYAW ERROR: \t");
			//Serial.println(gyro_error);
			//Serial.print("\tDistance: ");
			//Serial.println(distance);
		}
	}
	else
	{
		arcadeDrive(0, 0);
		stateMachine = SEND;
	}
}
/*
	End Control Loops
*/

void setup() 
{
  pinMode(leftEncoderA, INPUT);
  pinMode(leftEncoderB, INPUT);
  digitalWrite(leftEncoderA, HIGH);//pull up resistor
  digitalWrite(leftEncoderB, HIGH);//pull up resistor
  
  pinMode(rightEncoderA, INPUT);
  pinMode(rightEncoderB, INPUT);
  digitalWrite(rightEncoderA, HIGH);//pull up resistor
  digitalWrite(rightEncoderB, HIGH);//pull up resistor

  attachInterrupt(1, doRightEncoder, CHANGE); //pin 3 interrupt
  attachInterrupt(5, doLeftEncoder, CHANGE);

  //Motor Initialization
  pinMode(left_motor_pin, OUTPUT);
  pinMode(right_motor_pin, OUTPUT);
  pinMode(right_in_1, OUTPUT);
  pinMode(right_in_2, OUTPUT);
  pinMode(left_in_1, OUTPUT);
  pinMode(left_in_2, OUTPUT);
  
  //Message Out Pins
  pinMode(E, INPUT);
  pinMode(F, INPUT);
  pinMode(G, INPUT);
  pinMode(INSTRUCT, INPUT);
  pinMode(NAV_READY, OUTPUT); //Tell the AI we are ready
  
  //Set to default ready mode (Motion complete, waiting for instructions)
  digitalWrite(E, HIGH);
  digitalWrite(F, LOW);
  digitalWrite(G, LOW);
  digitalWrite(I, LOW);
  
  //Setup metal detector pin to get readings
  pinMode(metalDetectorPin, INPUT);
  
  //Set output range
  turningPID.SetOutputRange(0.4, -0.4);
  distancePID.SetOutputRange(0.6, -0.6);

  
  Wire.begin();
  if(is_debug || is_nav_debug)
  {
	Serial.begin(115200);
  }
  
  //Serial3.begin(115200);
  
  //Gyro initialization
  if(is_debug)
  {
    Serial.println("Initializing MPU6050...");
  }
  mpu.initialize();
  
  if(is_debug)
  {
    Serial.println(mpu.testConnection() ? F("MPU6050 connection successful") : F("MPU6050 connection failed"));
  }
  
  devStatus = mpu.dmpInitialize();
  
  if(devStatus == 0)
  {
    mpu.setDMPEnabled(true);
    
    //Attaches the interrupt used for detecting new data
    attachInterrupt(0, dmpDataReady, RISING);
    
    mpuIntStatus = mpu.getIntStatus();
    dmpReady = true;
    
    packetSize = mpu.dmpGetFIFOPacketSize();
  }
  else
  {
	if(is_debug)
    {
      Serial.println("Error connecting to MPU6050");
	}
  }
  
	digitalWrite(MSG_LED, HIGH);
	delay(500);
	digitalWrite(MSG_LED, LOW);
	delay(500);
	digitalWrite(MSG_LED, HIGH);
	delay(500);
	digitalWrite(MSG_LED, LOW);
}

void loop() 
{ 
  if(progFirstRun)
  {
    delay(15000);
    progFirstRun = false;
  }
  
  if(!dmpReady) //Die if there are errors
  {
	if(is_debug)
	{
		Serial.println("RIP: There were errors.");
	}
    return;
  }
  
  Serial.print("FC: \t");
  Serial.println(fifoCount);
  
  //while the gyro isn't giving any data, do other things
  while(!mpuInterrupt && packetSize >= fifoCount)
  {	
	//Get all sensor data
	distance = ((double)(kL*leftEncoderPos) + (double)(kR * rightEncoderPos))/2;
	
	//Calculate gyro error
	gyro_error = yaw - targetYaw;
	//gyro_error = fmod((gyro_error + 180), 360.0) - 180;
	if(gyro_error > 180)
	{
		gyro_error = -(360 - gyro_error); 
	}
	
	if(is_debug)
	{
		//Now that the screen is cleared, we can print the latest data
		Serial.print("Left: \t");
		Serial.print(leftEncoderPos);
		Serial.print("\tRight: \t");
		Serial.print(rightEncoderPos);
		Serial.print("\tYAW: \t");
		Serial.println(yaw, 4);
	}
	else
	{
		//Serial.println("loop loop");
	
		if(stateMachine == MAIN_STATE)
		{
			mainControlLoop();
		}
		else if(stateMachine == SEND)
		{
			sendMessage();
			
			digitalWrite(MSG_LED, LOW);
		}
		else if(stateMachine == IDLE_STATE)
		{
			digitalWrite(NAV_READY, HIGH);
			if(digitalRead(INSTRUCT))
			{
				getMessage();
			}
			arcadeDrive(0, 0);
		}
	}
  }
  
  // reset interrupt flag and get INT_STATUS byte
  mpuInterrupt = false;
  mpuIntStatus = mpu.getIntStatus();
  
  fifoCount = mpu.getFIFOCount();
  
  //check for overflow (should not overflow often)
  if ((mpuIntStatus & 0x10) || fifoCount == 1024) 
  {
        // reset so we can continue cleanly
        mpu.resetFIFO();
		if(is_debug)
		{
			Serial.println(F("FIFO overflow!"));
		}
  } 
  else if (mpuIntStatus & 0x02) // otherwise, check for DMP data ready interrupt (this should happen frequently)
  {
    // wait for correct available data length, should be a VERY short wait
    while (fifoCount < packetSize)
    {
      fifoCount = mpu.getFIFOCount();
    }

    // read a packet from FIFO
    mpu.getFIFOBytes(fifoBuffer, packetSize);
        
    // track FIFO count here in case there is > 1 packet available
    // (this lets us immediately read more without waiting for an interrupt)
    fifoCount -= packetSize;

    //Get Yaw, Pitch and Roll
    mpu.dmpGetQuaternion(&q, fifoBuffer);
    mpu.dmpGetGravity(&gravity, &q);
    mpu.dmpGetYawPitchRoll(ypr, &q, &gravity);
  
    yaw = ((ypr[0] - reference[0]) * 180 / M_PI);
    if(yaw < 0)
    {
      yaw = 360 + yaw;
    }
    
    if(firstRun)
    {
      resetGyro();

      turningPID.reinitialize(yaw);
      
      firstRun = false;
    }
  }  
  
	if(is_nav_debug)
	{
		Serial.print(distance);
		Serial.print(" vs. ");
		Serial.print(distance_target);
		//Serial.print(digitalRead(PR));
		Serial.print("\t");
		Serial.println(stateMachine);
	}
	
}