#include <object_manipulation/object_manipulation.h>

ObjectManipulation::ObjectManipulation(ros::NodeHandle& nh,
                                       const std::string& labeled_objects_topic,
                                       const std::string& camera_point_cloud_topic)
: nh_{nh},
  labeled_objects_cloud_topic_{labeled_objects_topic},
  camera_point_cloud_topic_{camera_point_cloud_topic},
  move_group_{"arm_torso"},
  visual_tools_{"base_footprint"},
  pickup_ac_{"/pickup", true} {}

bool ObjectManipulation::initalise()
{
    ROS_INFO("Waiting for action server to start.");
    pickup_ac_.waitForServer();
    ROS_INFO("Action server started.");

    labeled_object_cloud_sub_.subscribe(nh_, labeled_objects_cloud_topic_, 10);
    camera_point_cloud_sub_.subscribe(nh_, camera_point_cloud_topic_, 10);
    sync_sub_.reset(new message_filters::Synchronizer<SyncPolicy>(SyncPolicy(10), labeled_object_cloud_sub_, camera_point_cloud_sub_));
    sync_sub_->registerCallback(boost::bind(&ObjectManipulation::cloudCallback, this, _1, _2));

    gpd_ros_grasps_sub_ = nh_.subscribe("/detect_grasps/clustered_grasps", 1, &ObjectManipulation::graspsCallback, this);

    gpd_ros_cloud_pub_ = nh_.advertise<gpd_ros::CloudSamples>("/cloud_stitched", 10);

    // set moveit configurations
    move_group_.setPlannerId("RRTConnectkConfigDefault");
    move_group_.setPlanningTime(2.0);
    // move_group_.setGoalOrientationTolerance(deg2rad(10));
    visual_tools_.loadRobotStatePub("/display_robot_state");

    return true;
}

double ObjectManipulation::deg2rad(const double degrees)
{
    return degrees * M_PI / 180.0;
}

Eigen::Affine3d ObjectManipulation::poseMsgToEigen(const geometry_msgs::Pose& pose_msg)
{
    Eigen::Affine3d transformation_matrix;

    // Translation vector
    Eigen::Vector3d translation(pose_msg.position.x, pose_msg.position.y, pose_msg.position.z);

    // Quaternion for rotation
    Eigen::Quaterniond rotation(
        pose_msg.orientation.w,
        pose_msg.orientation.x,
        pose_msg.orientation.y,
        pose_msg.orientation.z
    );

    // Set the transformation matrix
    transformation_matrix = Eigen::Translation3d(translation) * rotation;

    return transformation_matrix;
}

