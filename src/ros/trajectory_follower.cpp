#include <endian.h>
#include "ur_modern_driver/ros/trajectory_follower.h"
  
  
static const int32_t MULT_JOINTSTATE_ = 1000000;
static const std::string JOINT_STATE_REPLACE("{{JOINT_STATE_REPLACE}}");
static const std::string SERVO_J_REPLACE("{{SERVO_J_REPLACE}}");
static const std::string SERVER_IP_REPLACE("{{SERVER_IP_REPLACE}}");
static const std::string SERVER_PORT_REPLACE("{{SERVER_PORT_REPLACE}}");
static const std::string POSITION_PROGRAM = R"(
def driverProg():
	MULT_jointstate = {{JOINT_STATE_REPLACE}}

	SERVO_IDLE = 0
	SERVO_RUNNING = 1
	cmd_servo_state = SERVO_IDLE
	cmd_servo_q = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
	
	def set_servo_setpoint(q):
		enter_critical
		cmd_servo_state = SERVO_RUNNING
		cmd_servo_q = q
		exit_critical
	end
	
	thread servoThread():
		state = SERVO_IDLE
		while True:
			enter_critical
			q = cmd_servo_q
			do_brake = False
			if (state == SERVO_RUNNING) and (cmd_servo_state == SERVO_IDLE):
				do_brake = True
			end
			state = cmd_servo_state
			cmd_servo_state = SERVO_IDLE
			exit_critical
			if do_brake:
				stopj(1.0)
				sync()
			elif state == SERVO_RUNNING:
				servoj(q, {{SERVO_J_REPLACE}})
			else:
				sync()
			end
		end
	end

  socket_open(\"{{SERVER_IP_REPLACE}}\", {{SERVER_PORT_REPLACE}})

  thread_servo = run servoThread()
  keepalive = 1
  while keepalive > 0:
	  params_mult = socket_read_binary_integer(6+1)
	  if params_mult[0] > 0:
		  q = [params_mult[1] / MULT_jointstate, params_mult[2] / MULT_jointstate, params_mult[3] / MULT_jointstate, params_mult[4] / MULT_jointstate, params_mult[5] / MULT_jointstate, params_mult[6] / MULT_jointstate]
		  keepalive = params_mult[7]
		  set_servo_setpoint(q)
	  end
  end
  sleep(.1)
  socket_close()
  kill thread_servo
end
)";

TrajectoryFollower::TrajectoryFollower(URCommander &commander, int reverse_port, bool version_3)
  : running_(false)
  , commander_(commander)
  , reverse_port_(reverse_port)
  , server_(reverse_port)
{
  std::string res(POSITION_PROGRAM);

  res.replace(res.find(JOINT_STATE_REPLACE), JOINT_STATE_REPLACE.length(), std::to_string(MULT_JOINTSTATE_));

  std::ostringstream out;
  out << "t=" << std::fixed << std::setprecision(4) << servoj_time_;
  if(version_3)
    out << ", lookahead_time=" << servoj_lookahead_time_ << ", gain=" << servoj_gain_;

  res.replace(res.find(SERVO_J_REPLACE), SERVO_J_REPLACE.length(), out.str());

  program_ = res;
}

std::string TrajectoryFollower::buildProgram()
{
  std::string res(program_);
  std::string IP(server_.getIP());
  LOG_INFO("Local IP: %s  ", IP.c_str());
  res.replace(res.find(SERVER_IP_REPLACE), SERVER_IP_REPLACE.length(), "127.0.0.1");
  res.replace(res.find(SERVER_PORT_REPLACE), SERVER_PORT_REPLACE.length(), std::to_string(reverse_port_));
  return res;
}

bool TrajectoryFollower::start()
{
  if(running_)
    return true; //not sure

  if(!server_.bind())
  {
    LOG_ERROR("Failed to bind server");
    return false;
  }

  LOG_INFO("Uploading trajectory program to robot");

  std::string prog(buildProgram());
  //std::string prog = "socket_open(\"127.0.0.1\", 50001)\n";
  if(!commander_.uploadProg(prog))
  {
    LOG_ERROR("Program upload failed!");
    return false;
  }

  LOG_INFO("Awaiting incomming robot connection");

  if(!server_.accept())
  {
    LOG_ERROR("Failed to accept incomming robot connection");
    return false;
  }
  
  LOG_INFO("Robot successfully connected");
  return (running_ = true);
}

bool TrajectoryFollower::execute(std::array<double, 6> &positions, bool keep_alive)
{
  if(!running_)
    return false;

  last_positions_ = positions;

  uint8_t buf[sizeof(uint32_t)*7];
  uint8_t *idx = buf;
  
  for(auto const& pos : positions)
  {
    int32_t val = static_cast<int32_t>(pos * MULT_JOINTSTATE_);
    val = htobe32(val);
    idx += append(idx, val);
  }

  int32_t val = htobe32(static_cast<int32_t>(keep_alive));
  append(idx, val);

  size_t written; 
  return server_.write(buf, sizeof(buf), written);
}

double TrajectoryFollower::interpolate(double t, double T, double p0_pos, double p1_pos, double p0_vel, double p1_vel)
{
  using std::pow;
  double a = p0_pos;
  double b = p0_vel;
  double c = (-3 * a + 3 * p1_pos - 2 * T * b - T * p1_vel) / pow(T, 2);
  double d = (2 * a - 2 * p1_pos + T * b + T * p1_vel) / pow(T, 3);
  return a + b * t + c * pow(t, 2) + d * pow(t, 3);
}

bool TrajectoryFollower::execute(std::array<double, 6> &positions)
{
  return execute(positions, true);
}

bool TrajectoryFollower::execute(std::vector<TrajectoryPoint> &trajectory, std::atomic<bool> &interrupt)
{
  if(!running_)
    return false;
  
  using namespace std::chrono;
  typedef duration<double> double_seconds;
  typedef high_resolution_clock Clock;
  typedef Clock::time_point Time;

  auto const& last = trajectory[trajectory.size()-1];
  auto& prev = trajectory[0];

  Time t0 = Clock::now();
  Time latest = t0;

  std::array<double, 6> positions;

  for(auto const& point : trajectory)
  {
    //skip t0
    if(&point == &prev)
      continue;

    auto duration = point.time_from_start - prev.time_from_start;
    double d_s = duration_cast<double_seconds>(duration).count();

    //interpolation loop
    while(!interrupt)
    {
      latest = Clock::now();
      auto elapsed = latest - t0;

      if(point.time_from_start <= elapsed || last.time_from_start >= elapsed)
        break;

      double elapsed_s = duration_cast<double_seconds>(elapsed - prev.time_from_start).count();
      //double prev_seconds
      for(size_t j = 0; j < positions.size(); j++)
      {
        positions[j] = interpolate(
          elapsed_s, 
          d_s, 
          prev.positions[j], 
          point.positions[j],
          prev.velocities[j],
          point.velocities[j]
        );
      }

      if(!execute(positions, true))
        return false;

      std::this_thread::sleep_for(double_seconds(servoj_time_));
    }

    prev = point;
  }

  return true;
}

void TrajectoryFollower::stop()
{
  if(!running_)
    return;

  std::array<double, 6> empty;
  execute(empty, false);

  //server_.disconnect();
  running_ = false;
}