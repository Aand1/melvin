/*********************************************************************
*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2009, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of Willow Garage, Inc. nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
* Author: Eitan Marder-Eppstein
*********************************************************************/
#include <pd_local_planner/pd_planner.h>
#include <base_local_planner/goal_functions.h>
#include <base_local_planner/map_grid_cost_point.h>
#include <cmath>

//for computing path distance
#include <queue>

#include <angles/angles.h>

#include <ros/ros.h>

#include <pcl_conversions/pcl_conversions.h>

#include <visualization_msgs/Marker.h>

#define PI 3.1415926

namespace pd_local_planner {
  void PDPlanner::reconfigure(PDPlannerConfig &config)
  {

    boost::mutex::scoped_lock l(configuration_mutex_);

    generator_.setParameters(config.sim_time, config.sim_granularity, config.angular_sim_granularity, config.use_dwa, sim_period_);
    path_p_ = config.path_p;
    path_d_ = config.path_d;
    max_vel_x_ = config.max_vel_x;
    min_vel_x_ = config.min_vel_x;
    max_vel_y_ = config.max_vel_y;
    min_vel_y_ = config.min_vel_y;
    // TODO read in param correctly
    max_rot_vel_ = 0.15;//config.max_rot_vel;
    min_rot_vel_ = -0.15;//config.min_rot_vel;

    double resolution = planner_util_->getCostmap()->getResolution();
    pdist_scale_ = config.path_distance_bias;
    // pdistscale used for both path and alignment, set  forward_point_distance to zero to discard alignment
    path_costs_.setScale(resolution * pdist_scale_ * 0.5);
    alignment_costs_.setScale(resolution * pdist_scale_ * 0.5);

    gdist_scale_ = config.goal_distance_bias;
    goal_costs_.setScale(resolution * gdist_scale_ * 0.5);
    goal_front_costs_.setScale(resolution * gdist_scale_ * 0.5);

    occdist_scale_ = config.occdist_scale;
    obstacle_costs_.setScale(resolution * occdist_scale_);

    stop_time_buffer_ = config.stop_time_buffer;
    oscillation_costs_.setOscillationResetDist(config.oscillation_reset_dist, config.oscillation_reset_angle);
    forward_point_distance_ = config.forward_point_distance;
    goal_front_costs_.setXShift(forward_point_distance_);
    alignment_costs_.setXShift(forward_point_distance_);

    // obstacle costs can vary due to scaling footprint feature
    obstacle_costs_.setParams(config.max_trans_vel, config.max_scaling_factor, config.scaling_speed);

    int vx_samp, vy_samp, vth_samp;
    vx_samp = config.vx_samples;
    vy_samp = config.vy_samples;
    vth_samp = config.vth_samples;

    if (vx_samp <= 0) {
      ROS_WARN("You've specified that you don't want any samples in the x dimension. We'll at least assume that you want to sample one value... so we're going to set vx_samples to 1 instead");
      vx_samp = 1;
      config.vx_samples = vx_samp;
    }

    if (vy_samp <= 0) {
      ROS_WARN("You've specified that you don't want any samples in the y dimension. We'll at least assume that you want to sample one value... so we're going to set vy_samples to 1 instead");
      vy_samp = 1;
      config.vy_samples = vy_samp;
    }

    if (vth_samp <= 0) {
      ROS_WARN("You've specified that you don't want any samples in the th dimension. We'll at least assume that you want to sample one value... so we're going to set vth_samples to 1 instead");
      vth_samp = 1;
      config.vth_samples = vth_samp;
    }

    vsamples_[0] = vx_samp;
    vsamples_[1] = vy_samp;
    vsamples_[2] = vth_samp;
  }