void ObjectManipulation::createPlanningScene(const std::string& label)
{
    ROS_INFO("Removing any previous collision objects.");
    // remove attached object from pipeline
    moveit_msgs::AttachedCollisionObject att_coll_object;
    att_coll_object.object.id = "target";
    att_coll_object.object.operation = att_coll_object.object.REMOVE;
    planning_interface_.applyAttachedCollisionObject(att_coll_object);
    planning_interface_.removeCollisionObjects(
        planning_interface_.getKnownObjectNames()
    );
    ////TODO: do we need this?
    ros::Duration(2.0).sleep();

    // obtain the currently detected labels
    visualization_msgs::MarkerArrayConstPtr labels = 
        ros::topic::waitForMessage<visualization_msgs::MarkerArray>(
            "/text_markers", nh_, ros::Duration{2.0}
        );
    
    // find the pose of the target object centroid
    bool target_object_found{false};
    geometry_msgs::Pose target_object_pose;
    if (labels != nullptr)
    {
        for (size_t i{0}; i < labels->markers.size(); ++i)
        {
            if (labels->markers[i].text == label)
            {
                target_object_found = true;
                target_object_pose = labels->markers[i].pose;
                target_object_pose.position.z -= 0.1;
                target_object_pose.orientation.x = 0.0;
                target_object_pose.orientation.y = 0.0;
                target_object_pose.orientation.z = 0.0;
                target_object_pose.orientation.w = 1.0;
                break;
            }
        }
        if (!target_object_found)
        {
            ROS_ERROR_STREAM("no label: " << label << " found");
        }
    } else
    {
        ROS_ERROR("no labels detected");
    }

    // find the verticies of the plane to avoid collision with
    sensor_msgs::PointCloud2ConstPtr plane_verticies = 
        ros::topic::waitForMessage<sensor_msgs::PointCloud2>(
            "/table_vertices", nh_, ros::Duration{2.0}
        );


    if (target_object_found && plane_verticies != nullptr)
    {
        sensor_msgs::PointCloud2ConstIterator<float> iter_x(*plane_verticies, "x");
        sensor_msgs::PointCloud2ConstIterator<float> iter_y(*plane_verticies, "y");
        sensor_msgs::PointCloud2ConstIterator<float> iter_z(*plane_verticies, "z");

        // you will always only have two points in this point cloud
        float min_x, min_y, min_z;
        float max_x, max_y, max_z;
        
        min_x = *iter_x;
        min_y = *iter_y;
        min_z = *iter_z;

        max_x = *(++iter_x);
        max_y = *(++iter_y);
        max_z = *(++iter_z);

        // Create a plane collision object
        moveit_msgs::CollisionObject plane_collision_object;
        plane_collision_object.header.frame_id = plane_verticies->header.frame_id;
        plane_collision_object.id = "plane";

        // define the pose of the box in the planning scene representing the plane
        geometry_msgs::Pose plane_pose;
        plane_pose.position.x = (max_x + min_x) / 2.0;
        plane_pose.position.y = (max_y + min_y) / 2.0;
        plane_pose.position.z = (max_z + min_z) / 2.0 - 0.025;
        plane_pose.orientation.w = 1.0;

        // define the shape of the plane box object
        shape_msgs::SolidPrimitive plane_primitive;
        plane_primitive.type = plane_primitive.BOX;
        plane_primitive.dimensions.resize(3);
        plane_primitive.dimensions[plane_primitive.BOX_X] = std::abs(max_x - min_x);
        plane_primitive.dimensions[plane_primitive.BOX_Y] = std::abs(max_y - min_y);
        plane_primitive.dimensions[plane_primitive.BOX_Z] = 0.05;

        plane_collision_object.primitives.push_back(plane_primitive);
        plane_collision_object.primitive_poses.push_back(plane_pose);

        // create a target collision object
        moveit_msgs::CollisionObject target_collision_object;
        target_collision_object.header.frame_id = plane_verticies->header.frame_id;
        target_collision_object.id = "target";

        // define the shape of the target box object
        shape_msgs::SolidPrimitive target_primitive;
        target_primitive.type = target_primitive.BOX;
        target_primitive.dimensions.resize(3);
        target_primitive.dimensions[target_primitive.BOX_X] = 0.08;
        target_primitive.dimensions[target_primitive.BOX_Y] = 0.08;
        target_primitive.dimensions[target_primitive.BOX_Z] = 0.08;

        target_collision_object.primitives.push_back(target_primitive);
        target_collision_object.primitive_poses.push_back(target_object_pose);

        // add objects to the planning scene
        planning_interface_.applyCollisionObject(plane_collision_object);
        ROS_INFO("Added plane collision object.");
        planning_interface_.applyCollisionObject(target_collision_object);
        ROS_INFO("Added target collision object.");
    } else
    {
        ROS_ERROR("no collision objects added to planning scene");
    }
}

moveit_msgs::PickupGoal ObjectManipulation::createPickupGoal(const std::string& group,
                                                             const std::string& target,
                                                             const geometry_msgs::PoseStamped& grasp_pose,
                                                             const std::vector<moveit_msgs::Grasp>& possible_grasps,
                                                             const std::vector<std::string>& links_to_allow_contact)
{
    moveit_msgs::PickupGoal pug;
    pug.target_name = target;
    pug.group_name = group; 
    pug.possible_grasps.insert(
        pug.possible_grasps.begin(), 
        possible_grasps.begin(), 
        possible_grasps.end()
    );
    pug.allowed_planning_time = 35.0;
    pug.planning_options.planning_scene_diff.is_diff = true;
    pug.planning_options.planning_scene_diff.robot_state.is_diff = true;
    pug.planning_options.plan_only = false;
    pug.planning_options.replan = true;
    pug.planning_options.replan_attempts = 1;
    // pug.attached_object_touch_links.push_back("<octomap>");
    pug.attached_object_touch_links.insert(
        pug.attached_object_touch_links.begin(),
        links_to_allow_contact.begin(),
        links_to_allow_contact.end()
    );

    return pug;
}

