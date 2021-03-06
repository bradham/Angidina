#include <PID.h>

#include <AP_Common.h>
#include <AP_Math.h>
#include <AP_Param.h>
#include <AP_Progmem.h>
#include <AP_ADC.h>
#include <AP_InertialSensor.h>
#include <AP_HAL.h>
#include <AP_HAL_AVR.h>

#define MOTOR_FL 2 // Front left
#define MOTOR_FR 0 // Front right
#define MOTOR_BL 1 // back left
#define MOTOR_BR 3 // back right

#define PID_PITCH_RATE 0
#define PID_ROLL_RATE 1
#define PID_PITCH_STAB 2
#define PID_ROLL_STAB 3
#define PID_YAW_RATE 4
#define PID_YAW_STAB 5

// Wrap around for Yaw
#define wrap_180(x) (x < -180 ? x + 360 : (x > 180 ? x - 360: x))

PID pids[6];

float yaw_target = 0;

int loopCount = 0;
int printMod = 100;

const AP_HAL::HAL& hal = AP_HAL_AVR_APM2; // Hardware abstraction layer
AP_InertialSensor_MPU6000 ins;

long map(long x, long in_min, long in_max, long out_min, long out_max)
{
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

void setup()
{
  hal.rcout->set_freq(0xF, 490);
  hal.rcout->enable_mask(0xFF);
 
  // Determine Orientation Below
  // Disable barometer to stop it corrupting bus
  hal.gpio->pinMode(40, GPIO_OUTPUT);
  hal.gpio->write(40, 1);
  
  // Initialise MPU6050 sensor
  ins.init(AP_InertialSensor::COLD_START, AP_InertialSensor::RATE_100HZ, NULL);
  
  // Initialise MPU6050's internal sensor fusion (aka DigitalMotionProcessing)
  hal.scheduler->suspend_timer_procs(); // stop bus collisions
  ins.dmp_init();
  ins.push_gyro_offsets_to_dmp();
  hal.scheduler->resume_timer_procs();
  
  pids[PID_PITCH_RATE].kP(0.7);
  // pids[PID_PITCH_RATE].kI(1);
  pids[PID_PITCH_RATE].imax(50);
  pids[PID_ROLL_RATE].kP(0.7);
  // pids[PID_ROLL_RATE].kI(1);
  pids[PID_ROLL_RATE].imax(50);
  pids[PID_YAW_RATE].kP(2.5);
  // pids[PID_YAW_RATE].kI(1);
  pids[PID_YAW_RATE].imax(50);
  pids[PID_PITCH_STAB].kP(4.5);
  pids[PID_ROLL_STAB].kP(4.5);
  pids[PID_YAW_STAB].kP(10);
}

void loop()
{
  while (ins.num_samples_available() == 0);
  
  ins.update();
  
  float roll, pitch, yaw;
 
  ins.quaternion.to_euler(&roll, &pitch, &yaw);
  
  roll = ToDeg(roll);
  pitch = ToDeg(pitch);
  yaw = ToDeg(yaw);
  
  if (loopCount % printMod == 0) {
    hal.console->printf_P(PSTR("P:%4.1f R:%4.1f Y:%4.1f\n"), pitch, roll, yaw);
  }
  
  uint16_t channels[8]; // array for raw channel values
  
  // Read RC channels and store in channels array
  hal.rcin->read(channels, 8);
  
  // Copy from channels array to something human readable - array entry 0 =
  // Map variables for reading radio buttons
  long rcthr, rcyaw, rcpit, rcroll;
  
  rcthr = channels[2];
  rcyaw = map(channels[0], 1037, 1863, -150, 150);
  rcpit = map(channels[1], 1059, 1866, -45, 45);
  rcroll = map(channels[3], 1054, 1889, -45, 45);
  
  if (loopCount % printMod == 0) {
    hal.console->printf_P(PSTR("individual read THR %d YAW %d PIT %d ROLL %d\r\n"), rcthr, rcyaw, rcpit, rcroll);
  }
  
  hal.rcout->write(MOTOR_FR, rcthr);
  hal.rcout->write(MOTOR_FL, rcthr);
  hal.rcout->write(MOTOR_BL, rcthr);
  hal.rcout->write(MOTOR_BR, rcthr);
  
  Vector3f gyro = ins.get_gyro();
  
  float gyroPitch = ToDeg(gyro.y);
  float gyroRoll = ToDeg(gyro.x);
  float gyroYaw = ToDeg(gyro.z);
 
  if (rcthr > 1088 && rcthr < 2000) { // *** MINIMUM THROTTLE TO DO CORRECTIONS MAKE THIS
    //20pts ABOVE YOUR MIN THR STICK ***/
    
    // Stab PIDS
    float pitch_stab_output = constrain(pids[PID_PITCH_STAB].get_pid((float)rcpit - pitch, 1), -250, 250);
    float roll_stab_output = constrain(pids[PID_ROLL_STAB].get_pid((float)rcroll - roll, 1), -250, 250);
    float yaw_stab_output = constrain(pids[PID_YAW_STAB].get_pid(wrap_180(yaw_target - yaw), 1), -360, 360);
    
    if (abs(rcyaw) > 5) { // if pilot commanding yaw
      yaw_stab_output = rcyaw; // feed to rate controller (overwriting stabcontroller output)
      yaw_target = yaw; // update yaw target
    }
    
    //Rate PIDS
    long pitch_output = (long)constrain(pids[PID_PITCH_RATE].get_pid(pitch_stab_output - gyroPitch, 1), -500, 500);
    long roll_output = (long)constrain(pids[PID_ROLL_RATE].get_pid(roll_stab_output - gyroRoll, 1), -500, 500);
    long yaw_output = (long)constrain(pids[PID_YAW_RATE].get_pid(yaw_stab_output - gyroYaw, 1), -500, 500);
    
    hal.rcout->write(MOTOR_FL, rcthr - roll_output - pitch_output - yaw_output);
    hal.rcout->write(MOTOR_BL, 1.1 * (rcthr - roll_output + pitch_output + yaw_output));
    hal.rcout->write(MOTOR_FR, rcthr + roll_output - pitch_output + yaw_output);
    hal.rcout->write(MOTOR_BR, rcthr + roll_output + pitch_output - yaw_output);
  }
  else { // MOTORS OFF
    hal.rcout->write(MOTOR_FL, 1000);
    hal.rcout->write(MOTOR_BL, 1000);
    hal.rcout->write(MOTOR_FR, 1000);
    hal.rcout->write(MOTOR_BR, 1000);
    
    //Yaw Target direction of quadis on the ground
    yaw_target = yaw;
    
    for (int i = 0; i < 6; i++) {
      // reset PID integrals whilst on the ground
      pids[i].reset_I();
    }
  }
  
  loopCount = loopCount + 1;
}

AP_HAL_MAIN();
