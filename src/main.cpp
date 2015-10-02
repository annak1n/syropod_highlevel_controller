/* (c) Copyright CSIRO 2013. Author: Thomas Lowe
   This software is provided under the terms of Schedule 1 of the license agreement between CSIRO, 3DLM and GeoSLAM.
*/
#include "../include/simple_hexapod_controller/standardIncludes.h"
#include "../include/simple_hexapod_controller/model.h"
#include "../include/simple_hexapod_controller/walkController.h"
#include "../include/simple_hexapod_controller/debugOutput.h"
#include "../include/simple_hexapod_controller/motorInterface.h"
#include "../include/simple_hexapod_controller/dynamixelMotorInterface.h"
#include "../include/simple_hexapod_controller/dynamixelProMotorInterface.h"
#include "../include/simple_hexapod_controller/imuCompensation.h"
#include <boost/concept_check.hpp>
#include <iostream>
#include "std_msgs/Bool.h"
#include <sys/select.h>
#include "geometry_msgs/Twist.h"
#include "geometry_msgs/Vector3.h"
#include "sensor_msgs/JointState.h"
#include <dynamic_reconfigure/server.h>
//#include <simple_hexapod_controller/simpleController.h>

//Globals for joypad callback
static Vector2d localVelocity(0,0);
static double turnRate = 0;
static double pitchJoy = 0;
static double rollJoy = 0;
static double yawJoy = 0;
static double xJoy = 0;
static double yJoy = 0;
static double zJoy = 0;

//Globals for joint states callback
sensor_msgs::JointState jointStates;
double jointPositions[18];
bool jointPosFlag = false;
bool startFlag = false;

void joypadVelocityCallback(const geometry_msgs::Twist &twist);
void joypadPoseCallback(const geometry_msgs::Twist &twist);
void startCallback(const std_msgs::Bool &startBool);

double getTiltCompensation(WalkController walker);
double getPitchCompensation(WalkController walker);

void jointStatesCallback(const sensor_msgs::JointState &joint_States);
void getParameters(ros::NodeHandle n, Parameters *params);