std::vector<moveit_msgs::Grasp> ObjectManipulation::createGrasps(const gpd_ros::GraspConfigListConstPtr& grasps_msg)
{
    std::vector<moveit_msgs::Grasp> grasps;

    for (size_t idx{0}; idx < grasps_msg->grasps.size(); ++idx)
    {
        gpd_ros::GraspConfig grasp{grasps_msg->grasps[idx]};

        moveit_msgs::Grasp moveit_grasp;
        moveit_grasp.id = "grasp_" + std::to_string(idx);

        trajectory_msgs::JointTrajectory pre_grasp_posture;
        pre_grasp_posture.header.frame_id = "arm_tool_link";
        pre_grasp_posture.joint_names.push_back("gripper_left_finger_joint");
        pre_grasp_posture.joint_names.push_back("gripper_right_finger_joint");

        trajectory_msgs::JointTrajectoryPoint jt_point;
        jt_point.time_from_start = ros::Duration(2.0);
        jt_point.positions.push_back(0.05);
        jt_point.positions.push_back(0.05);
        pre_grasp_posture.points.push_back(jt_point);

        trajectory_msgs::JointTrajectory grasp_posture{pre_grasp_posture};
        grasp_posture.points[0].time_from_start += ros::Duration(2.0);
        trajectory_msgs::JointTrajectoryPoint jt_point2;
        jt_point2.time_from_start = ros::Duration(6.0);
        jt_point2.positions.push_back(0.01);
        jt_point2.positions.push_back(0.01);
        grasp_posture.points.push_back(jt_point2);

        moveit_grasp.pre_grasp_posture = pre_grasp_posture;
        moveit_grasp.grasp_posture = grasp_posture;

        geometry_msgs::PoseStamped grasp_pose;
        grasp_pose.header.frame_id = "base_footprint";
        grasp_pose.pose.position.x = grasp.position.x;
        grasp_pose.pose.position.y = grasp.position.y;
        grasp_pose.pose.position.z = grasp.position.z;

        // convert vectors to rotation matrix
        tf2::Matrix3x3 rotation_matrix{
            grasp.approach.x, grasp.binormal.x, grasp.axis.x,
            grasp.approach.y, grasp.binormal.y, grasp.axis.y,
            grasp.approach.z, grasp.binormal.z, grasp.axis.z,
        };

        // fix the rotation of the gripper from gpd_ros to Tiago's convention
        tf2::Quaternion quaternion;
        rotation_matrix.getRotation(quaternion);
        // rotation about x-axis by -90 deg
        tf2::Vector3 axis{1.0, 0.0, 0.0};
        quaternion *= tf2::Quaternion(axis, -M_PI_2);

        grasp_pose.pose.orientation.x = quaternion.x();
        grasp_pose.pose.orientation.y = quaternion.y();
        grasp_pose.pose.orientation.z = quaternion.z();
        grasp_pose.pose.orientation.w = quaternion.w();

        // shift target pose back slightly to avoid gripper collision
        Eigen::Affine3d T_base_target = poseMsgToEigen(grasp_pose.pose);
        Eigen::Vector3d shift_x_axis{-0.1, 0.0, 0.0};
        auto shifted_position = T_base_target * shift_x_axis;
        grasp_pose.pose.position.x = shifted_position.x();
        grasp_pose.pose.position.y = shifted_position.y();
        grasp_pose.pose.position.z = shifted_position.z();

        moveit_grasp.grasp_pose = grasp_pose;
        moveit_grasp.grasp_quality = grasp.score.data;

        ////TODO: check this is correct
        moveit_grasp.pre_grasp_approach.direction.header.frame_id = "arm_tool_link";
        moveit_grasp.pre_grasp_approach.direction.vector.x = 1.0;
        moveit_grasp.pre_grasp_approach.direction.vector.y = 0.0;
        moveit_grasp.pre_grasp_approach.direction.vector.z = 0.0;
        moveit_grasp.pre_grasp_approach.desired_distance = 0.15;
        moveit_grasp.pre_grasp_approach.min_distance = 0.0;
        ////TODO: check this is correct
        moveit_grasp.post_grasp_retreat.direction.header.frame_id = "arm_tool_link";
        moveit_grasp.post_grasp_retreat.direction.vector.x = -1.0;
        moveit_grasp.post_grasp_retreat.direction.vector.y = 0.0;
        moveit_grasp.post_grasp_retreat.direction.vector.z = 0.0;
        moveit_grasp.post_grasp_retreat.desired_distance = 0.15;
        moveit_grasp.post_grasp_retreat.min_distance = 0.0;

        moveit_grasp.max_contact_force = 0.0;

        grasps.push_back(moveit_grasp);
        ROS_INFO_STREAM("inserted grasp configuration with score: " 
            << grasp.score
        );
    }

    return grasps;
}

