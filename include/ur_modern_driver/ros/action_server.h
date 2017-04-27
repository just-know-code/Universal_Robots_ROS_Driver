#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <ros/ros.h>
#include <actionlib/server/action_server.h>
#include <actionlib/server/server_goal_handle.h>
#include <control_msgs/FollowJointTrajectoryAction.h>
#include "ur_modern_driver/log.h"
#include "ur_modern_driver/ur/consumer.h"
#include "ur_modern_driver/ur/master_board.h"
#include "ur_modern_driver/ur/state.h"
#include "ur_modern_driver/ros/service_stopper.h"
#include "ur_modern_driver/ros/trajectory_follower.h"


class ActionServer : public Service //,public URRTPacketConsumer
{
private:
  typedef control_msgs::FollowJointTrajectoryAction Action;
  typedef control_msgs::FollowJointTrajectoryResult Result;
  typedef actionlib::ServerGoalHandle<Action> GoalHandle;
  typedef actionlib::ActionServer<Action> Server;
  
  ros::NodeHandle nh_;
  Server as_;

  std::vector<std::string> joint_names_;
  std::set<std::string> joint_set_;
  double max_velocity_;
  RobotState state_;

  
  GoalHandle curr_gh_;
  std::atomic<bool> interrupt_traj_;
  std::atomic<bool> has_goal_, running_;
  std::mutex tj_mutex_;
  std::condition_variable tj_cv_;
  std::thread tj_thread_;

  TrajectoryFollower& follower_;

  void onGoal(GoalHandle gh);
  void onCancel(GoalHandle gh);

  bool validate(GoalHandle& gh, Result& res);
  bool validateState(GoalHandle& gh, Result& res);  
  bool validateJoints(GoalHandle& gh, Result& res);
  bool validateTrajectory(GoalHandle& gh, Result& res);

  bool try_execute(GoalHandle& gh, Result& res);
  void interruptGoal(GoalHandle& gh);

  std::vector<size_t> reorderMap(std::vector<std::string> goal_joints);

  void trajectoryThread();


public:
  ActionServer(TrajectoryFollower& follower, std::vector<std::string>& joint_names, double max_velocity);

  void start();
  virtual void onRobotStateChange(RobotState state);
};