/***********************************************************************************************************************
 * Main
***********************************************************************************************************************/
int main(int argc, char* argv[])
{
  ros::init(argc, argv, "Hexapod");
  ros::NodeHandle n;
  ros::NodeHandle n_priv("~");
  
  ros::Subscriber velocitySubscriber = n.subscribe("/desired_velocity", 1, joypadVelocityCallback);
  ros::Subscriber poseSubscriber = n.subscribe("/desired_pose", 1, joypadPoseCallback);
  ros::Subscriber imuSubscriber = n.subscribe("/ig/imu/data", 1, imuCallback);
  ros::Publisher controlPub = n.advertise<geometry_msgs::Vector3>("controlsignal", 1000);
  geometry_msgs::Vector3 controlMeanAcc;
  ros::Subscriber startSubscriber = n.subscribe("/start_state", 1, startCallback);
  ros::Subscriber jointStatesSubscriber;
  
  //DEBUGGING
  ros::Publisher tipPosPub[3][2];
  tipPosPub[0][0] = n.advertise<geometry_msgs::Vector3>("tip_positions_00", 1);
  tipPosPub[0][1] = n.advertise<geometry_msgs::Vector3>("tip_positions_01", 1);
  tipPosPub[1][0] = n.advertise<geometry_msgs::Vector3>("tip_positions_10", 1);
  tipPosPub[1][1] = n.advertise<geometry_msgs::Vector3>("tip_positions_11", 1);
  tipPosPub[2][0] = n.advertise<geometry_msgs::Vector3>("tip_positions_20", 1);
  tipPosPub[2][1] = n.advertise<geometry_msgs::Vector3>("tip_positions_21", 1);
  //DEBUGGING
  
  ros::Rate r(roundToInt(1.0/timeDelta));         //frequency of the loop. 
  
  cout << "Press 'Start' to run controller" << endl;
  while(!startFlag)
  {
    ros::spinOnce();
    r.sleep();
  }
  
  
  //Get parameters from rosparam via loaded config file
  Parameters params;
  getParameters(n, &params);
  
  //MOVE_TO_START
  if (params.moveToStart)
  {
    jointStatesSubscriber = n.subscribe("/hexapod/joint_states", 1, jointStatesCallback);
    
    if(!jointStatesSubscriber)
    {
      cout << "Failed to subscribe to joint_states topic - check to see if topic is being published." << endl;
      params.moveToStart = false;
    }
    else
    {
      for (int i=0; i<18; i++)
        jointPositions[i] = 1e10;
    
      int spin = 20; //Max ros spin cycles to find joint positions
      while(spin--)
      {
        ros::spinOnce();
        r.sleep();
      }
    }
  }
  
  //Create hexapod model    
  Model hexapod(params.hexapodType,
                params.stanceLegYaws, 
                params.yawLimits, 
                params.kneeLimits, 
                params.hipLimits);
  
  if (params.hexapodType == "large_hexapod")
    hexapod.jointMaxAngularSpeeds = params.jointMaxAngularSpeeds;
  
  //MOVE_TO_START
  if (params.moveToStart)
  {
    if (jointPosFlag)
    {
      // set initial leg angles
      for (int leg = 0; leg<3; leg++)
      {
        for (int side = 0; side<2; side++)
        {
          double dir = side==0 ? -1 : 1;
          int index = leg*6+(side == 0 ? 0 : 3);
          hexapod.setLegStartAngles(side, leg, dir*Vector3d(jointPositions[index+0]+dir*params.stanceLegYaws[leg],
                                                            -jointPositions[index+1], jointPositions[index+2]));
          double yaw = hexapod.legs[leg][side].yaw;
          double liftAngle = hexapod.legs[leg][side].liftAngle;
          double kneeAngle = hexapod.legs[leg][side].kneeAngle;
          cout<<"leg "<<leg<<", side: "<<side<<" values: "<<yaw<<", "<<liftAngle<<", "<<kneeAngle<<endl;
        }
      }
    }
    else
    {
      cout << "Failed to acquire all joint position values" << endl;
      params.moveToStart = false;
    }
  }

  // Create walk controller object
  WalkController walker(&hexapod, params);
    
  DebugOutput debug;
  setCompensationDebug(debug);

  //Setup motor interaface
  MotorInterface *interface;
  if (params.dynamixelInterface)
    interface = new DynamixelMotorInterface();
  
  else
    interface = new DynamixelProMotorInterface();
  interface->setupSpeed(params.interfaceSetupSpeed);   
  
  if (params.moveToStart)
    cout << "Attempting to move to starting stance . . ." << endl;
  
  //Position update loop
  bool firstFrame = true;
  bool started = false;
  while (ros::ok())
  {
    Pose adjust = Pose::identity();
    if (params.imuCompensation)
    {
      //Auto Compensation using IMU feedback
      Vector2d acc = walker.localCentreAcceleration;
      //adjust = compensation(Vector3d(acc[0], acc[1], 0), walker.angularVelocity);
	  Vector3d deltaAngle;
      Vector3d deltaPos = compensation(Vector3d(acc[0], acc[1], 0), walker.angularVelocity, &deltaAngle);
      //Vector3d deltaPos = Vector3d(0,0,0);
      controlMeanAcc.x=deltaPos(0);
      controlMeanAcc.y=deltaPos(1);
      controlMeanAcc.z=deltaPos(2);
      controlPub.publish(controlMeanAcc); 
    }
    else if (params.autoCompensation)
    {
      //Automatic (non-feedback) compensation
      double pitch = getPitchCompensation(walker);
      double roll = getTiltCompensation(walker);  
      if (params.gaitType == "wave_gait")
        adjust = Pose(Vector3d(xJoy,yJoy,zJoy), Quat(1,pitch,roll,yawJoy));
      else if (params.gaitType == "tripod_gait")
        ;//adjust = Pose(Vector3d(0,0,0), Quat(1,0,roll,0)); //NOT WORKING YET FOR TRIPOD
    }    
    else if (params.manualCompensation)
    {    
      //Manual body compensation 
      adjust = Pose(Vector3d(xJoy,yJoy,zJoy), Quat(1,pitchJoy,rollJoy,yawJoy));
    }   

	 
    
    //Manual velocity control
    //localVelocity = Vector2d(1e-10, 1e-10);
    
    //Update walker or move to starting stance
    if (!started && params.moveToStart)
      started = walker.moveToStart();
    else
    walker.update(localVelocity, turnRate*turnRate*turnRate, &adjust, &deltaPos, &deltaAngle); // the cube just lets the thumbstick give small turns easier
    
    debug.drawRobot(hexapod.legs[0][0].rootOffset, hexapod.getJointPositions(walker.pose * adjust), Vector4d(1,1,1,1));
    debug.drawPoints(walker.targets, Vector4d(1,0,0,1));
    
    //DEBUGGING
    geometry_msgs::Vector3 msg;
    for (int l = 0; l<3; l++)
    {
      for (int s = 0; s<2; s++)
      {
        msg.x = walker.tipPositions[l][s][0];
        msg.y = walker.tipPositions[l][s][1];
        msg.z = walker.tipPositions[l][s][2];
        tipPosPub[l][s].publish(msg);
      }
    }
    //DEBUGGING
    
    if (true)
    {
      for (int s = 0; s<2; s++)
      {
        double dir = s==0 ? -1 : 1;
        for (int l = 0; l<3; l++)
        {
          double angle;  
          double yaw = dir*(walker.model->legs[l][s].yaw - params.stanceLegYaws[l]);
          double lift = -dir*walker.model->legs[l][s].liftAngle;
          double knee = dir*walker.model->legs[l][s].kneeAngle;
          
          if (false) // !firstFrame)
          {
            double yawVel = (yaw - walker.model->legs[l][s].debugOldYaw)/timeDelta;
            double liftVel = (lift - walker.model->legs[l][s].debugOldLiftAngle)/timeDelta;
            double kneeVel = (knee - walker.model->legs[l][s].debugOldKneeAngle)/timeDelta;
            
            if (abs(yawVel) > hexapod.jointMaxAngularSpeeds[0])
              yaw = walker.model->legs[l][s].debugOldYaw + sign(yawVel)*hexapod.jointMaxAngularSpeeds[0]*timeDelta;
            if (abs(liftVel) > hexapod.jointMaxAngularSpeeds[1])
              lift = walker.model->legs[l][s].debugOldLiftAngle + sign(liftVel)*hexapod.jointMaxAngularSpeeds[1]*timeDelta;
            if (abs(yawVel) > hexapod.jointMaxAngularSpeeds[2])
              knee = walker.model->legs[l][s].debugOldKneeAngle + sign(kneeVel)*hexapod.jointMaxAngularSpeeds[2]*timeDelta;
            if (abs(yawVel) > hexapod.jointMaxAngularSpeeds[0] || abs(liftVel) > hexapod.jointMaxAngularSpeeds[1] || abs(yawVel) > hexapod.jointMaxAngularSpeeds[2])
              cout << "WARNING: MAXIMUM SPEED EXCEEDED! Clamping to maximum angular speed for leg " << l << " side " << s << endl;
          }
          
          interface->setTargetAngle(l, s, 0, yaw);

          interface->setTargetAngle(l, s, 1, lift);
          interface->setTargetAngle(l, s, 2, knee);
          
          walker.model->legs[l][s].debugOldYaw = yaw;
          walker.model->legs[l][s].debugOldLiftAngle = lift;
          walker.model->legs[l][s].debugOldKneeAngle = knee;
        }
      }
      interface->publish();
    }
    
    firstFrame = false;
    ros::spinOnce();
    r.sleep();

    debug.reset();
  }
}