  PDPlanner::PDPlanner(std::string name, base_local_planner::LocalPlannerUtil *planner_util) :
      planner_util_(planner_util),
      obstacle_costs_(planner_util->getCostmap()),
      path_costs_(planner_util->getCostmap()),
      goal_costs_(planner_util->getCostmap(), 0.0, 0.0, true),
      goal_front_costs_(planner_util->getCostmap(), 0.0, 0.0, true),
      alignment_costs_(planner_util->getCostmap())
  {
    ros::NodeHandle private_nh("~/" + name);

    goal_front_costs_.setStopOnFailure( false );
    alignment_costs_.setStopOnFailure( false );

    //Assuming this planner is being run within the navigation stack, we can
    //just do an upward search for the frequency at which its being run. This
    //also allows the frequency to be overwritten locally.
    std::string controller_frequency_param_name;
    if(!private_nh.searchParam("controller_frequency", controller_frequency_param_name)) {
      sim_period_ = 0.05;
    } else {
      double controller_frequency = 0;
      private_nh.param(controller_frequency_param_name, controller_frequency, 20.0);
      if(controller_frequency > 0) {
        sim_period_ = 1.0 / controller_frequency;
      } else {
        ROS_WARN("A controller_frequency less than 0 has been set. Ignoring the parameter, assuming a rate of 20Hz");
        sim_period_ = 0.05;
      }
    }
    ROS_INFO("Sim period is set to %.2f", sim_period_);

    oscillation_costs_.resetOscillationFlags();

    bool sum_scores;
    private_nh.param("sum_scores", sum_scores, false);
    obstacle_costs_.setSumScores(sum_scores);


    private_nh.param("publish_cost_grid_pc", publish_cost_grid_pc_, false);
    map_viz_.initialize(name, planner_util->getGlobalFrame(), boost::bind(&PDPlanner::getCellCosts, this, _1, _2, _3, _4, _5, _6));

    std::string frame_id;
    private_nh.param("global_frame_id", frame_id, std::string("odom"));

    traj_cloud_ = new pcl::PointCloud<base_local_planner::MapGridCostPoint>;
    traj_cloud_->header.frame_id = frame_id;
    traj_cloud_pub_.advertise(private_nh, "trajectory_cloud", 1);
    private_nh.param("publish_traj_pc", publish_traj_pc_, false);

    // set up all the cost functions that will be applied in order
    // (any function returning negative values will abort scoring, so the order can improve performance)
    std::vector<base_local_planner::TrajectoryCostFunction*> critics;
    critics.push_back(&oscillation_costs_); // discards oscillating motions (assisgns cost -1)
    critics.push_back(&obstacle_costs_); // discards trajectories that move into obstacles
    critics.push_back(&goal_front_costs_); // prefers trajectories that make the nose go towards (local) nose goal
    critics.push_back(&alignment_costs_); // prefers trajectories that keep the robot nose on nose path
    critics.push_back(&path_costs_); // prefers trajectories on global path
    critics.push_back(&goal_costs_); // prefers trajectories that go towards (local) goal, based on wave propagation

    // trajectory generators
    std::vector<base_local_planner::TrajectorySampleGenerator*> generator_list;
    generator_list.push_back(&generator_);

    scored_sampling_planner_ = base_local_planner::SimpleScoredSamplingPlanner(generator_list, critics);

    private_nh.param("cheat_factor", cheat_factor_, 1.0);

    index_ = 0;
    last_point(0) = -1;
    last_point(1) = -1;
    last_point(2) = -10;
    last_err = 0.0;
  }

  // used for visualization only, total_costs are not really total costs
  bool PDPlanner::getCellCosts(int cx, int cy, float &path_cost, float &goal_cost, float &occ_cost, float &total_cost) {

    path_cost = path_costs_.getCellCosts(cx, cy);
    goal_cost = goal_costs_.getCellCosts(cx, cy);
    occ_cost = planner_util_->getCostmap()->getCost(cx, cy);
    if (path_cost == path_costs_.obstacleCosts() || path_cost == path_costs_.unreachableCellCosts() || occ_cost >= costmap_2d::INSCRIBED_INFLATED_OBSTACLE) {
      return false;
    }

    double resolution = planner_util_->getCostmap()->getResolution();
    total_cost = pdist_scale_ * resolution * path_cost + gdist_scale_ * resolution * goal_cost + occdist_scale_ * occ_cost;
    return true;
  }

  bool PDPlanner::setPlan(const std::vector<geometry_msgs::PoseStamped>& orig_global_plan) {
    oscillation_costs_.resetOscillationFlags();
    return planner_util_->setPlan(orig_global_plan);
  }