void ObjectManipulation::cloudCallback(const sensor_msgs::PointCloud2ConstPtr& labeled_cloud_msg,
                                       const sensor_msgs::PointCloud2ConstPtr& camera_cloud_msg)
{
    // obtain the camera position at the provided cloud_msg timestamp
    tf::StampedTransform T_base_camera;
    tf_listener_.lookupTransform(
        "base_footprint",
        "xtion_rgb_optical_frame",
        ros::Time(0),
        T_base_camera
    );

    // populate the merged cloud information
    gpd_ros::CloudSources gpd_cloud_msg; 
    gpd_cloud_msg.cloud = *camera_cloud_msg;
    gpd_cloud_msg.camera_source = std::vector<std_msgs::Int64>{
        camera_cloud_msg->width * camera_cloud_msg->height,
        std_msgs::Int64{} 
    };
    geometry_msgs::Point camera_position;
    camera_position.x = T_base_camera.getOrigin().x();
    camera_position.y = T_base_camera.getOrigin().y();
    camera_position.z = T_base_camera.getOrigin().z();
    gpd_cloud_msg.view_points = std::vector<geometry_msgs::Point>{camera_position};

    // populate the gpd cloud samples msg for publishing to gpd_ros
    gpd_ros::CloudSamples gpd_cloud_samples_msg;
    gpd_cloud_samples_msg.cloud_sources = gpd_cloud_msg;

    // create a vector of points for which to search for grasp poses
    sensor_msgs::PointCloud2ConstIterator<float> iter_x(*labeled_cloud_msg, "x");
    sensor_msgs::PointCloud2ConstIterator<float> iter_y(*labeled_cloud_msg, "y");
    sensor_msgs::PointCloud2ConstIterator<float> iter_z(*labeled_cloud_msg, "z");
    sensor_msgs::PointCloud2ConstIterator<int> iter_label(*labeled_cloud_msg, "label");

    for (; iter_x != iter_x.end(); ++iter_x, ++iter_y, ++iter_z, ++iter_label)
    {
        ////TODO: for now we only try and detect a cup, which is labeled as 4 
        // if (*iter_label == 4)
        if (*iter_label == 7)
        {
            geometry_msgs::Point sample_point;
            sample_point.x = *iter_x;
            sample_point.y = *iter_y;
            sample_point.z = *iter_z;
            gpd_cloud_samples_msg.samples.push_back(sample_point);
        }
    }

    // publish to gpd_ros
    if (gpd_cloud_samples_msg.samples.size() > 0)
    {
        gpd_ros_cloud_pub_.publish(gpd_cloud_samples_msg);
        // std::cout << "Published cloud sample" << std::endl;
    }
}

void ObjectManipulation::graspsCallback(const gpd_ros::GraspConfigListConstPtr& msg)
{
    ROS_INFO("Obtained possible grasp pose candidates from gpd_ros.");
    // createPlanningScene("cup");
    createPlanningScene("traffic light");
    std::vector<moveit_msgs::Grasp> possible_grasps = createGrasps(msg);
    moveit_msgs::PickupGoal goal = createPickupGoal(
        "arm_torso",
        "target",
        geometry_msgs::PoseStamped{},
        possible_grasps,
        {
            "gripper_left_finger_link",
            "gripper_right_finger_link",
            "gripper_link"
        }
    );
    ROS_INFO("Sending goal.");
    pickup_ac_.sendGoal(goal);
    ROS_INFO("Waiting for result.");
    bool success = pickup_ac_.waitForResult();
    ROS_INFO("Pick result: %s", success ? "SUCCESS" : "FAILED");
}

