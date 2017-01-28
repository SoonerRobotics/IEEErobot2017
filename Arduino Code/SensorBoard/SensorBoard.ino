#include "I2Cdev.h"
#include "MPU6050_6Axis_MotionApps20.h"
#include "Wire.h"
#include "SPI.h"

//Debug flag
bool is_debug = false;

//SPI
int SS = 53;

//The device itself
MPU6050 mpu;

//Variables required to have a gyro reset
bool firstRun = true;
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

float leftEncoderPos = 0;
float rightEncoderPos = 0;
float yaw = 0;

int n = LOW, m = LOW;
int leftEncoderALast = LOW;
int rightEncoderALast = LOW;

//We need interrupts to make sure we have the most accurate data
volatile bool mpuInterrupt = false;// indicates whether MPU interrupt pin has gone high
void dmpDataReady() {
    mpuInterrupt = true;
}

void resetGyro()
{
  reference[0] = ypr[0];
  reference[1] = ypr[1];
  reference[2] = ypr[2];
}

//If the signals are the same, the encoder is rotating forward, else backwards
void doLeftEncoder()
{
  if(digitalRead(leftEncoderA) == digitalRead(leftEncoderB))
  {
    leftEncoderPos += 1.0;
  }
  else
  {
    leftEncoderPos -= 1.0;
  }
}
void doRightEncoder()
{
  if(digitalRead(rightEncoderA) == digitalRead(rightEncoderB))
  {
    rightEncoderPos += 1.0;
  }
  else
  {
    rightEncoderPos -= 1.0;
  }
}

void sendSensorData(float data)
{
	unsigned char *chptr;
	chptr = (unsigned char *) &data;
	SPI.transfer(*chptr++);
	SPI.transfer(*chptr++);
	SPI.transfer(*chptr++);
	SPI.transfer(*chptr);
}

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
  
  //SPI initialization
  //Turn on SPI in slave mode
  SPCR |= bit (SPE);

  // have to send on master in, *slave out*
  pinMode(MISO, OUTPUT);
  
  SPI.attachInterrupt();
  
  
  Wire.begin();
  if(is_debug)
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
}

void loop() 
{ 
  if(!dmpReady) //Die if there are errors
  {
	if(is_debug)
	{
      Serial.println("RIP: There were errors.");
	}
    return;
  }
  
  //while the gyro isn't giving any data, do other things
  while(!mpuInterrupt && packetSize >= fifoCount)
  {
	if(is_debug)
	{
      //Serial.write(27);       // ESC command
      //Serial.print("[2J");    // clear screen command
      //Serial.write(27);
      //Serial.print("[H");     //cursor to home command
      
      //Now that the screen is cleared, we can print the latest data
      Serial.print("Left: ");
      Serial.println(leftEncoderPos);
      Serial.print("Right: ");
      Serial.println(rightEncoderPos);
      Serial.print("YAW: \t");
      Serial.println(, 4);
	}
	else
	{
		/*
		Serial3.println('L');
		Serial3.println(leftEncoderPos); //Left Encoder
		Serial3.println('R');
		Serial3.println(rightEncoderPos); //Right Encoder
		Serial3.println('Y');
		Serial3.println((ypr[0] - reference[0]) * 180 / M_PI); //YAW
		Serial3.println("N");*/
		
		//Get the data that the master wants
		int id = SPI.transfer(0);
		
		//Send data over SPI
		if(id == 1)
		{
			sendSensorData(leftEncoderPos);
		}
		else if(id == 2)
		{
			sendSensorData(rightEncoderPos);
		}
		else if(id == 3)
		{
			sendSensorData(yaw);
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
	
	yaw = (ypr[0] - reference[0]) * 180 / M_PI;
    
    if(firstRun)
    {
      resetGyro();
      firstRun = false;
    }
  }  
}