/***********************************************************************************************************************
 * Joypad Callback
***********************************************************************************************************************/
void joypadVelocityCallback(const geometry_msgs::Twist &twist)
{
  localVelocity = Vector2d(twist.linear.x, twist.linear.y);
  localVelocity = clamped(localVelocity, 1.0);
  turnRate = twist.angular.x; //RS ROTATION CONTROL SCHEME
  //turnRate = (twist.linear.z-twist.angular.z)/2; //TRIGGER ROTATION CONTROL SCHEME
}

void joypadPoseCallback(const geometry_msgs::Twist &twist)
{
   //ADJUSTED FOR SENSITIVITY OF JOYSTICK
  rollJoy = twist.angular.x*0.075;
  pitchJoy = twist.angular.y*-0.075;
  yawJoy = twist.angular.z*0.2;  
  xJoy = twist.linear.x*0.05;
  yJoy = twist.linear.y*0.05; 
  zJoy = twist.linear.z*0.05; 
}

void startCallback(const std_msgs::Bool &startBool)
{
  if (startBool.data == true)
    startFlag = true;
  else
    startFlag = false;
}



/***********************************************************************************************************************
 * Calculates pitch for body compensation
***********************************************************************************************************************/
double getPitchCompensation(WalkController walker)
{
  double pitch;
  double amplitude = walker.params.pitchAmplitude;
  double phase = walker.legSteppers[0][0].phase;
  double phaseLength = walker.params.stancePhase + walker.params.swingPhase;
  double buffer = walker.params.phaseOffset/2;
  double p0[2] = {0*phaseLength/6, -amplitude};
  double p1[2] = {1*phaseLength/6 + buffer, -amplitude};
  double p2[2] = {2*phaseLength/6 + buffer, amplitude};
  double p3[2] = {4*phaseLength/6 + buffer, amplitude};
  double p4[2] = {5*phaseLength/6 + buffer, -amplitude};
  double p5[2] = {6*phaseLength/6, -amplitude};
    
  if (phase >= p0[0] && phase < p1[0])
    pitch = p0[1];
  else if (phase >= p1[0] && phase < p2[0])
  {
    double gradient = (p2[1]-p1[1])/(p2[0]-p1[0]);
    double offset = ((p2[0]-p1[0])/2 + p1[0]);
    pitch = gradient*phase - gradient*offset;   //-2*phase/3 + 4;
  }
  else if (phase >= p2[0] && phase < p3[0])
    pitch = p2[1];
  else if (phase >= p3[0] && phase < p4[0])
  {
    double gradient = (p4[1]-p3[1])/(p4[0]-p3[0]);
    double offset = ((p4[0]-p3[0])/2 + p3[0]);
    pitch = gradient*phase - gradient*offset;   //2*phase/3 - 10;
  }
  else if (phase >= p4[0] && phase < p5[0])
    pitch = p4[1];    
  
  return pitch;    
}