// void ObjectManipulation::graspsCallback(const gpd_ros::GraspConfigListConstPtr& msg)
// {
//     size_t idx{0};
//     bool success{false};
//     while (success == false && idx < msg->grasps.size())
//     {
//         ROS_INFO_STREAM("selected grasp configuration with score: " << msg->grasps[idx].score);
//         gpd_ros::GraspConfig grasp{msg->grasps[idx]};

//         // set up a pose goal
//         geometry_msgs::Pose target_pose;
//         target_pose.position.x = grasp.position.x;
//         target_pose.position.y = grasp.position.y;
//         // target_pose.position.z = grasp.position.z + 0.2;
//         target_pose.position.z = grasp.position.z;

//         // convert vectors to rotation matrix
//         tf2::Matrix3x3 rotation_matrix{
//             grasp.approach.x, grasp.binormal.x, grasp.axis.x,
//             grasp.approach.y, grasp.binormal.y, grasp.axis.y,
//             grasp.approach.z, grasp.binormal.z, grasp.axis.z,
//         };

//         // convert the rotation matrix into a quaternion
//         tf2::Quaternion quaternion;
//         rotation_matrix.getRotation(quaternion);

//         tf2::Vector3 axis{1.0, 0.0, 0.0};
//         double angle = M_PI / 2.0;

//         quaternion *= tf2::Quaternion(axis, -angle);

//         target_pose.orientation.x = quaternion.x();
//         target_pose.orientation.y = quaternion.y();
//         target_pose.orientation.z = quaternion.z();
//         target_pose.orientation.w = quaternion.w();

//         // before transformation
//         visual_tools_.publishAxis(target_pose, rviz_visual_tools::LARGE);

//         // obtain the transformation matrix from the base to the end effector 
//         Eigen::Affine3d T_base_target = poseMsgToEigen(target_pose);
//         // shift the target pose along its own x-axis by 0.12m
//         Eigen::Vector3d shift_x_axis{-0.25, 0.0, 0.0};
//         auto shifted_position = T_base_target * shift_x_axis;
//         target_pose.position.x = shifted_position.x();
//         target_pose.position.y = shifted_position.y();
//         target_pose.position.z = shifted_position.z();

//         move_group_.setPoseTarget(target_pose);
//         visual_tools_.publishAxis(target_pose, rviz_visual_tools::LARGE);
//         visual_tools_.trigger();

//         // generate planning scene
//         ////TODO: make it generalised
//         // createPlanningScene("cup");
//         createPlanningScene("traffic light");

//         moveit::planning_interface::MoveGroupInterface::Plan move_plan;
//         success = (move_group_.plan(move_plan) == moveit::core::MoveItErrorCode::SUCCESS);
//         ROS_INFO("plan (pose goal) %s", success ? "SUCCESS" : "FAILED");

//         // if failed try and plan based on 180 deg rotation about x-axis
//         if (!success)
//         {
//             quaternion *= tf2::Quaternion(axis, -M_PI);
//             target_pose.orientation.x = quaternion.x();
//             target_pose.orientation.y = quaternion.y();
//             target_pose.orientation.z = quaternion.z();
//             target_pose.orientation.w = quaternion.w();

//             visual_tools_.publishAxis(target_pose, rviz_visual_tools::LARGE);
//             visual_tools_.trigger();

//             success = (move_group_.plan(move_plan) == moveit::core::MoveItErrorCode::SUCCESS);
//             ROS_INFO("plan @ 180 (pose goal) %s", success ? "SUCCESS" : "FAILED");
//         }


//         move_group_.setPoseTarget(target_pose);

//         if (success)
//         {
//             move_group_.execute(move_plan);

//             ////TODO: now move along an axis towards the object
//             planning_interface_.removeCollisionObjects(planning_interface_.getKnownObjectNames());
//             shift_x_axis[0] = 0.13;
//             T_base_target = poseMsgToEigen(target_pose);
//             shifted_position = T_base_target * shift_x_axis;
//             target_pose.position.x = shifted_position.x();
//             target_pose.position.y = shifted_position.y();
//             target_pose.position.z = shifted_position.z();
//             move_group_.setPoseTarget(target_pose);
//             move_group_.move();
//         }
//         idx++;
//     }

//     if (success == false)
//     {
//         ROS_ERROR("No grasp pose was possible for this object.");
//     }
//     // planning_interface_.removeCollisionObjects(planning_interface_.getKnownObjectNames());

// }