  /**
   * This function is used when other strategies are to be applied,
   * but the cost functions for obstacles are to be reused.
   */
  bool PDPlanner::checkTrajectory(Eigen::Vector3f pos, Eigen::Vector3f vel, Eigen::Vector3f vel_samples)
  {
    oscillation_costs_.resetOscillationFlags();
    base_local_planner::Trajectory traj;
    geometry_msgs::PoseStamped goal_pose = global_plan_.back();
    Eigen::Vector3f goal(goal_pose.pose.position.x, goal_pose.pose.position.y, tf::getYaw(goal_pose.pose.orientation));
    base_local_planner::LocalPlannerLimits limits = planner_util_->getCurrentLimits();
    generator_.initialise(pos, vel, goal, &limits, vsamples_);
    generator_.generateTrajectory(pos, vel, vel_samples, traj);
    double cost = scored_sampling_planner_.scoreTrajectory(traj, -1);
    //if the trajectory is a legal one... the check passes
    if(cost >= 0) {
      return true;
    }
    ROS_WARN("Invalid Trajectory %f, %f, %f, cost: %f", vel_samples[0], vel_samples[1], vel_samples[2], cost);

    //otherwise the check fails
    return false;
  }


  void PDPlanner::updatePlanAndLocalCosts(tf::Stamped<tf::Pose> global_pose, const std::vector<geometry_msgs::PoseStamped>& new_plan)
  {
    global_plan_.resize(new_plan.size());
    for (unsigned int i = 0; i < new_plan.size(); ++i) {
      global_plan_[i] = new_plan[i];
    }

    // costs for going away from path
    path_costs_.setTargetPoses(global_plan_);

    // costs for not going towards the local goal as much as possible
    goal_costs_.setTargetPoses(global_plan_);

    // alignment costs
    geometry_msgs::PoseStamped goal_pose = global_plan_.back();

    Eigen::Vector3f pos(global_pose.getOrigin().getX(), global_pose.getOrigin().getY(), tf::getYaw(global_pose.getRotation()));
    double sq_dist = (pos[0] - goal_pose.pose.position.x) * (pos[0] - goal_pose.pose.position.x) + (pos[1] - goal_pose.pose.position.y) * (pos[1] - goal_pose.pose.position.y);

    // we want the robot nose to be drawn to its final position
    // (before robot turns towards goal orientation), not the end of the
    // path for the robot center. Choosing the final position after
    // turning towards goal orientation causes instability when the
    // robot needs to make a 180 degree turn at the end
    std::vector<geometry_msgs::PoseStamped> front_global_plan = global_plan_;
    double angle_to_goal = atan2(goal_pose.pose.position.y - pos[1], goal_pose.pose.position.x - pos[0]);
    front_global_plan.back().pose.position.x = front_global_plan.back().pose.position.x + forward_point_distance_ * cos(angle_to_goal);
    front_global_plan.back().pose.position.y = front_global_plan.back().pose.position.y + forward_point_distance_ * sin(angle_to_goal);

    goal_front_costs_.setTargetPoses(front_global_plan);

    // keeping the nose on the path
    if (sq_dist > forward_point_distance_ * forward_point_distance_ * cheat_factor_) {
      double resolution = planner_util_->getCostmap()->getResolution();
      alignment_costs_.setScale(resolution * pdist_scale_ * 0.5);
      // costs for robot being aligned with path (nose on path, not ju
      alignment_costs_.setTargetPoses(global_plan_);
    } else {
      // once we are close to goal, trying to keep the nose close to anything destabilizes behavior.
      alignment_costs_.setScale(0.0);
    }
  }