/***********************************************************************************************************************
 * Calculates roll for body compensation
***********************************************************************************************************************/
double getTiltCompensation(WalkController walker)
{ 
  double roll;
  double amplitude = walker.params.rollAmplitude;
  double phase = walker.legSteppers[0][0].phase;
  double phaseLength = walker.params.stancePhase + walker.params.swingPhase;
  double buffer = walker.params.swingPhase/2;
  double p0[2] = {0*phaseLength/6, -amplitude};           
  double p1[2] = {0*phaseLength/6 + buffer, -amplitude}; 
  double p2[2] = {1*phaseLength/6 - buffer, amplitude};
  double p3[2] = {3*phaseLength/6 + buffer, amplitude};
  double p4[2] = {4*phaseLength/6 - buffer, -amplitude};
  double p5[2] = {6*phaseLength/6, -amplitude};
  
  if (phase >= p0[0] && phase < p1[0])
    roll = p0[1];
  else if (phase >= p1[0] && phase < p2[0])
  {
    double gradient = (p2[1]-p1[1])/(p2[0]-p1[0]);
    double offset = ((p2[0]-p1[0])/2 + p1[0]);
    roll = gradient*phase - gradient*offset; //-2*phase + 3;
  }     
  else if (phase >= p2[0] && phase < p3[0])
    roll = p2[1];
  else if (phase >= p3[0] && phase < p4[0])
  {
    double gradient = (p4[1]-p3[1])/(p4[0]-p3[0]);
    double offset = ((p4[0]-p3[0])/2 + p3[0]);
    roll = gradient*phase - gradient*offset; //2*phase - 21;      
  }
  else if (phase >= p4[0] && phase < p5[0])
    roll = p4[1];
  
  return roll;  
}


/***********************************************************************************************************************
 * Gets hexapod parameters from rosparam server
***********************************************************************************************************************/
void getParameters(ros::NodeHandle n, Parameters *params)
{
  std::string paramString;
  
  // Hexapod Parameters
  if(!n.getParam("hexapod_type", params->hexapodType))
  {
    cout << "Error reading parameter/s (hexapod_type) from rosparam" << endl;
    cout << "Check config file is loaded and type is correct" << endl;
  }
  
  if(!n.getParam("move_to_start", params->moveToStart))
  {
    cout << "Error reading parameter/s (move_to_start) from rosparam" << endl;
    cout << "Check config file is loaded and type is correct" << endl;  
  }
  
  if(!n.getParam("imu_compensation", params->imuCompensation))
  {
    cout << "Error reading parameter/s (imu_compensation) from rosparam" << endl;
    cout << "Check config file is loaded and type is correct" << endl;  
  }
  
  if(!n.getParam("auto_compensation", params->autoCompensation))
  {
    cout << "Error reading parameter/s (auto_compensation) from rosparam" << endl;
    cout << "Check config file is loaded and type is correct" << endl;  
  }
  
  if(!n.getParam("pitch_amplitude", params->pitchAmplitude))
  {
    cout << "Error reading parameter/s (pitch_amplitude) from rosparam" << endl;
    cout << "Check config file is loaded and type is correct" << endl;  
  }
  
  if(!n.getParam("roll_amplitude", params->rollAmplitude))
  {
    cout << "Error reading parameter/s (roll_amplitude) from rosparam" << endl;
    cout << "Check config file is loaded and type is correct" << endl;  
  }
  
  if(!n.getParam("manual_compensation", params->manualCompensation))
  {
    cout << "Error reading parameter/s (manual_compensation) from rosparam" << endl;
    cout << "Check config file is loaded and type is correct" << endl;  
  }
  
  std::vector<double> stanceLegYaws(3);
  if(!n.getParam("stance_leg_yaws", stanceLegYaws))
  {
    cout << "Error reading parameter/s (stance_leg_yaws) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  else
  {
    params->stanceLegYaws = Map<Vector3d>(&stanceLegYaws[0], 3);
  }
  
  std::vector<double> yawLimits(3);
  if(!n.getParam("yaw_limits", yawLimits))
  {
    cout << "Error reading parameter/s (yaw_limits) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  else
  {
    params->yawLimits = Map<Vector3d>(&yawLimits[0], 3);
  }
    
  std::vector<double> kneeLimits(2);
  if(!n.getParam("knee_limits", kneeLimits))
  {
    cout << "Error reading parameter/s (knee_limits) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  else
  {
    params->kneeLimits = Map<Vector2d>(&kneeLimits[0], 2);
  }
  
  std::vector<double> hipLimits(2);    
  if(!n.getParam("hip_limits", hipLimits))
  {
    cout << "Error reading parameter/s (hip_limits) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  else
  {
    params->hipLimits = Map<Vector2d>(&hipLimits[0], 2);
  }
  
  std::vector<double> jointMaxAngularSpeeds(2); 
  if(!n.getParam("joint_max_angular_speeds", jointMaxAngularSpeeds))
  {
    cout << "Error reading parameter/s (joint_max_angular_speed) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  else
  {
    params->jointMaxAngularSpeeds = Map<Vector3d>(&jointMaxAngularSpeeds[0], 3);
  }
  
  if(!n.getParam("dynamixel_interface", params->dynamixelInterface))
  {
    cout << "Error reading parameter/s (dynamixel_interface) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  
  // Walk Controller Parameters
  if (!n.getParam("step_frequency", params->stepFrequency))
  {
    cout << "Error reading parameter/s (step_frequency) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  
  if (!n.getParam("step_clearance", params->stepClearance))
  {
    cout << "Error reading parameter/s (step_clearance) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  
  if (!n.getParam("body_clearance", params->bodyClearance))
  {
    cout << "Error reading parameter/s (body_clearance) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  
  if (!n.getParam("leg_span_scale", params->legSpanScale))
  {
    cout << "Error reading parameter/s (leg_span_scale) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  
  if (!n.getParam("max_acceleration", params->maxAcceleration))
  {
    cout << "Error reading parameter/s (max_acceleration) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  
  if (!n.getParam("max_curvature_speed", params->maxCurvatureSpeed))
  {
    cout << "Error reading parameter/s (max_curvature_speed) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  
  if (!n.getParam("step_curvature_allowance", params->stepCurvatureAllowance))
  {
    cout << "Error reading parameter/s (step_curvature_allowance) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  
  if (!n.getParam("interface_setup_speed", params->interfaceSetupSpeed))
  {
    cout << "Error reading parameter/s (interface_setup_speed) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  
  // Gait Parameters  
  if (!n.getParam("gait_type", params->gaitType))
  {
    cout << "Error reading parameter/s (gaitType) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  
  paramString = params->gaitType+"_parameters/stance_phase";
  if (!n.getParam(paramString, params->stancePhase))
  {
    cout << "Error reading parameter/s (stance_phase) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  
  paramString = params->gaitType+"_parameters/swing_phase";
  if (!n.getParam(paramString, params->swingPhase))
  {
    cout << "Error reading parameter/s (swing_phase) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  
  paramString = params->gaitType+"_parameters/phase_offset";
  if (!n.getParam(paramString, params->phaseOffset))
  {
    cout << "Error reading parameter/s (phase_offset) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  
  paramString = params->gaitType+"_parameters/leg_selection_pattern";
  if (!n.getParam(paramString, params->legSelectionPattern))
  {
    cout << "Error reading parameter/s (leg_selection_pattern) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  
  paramString = params->gaitType+"_parameters/side_selection_pattern";
  if (!n.getParam(paramString, params->sideSelectionPattern))
  {
    cout << "Error reading parameter/s (side_selection_pattern) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
  
  paramString = params->gaitType+"_parameters/transition_period";
  if (!n.getParam(paramString, params->transitionPeriod))
  {
    cout << "Error reading parameter/s (transition_period) from rosparam" <<endl; 
    cout << "Check config file is loaded and type is correct" << endl;
  }
}

/***********************************************************************************************************************
 * Gets ALL joint positions from joint state messages
***********************************************************************************************************************/
void jointStatesCallback(const sensor_msgs::JointState &joint_States)
{  
  if (!jointPosFlag)
  {
    for (int i=0; i<joint_States.name.size(); i++)
    {
      const char* jointName = joint_States.name[i].c_str();
      if (!strcmp(jointName, "front_left_body_coxa") ||
          !strcmp(jointName, "AL_coxa_joint"))
        jointPositions[0] = joint_States.position[i];
      else if (!strcmp(jointName, "front_left_coxa_femour") ||
                !strcmp(jointName, "AL_femur_joint"))
        jointPositions[1] = joint_States.position[i];
      else if (!strcmp(jointName, "front_left_femour_tibia") ||
                !strcmp(jointName, "AL_tibia_joint"))
        jointPositions[2] = joint_States.position[i];
      else if (!strcmp(jointName, "front_right_body_coxa") ||
                !strcmp(jointName, "AR_coxa_joint"))
        jointPositions[3] = joint_States.position[i];
      else if (!strcmp(jointName, "front_right_coxa_femour") ||
                !strcmp(jointName, "AR_femur_joint"))
        jointPositions[4] = joint_States.position[i];
      else if (!strcmp(jointName, "front_right_femour_tibia") ||
                !strcmp(jointName, "AR_tibia_joint"))
        jointPositions[5] = joint_States.position[i];
      else if (!strcmp(jointName, "middle_left_body_coxa") ||
                !strcmp(jointName, "BL_coxa_joint"))
        jointPositions[6] = joint_States.position[i];
      else if (!strcmp(jointName, "middle_left_coxa_femour") ||
                !strcmp(jointName, "BL_femur_joint"))
        jointPositions[7] = joint_States.position[i];
      else if (!strcmp(jointName, "middle_left_femour_tibia") ||
                !strcmp(jointName, "BL_tibia_joint"))
        jointPositions[8] = joint_States.position[i];
      else if (!strcmp(jointName, "middle_right_body_coxa") ||
                !strcmp(jointName, "BR_coxa_joint"))
        jointPositions[9] = joint_States.position[i];
      else if (!strcmp(jointName, "middle_right_coxa_femour") ||
                !strcmp(jointName, "BR_femur_joint"))
        jointPositions[10] = joint_States.position[i];
      else if (!strcmp(jointName, "middle_right_femour_tibia") ||
                !strcmp(jointName, "BR_tibia_joint"))
        jointPositions[11] = joint_States.position[i];
      else if (!strcmp(jointName, "rear_left_body_coxa") ||
                !strcmp(jointName, "CL_coxa_joint"))
        jointPositions[12] = joint_States.position[i];
      else if (!strcmp(jointName, "rear_left_coxa_femour") ||
                !strcmp(jointName, "CL_femur_joint"))
        jointPositions[13] = joint_States.position[i];
      else if (!strcmp(jointName, "rear_left_femour_tibia") ||
                !strcmp(jointName, "CL_tibia_joint"))
        jointPositions[14] = joint_States.position[i];
      else if (!strcmp(jointName, "rear_right_body_coxa") ||
                !strcmp(jointName, "CR_coxa_joint"))
        jointPositions[15] = joint_States.position[i];
      else if (!strcmp(jointName, "rear_right_coxa_femour") ||
                !strcmp(jointName, "CR_femur_joint"))
        jointPositions[16] = joint_States.position[i];
      else if (!strcmp(jointName, "rear_right_femour_tibia") ||
                !strcmp(jointName, "CR_tibia_joint"))
        jointPositions[17] = joint_States.position[i];
    }
    
    //Check if all joint positions have been received from topic
    jointPosFlag = true;
    for (int i=0; i<18; i++)
    {  
      if (jointPositions[i] > 1e9)
        jointPosFlag = false;
    }
  }
}