  /*
   * given the current state of the robot, find a good trajectory
   */
  base_local_planner::Trajectory PDPlanner::findBestPath(tf::Stamped<tf::Pose> global_pose, tf::Stamped<tf::Pose> global_vel, tf::Stamped<tf::Pose>& drive_velocities, std::vector<geometry_msgs::Point> footprint_spec)
  {
    obstacle_costs_.setFootprint(footprint_spec);

    //make sure that our configuration doesn't change mid-run
    boost::mutex::scoped_lock l(configuration_mutex_);

    Eigen::Vector3f pos(global_pose.getOrigin().getX(), global_pose.getOrigin().getY(), tf::getYaw(global_pose.getRotation()));
    Eigen::Vector3f vel(global_vel.getOrigin().getX(), global_vel.getOrigin().getY(), tf::getYaw(global_vel.getRotation()));
    geometry_msgs::PoseStamped goal_pose = global_plan_.back();
    Eigen::Vector3f goal(goal_pose.pose.position.x, goal_pose.pose.position.y, tf::getYaw(goal_pose.pose.orientation));
    base_local_planner::LocalPlannerLimits limits = planner_util_->getCurrentLimits();

    // prepare cost functions and generators for this run
    generator_.initialise(pos, vel, goal, &limits, vsamples_);

    result_traj_.cost_ = -7;
    // find best trajectory by sampling and scoring the samples
    std::vector<base_local_planner::Trajectory> all_explored;
    scored_sampling_planner_.findBestTrajectory(result_traj_, &all_explored);

    ////////
    index_ = 0;
    double dist_path = 0;
    geometry_msgs::PoseStamped goal_pose1 = global_plan_.at(index_);
    Eigen::Vector3f goal1(goal_pose1.pose.position.x, goal_pose1.pose.position.y, tf::getYaw(goal_pose1.pose.orientation));
    double x1 = (goal1(0) - last_point(0))*(goal1(0) - last_point(0));
    double y1 = (goal1(1) - last_point(1))*(goal1(1) - last_point(1));
    double d1 = x1 + y1;

    for(int i = 1; i < global_plan_.size() - 1; i++) // Find point that we are currently on
    {
      geometry_msgs::PoseStamped goal_pose2 = global_plan_.at(i);
      Eigen::Vector3f goal2(goal_pose2.pose.position.x, goal_pose2.pose.position.y, tf::getYaw(goal_pose2.pose.orientation));
      double x2 = (goal2(0) - last_point(0))*(goal2(0) - last_point(0));
      double y2 = (goal2(1) - last_point(1))*(goal2(1) - last_point(1));
      if(x2+y2 < x1+y1)
      {
        index_ = i;
        x1 = x2;
        y1 = y2;
        dist_path = sqrt(x1 + y1);
      }
    }


    if(index_ < global_plan_.size() - 2)
    {
      goal_pose1 = global_plan_.at(index_);
      goal1[0] = goal_pose1.pose.position.x;
      goal1[1] = goal_pose1.pose.position.y;
      goal1[2] = tf::getYaw(goal_pose1.pose.orientation);
      x1 = (goal1(0) - pos(0))*(goal1(0) - pos(0));
      y1 = (goal1(1) - pos(1))*(goal1(1) - pos(1));

      geometry_msgs::PoseStamped goal_pose2 = global_plan_.at(index_ + 1);
      Eigen::Vector3f goal2(goal_pose2.pose.position.x, goal_pose2.pose.position.y, tf::getYaw(goal_pose2.pose.orientation));
      double x2 = (goal2(0) - pos(0))*(goal2(0) - pos(0));
      double y2 = (goal2(1) - pos(1))*(goal2(1) - pos(1));

      if(x2 + y2 < x1 + y1)
      {
        index_ ++;
      }
    }

    // TODO: Forward simulate in time not distance
    double scale_ = 1.0;
    int index0_ = index_;
    int step_ = -1;

    geometry_msgs::PoseStamped pose1 = global_plan_.at(index_);
    Eigen::Vector3f pose1v(pose1.pose.position.x, pose1.pose.position.y, tf::getYaw(pose1.pose.orientation));
    for(int i = index_ + 1; i < global_plan_.size() - 2; i++)
    {
      geometry_msgs::PoseStamped pose2 = global_plan_.at(i);
      Eigen::Vector3f pose2v(pose2.pose.position.x, pose2.pose.position.y, tf::getYaw(pose2.pose.orientation));
      double dx = pose1v(0) - pose2v(0);
      double dy = pose1v(1) - pose2v(1);
      // double dt = pose1v(2) - pose2v(2);
      double dr = sqrt(dx*dx + dy*dy);
      if(dr > 0.3) // TODO: Configure carrot distance
      {
        step_ = i - index_;
        break;
      }
    }

    if(step_ > 0)
    {
      index_ = index_ + step_;
      scale_ = step_;
    }
    else
    {
      scale_ = global_plan_.size() - index_;
      index_ = global_plan_.size() - 2;
    }

    goal_pose1 = global_plan_.at(index0_);
    goal1[0] = goal_pose1.pose.position.x;
    goal1[1] = goal_pose1.pose.position.y;
    goal1[2] = tf::getYaw(goal_pose1.pose.orientation);

    last_point = goal1;

    geometry_msgs::PoseStamped goal_pose2 = global_plan_.at(index_ + 1);
    Eigen::Vector3f goal2(goal_pose2.pose.position.x, goal_pose2.pose.position.y, tf::getYaw(goal_pose2.pose.orientation));

    // Get vector from last point to next point
    Eigen::Vector3f ideal(goal2(0)-goal1(0),goal2(1)-goal1(1),goal2(2)-goal1(2));

    // Get vector from pose to next point
    Eigen::Vector3f current(goal2(0)-pos(0),goal2(1)-pos(1),goal2(2)-pos(2));

    double remaining_theta = atan2(current(1), current(0)) - pos(2);
    if(remaining_theta > PI)
    {
      remaining_theta -= 2*PI;
    }
    else if(remaining_theta < -PI)
    {
      remaining_theta += 2*PI;
    }

    result_traj_.thetav_ = remaining_theta/scale_/0.1;

    ROS_DEBUG("xv: %.4f dt: %.2f robott: %.4f nextt: %.4f", result_traj_.xv_, remaining_theta*180/PI, pos(2)*180/PI, goal2(2)*180/PI);

    if(step_ > scale_ && remaining_theta > PI/10) // If close to the end turn to get there
    {
      result_traj_.xv_ = 0.0;
    }
    else if(fabs(pos(2)-goal2(2)) < PI/4.0 && fabs(remaining_theta) > 3.0*PI/4.0) // If need to move backwards ... do it
    {
      result_traj_.xv_ = -min_vel_x_;
      if(remaining_theta > 3.0*PI/4.0)
      {
        remaining_theta -= PI;
      }
      else
      {
        remaining_theta += PI;
      }
      result_traj_.thetav_ = remaining_theta/scale_/0.1;
      // Read in param for escape_velocity
      result_traj_.xv_ = -0.2; //Using angle off
      ROS_DEBUG("new vel (reverse): %.4f newt: %.4f remaining_theta: %.4f", result_traj_.xv_, result_traj_.thetav_, remaining_theta);
    }
    else // If normal forward operation (also handles needing to turn)
    {
      if(step_ > scale_)
      {
        result_traj_.xv_ = min_vel_x_;
      }
      else
      {
        result_traj_.xv_ = max_vel_x_;
      }
      double range = PI/5.0;
      double scale_xv = 1.0f - fabs(remaining_theta/range);
      if(scale_xv < 0.0 || remaining_theta > range)
      {
        result_traj_.xv_ = 0.0;
      }
      else
      {
        result_traj_.xv_ *= scale_xv; //Using angle off
      }
      ROS_DEBUG("Percent max speed (forward): %.4f new vel: %.4f", scale_xv, result_traj_.xv_);
    }

    double err = ideal(0)*current(1) - ideal(1)*current(0);

    // Use error in PD control to modify result_traj
    double derr = (err - last_err)/0.05;

    result_traj_.thetav_ += err*path_p_ + derr*path_d_;

    ROS_DEBUG("pre_throttled_cmd_vel: (%.4f %.4f %.4f)", result_traj_.xv_, result_traj_.yv_, result_traj_.thetav_);

    Eigen::Vector3f vel_test(result_traj_.xv_, result_traj_.yv_, 0);
    if(fabs(remaining_theta) < PI/18 && dist_path < 0.2 && checkTrajectory(pos, vel, vel_test))
    {
      result_traj_.thetav_ = 0;
    }

    // vx bounds
    if(result_traj_.xv_ > max_vel_x_)
    {
      ROS_DEBUG("vx+ too big");
      result_traj_.xv_ = max_vel_x_;
    }
    else if(result_traj_.xv_ > min_vel_x_/2 && result_traj_.xv_ < min_vel_x_)
    {
      ROS_DEBUG("vx+ too small");
      result_traj_.xv_ = min_vel_x_;
    }
    else if(result_traj_.xv_ < -min_vel_x_/2 && result_traj_.xv_ > -min_vel_x_)
    {
      ROS_DEBUG("vx- too small");
      result_traj_.xv_ = -min_vel_x_;
    }
    // Read in param for escape_velocity
    else if(result_traj_.xv_ < -min_vel_x_)
    {
      ROS_DEBUG("vx- too big");
      result_traj_.xv_ = -min_vel_x_;
    }

    // vt bounds
    if(result_traj_.thetav_ > max_rot_vel_)
    {
      ROS_DEBUG("vt+ too big");
      result_traj_.thetav_ = max_rot_vel_;
    }
    else if(result_traj_.thetav_ < min_rot_vel_)
    {
      ROS_DEBUG("vt- too big");
      result_traj_.thetav_ = min_rot_vel_;
    }

    Eigen::Vector3f vel_samples(result_traj_.xv_, result_traj_.yv_, result_traj_.thetav_);

    // Check for collisions
    if(!checkTrajectory(pos, vel, vel_samples))
    {
      ROS_WARN("DETECTED A COLLISION AHEAD! (%.4f %.4f %.4f)", result_traj_.xv_, result_traj_.xv_, result_traj_.thetav_);
      // TODO make this have some kind of recovery behavior
      result_traj_.xv_ = 0.0;
      result_traj_.yv_ = 0.0;
      result_traj_.thetav_ = 0.0;
    }

    ROS_INFO("cmd_vel: (%.4f %.4f %.4f)", result_traj_.xv_, result_traj_.yv_, result_traj_.thetav_);

    last_err = err;
    ////////

    if(publish_traj_pc_)
    {
        base_local_planner::MapGridCostPoint pt;
        traj_cloud_->points.clear();
        traj_cloud_->width = 0;
        traj_cloud_->height = 0;
        std_msgs::Header header;
        pcl_conversions::fromPCL(traj_cloud_->header, header);
        header.stamp = ros::Time::now();
        traj_cloud_->header = pcl_conversions::toPCL(header);
        for(std::vector<base_local_planner::Trajectory>::iterator t=all_explored.begin(); t != all_explored.end(); ++t)
        {
            if(t->cost_<0)
                continue;
            // Fill out the plan
            for(unsigned int i = 0; i < t->getPointsSize(); ++i) {
                double p_x, p_y, p_th;
                t->getPoint(i, p_x, p_y, p_th);
                pt.x=p_x;
                pt.y=p_y;
                pt.z=0;
                pt.path_cost=p_th;
                pt.total_cost=t->cost_;
                traj_cloud_->push_back(pt);
            }
        }
        traj_cloud_pub_.publish(*traj_cloud_);
    }

    // verbose publishing of point clouds
    if (publish_cost_grid_pc_) {
      //we'll publish the visualization of the costs to rviz before returning our best trajectory
      map_viz_.publishCostCloud(planner_util_->getCostmap());
    }

    // debrief stateful scoring functions
    oscillation_costs_.updateOscillationFlags(pos, &result_traj_, planner_util_->getCurrentLimits().min_trans_vel);

    //if we don't have a legal trajectory, we'll just command zero
    if (result_traj_.cost_ < 0) {
      drive_velocities.setIdentity();
    } else {
      tf::Vector3 start(result_traj_.xv_, result_traj_.yv_, 0);
      drive_velocities.setOrigin(start);
      tf::Matrix3x3 matrix;
      matrix.setRotation(tf::createQuaternionFromYaw(result_traj_.thetav_));
      drive_velocities.setBasis(matrix);
    }

    return result_traj_;
  }
};
