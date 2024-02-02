#!/usr/bin/env python3

import rospy
import math
import smach
import numpy as np
from itertools import permutations
import copy

from typing import List
from tf.transformations import quaternion_from_euler
from ObjectInfo import ObjectInfo
from LocateObject import LocateObject
from geometry_msgs.msg import Point, Quaternion, PoseStamped, PointStamped, PoseArray
from nav_msgs.msg import GridCells

"""This state optimises the order in which objects are to be
picked up and dropped off to maximise efficiency.
"""
class Optimise(smach.State):
    ROBOT_RADIUS = 0.22
    ROBOT_RADIUS_BUFFER = 0.22
    DIST_FROM_OBJECT = ROBOT_RADIUS * 3
    def __init__(self):
        smach.State.__init__(self, outcomes=["succeeded"],
                             input_keys=["pickup_info", "nav_goal_index"],
                             output_keys=["pickup_info"])

        self._debug_pub = rospy.Publisher("/rviz_visual_tools", PoseArray, queue_size=10)

    def _generate_pose_around(self, 
                                centre: PointStamped, 
                                radius: float,
                                count: int = 10) -> List[PoseStamped]:
        """Generates a list of poses on the circumfrence of a 
        circle with the given dimensions. It makes sure that
        the poses x-axis is pointing towards the centre of the
        circle.
        """
        poses = []
        for i in range(count):
            angle = 2.0 * math.pi * i / count
            x = radius * math.cos(angle) + centre.point.x
            y = radius * math.sin(angle) + centre.point.y

            pose_stamped = PoseStamped()
            pose_stamped.header.stamp = rospy.Time.now()
            pose_stamped.header.frame_id = centre.header.frame_id

            pose_stamped.pose.position.x = x
            pose_stamped.pose.position.y = y
            pose_stamped.pose.position.z = 0.0

            # make sure the x-axis of the pose is always pointing 
            # towards the centre of the circle
            quaternion = quaternion_from_euler(0.0, 0.0, math.atan2(centre.point.y - y,
                                                                    centre.point.x - x)) 
            pose_stamped.pose.orientation = Quaternion(*quaternion)

            poses.append(pose_stamped)

        return poses

    
    def _cell_intersects(self, r: float, cx: float, cy: float, rw: float, rh: float) -> bool:
        """ Checks if there is an intersection between a rectangle and a circle.
        The position of the circle is relative to the rectangle. The rectangle
        is also assumed to be axis-aligned.
        Algo: https://stackoverflow.com/questions/401847/circle-rectangle-collision-detection-intersection 
        """
        if cx > (rw / 2 + r): return False
        if cy > (rh / 2 + r): return False

        if cx <= rw / 2: return True
        if cy <= rh / 2: return True

        corner_dist_squared = (cx - rw / 2)**2 + (cy - rh / 2)**2

        return corner_dist_squared <= r**2

    def _object_intersects(self, r: float, ox: float, oy: float) -> bool:
        """Checks if there is an intersection between an
        object and a circle. The position of the object is 
        relative to the circle.
        """
        object_distance_squared = ox**2 + oy**2
        return object_distance_squared <= r**2

    def _collision(self, 
                    pose: PoseStamped, 
                    obstacle_map: GridCells, 
                    other_objects: List[ObjectInfo]) -> bool:
        """Checks in a circular region centred at the given pose
        for collision. The circular region is greater than the radius
        of the robot.
        """
        # check for collisions with the obstacle map itself
        for cell in obstacle_map.cells:
            if self._cell_intersects(r=Optimise.ROBOT_RADIUS + Optimise.ROBOT_RADIUS_BUFFER,
                                cx=abs(pose.pose.position.x - cell.x),
                                cy=abs(pose.pose.position.y - cell.y),
                                rw=obstacle_map.cell_width,
                                rh=obstacle_map.cell_height):
                return True 

        # check for collisions with other pickup objects which are not
        # represented into the static obstacle map
        for object_info in other_objects:
            object_position = object_info.get_position().point
            if self._object_intersects(r=Optimise.ROBOT_RADIUS + Optimise.ROBOT_RADIUS_BUFFER,
                                       ox=abs(object_position.x - pose.pose.position.x),
                                       oy=abs(object_position.y - pose.pose.position.y)):
                return True 

        return False 

    def get_obstacle_map(self, wait=2.0) -> GridCells:
        """Returns the static obstacle map as generated by
        the map.
        """
        obstacle_map = None
        try:
            obstacle_map = rospy.wait_for_message("/base_path_planner/inflated_static_obstacle_map",
                                                  GridCells,
                                                  timeout=wait)
            rospy.loginfo(f"Obtained obstacle map.")
        except rospy.ROSException as e:
            rospy.logwarn(f"Timeout reached. No message received within {wait} seconds. \
                Error: {e}")

        return obstacle_map

    def _add_pickup_poses(self, ud) -> None:
        """Adds valid pickup locations for each of the 
        detected objects. A valid pickup location is defined
        by one where no collisions occur when the robot is at
        the given pickup location.
        """
        pose_array_msg = PoseArray()
        pose_array_msg.header.frame_id = "map"
        pose_array_msg.header.stamp = rospy.Time.now()
        obstacle_map = self.get_obstacle_map()
        for i, object in enumerate(ud.pickup_info):
            pose_around = self._generate_pose_around(centre=object.get_position(),
                                                     radius=Optimise.DIST_FROM_OBJECT)
            for pose in pose_around:
                # check if there are any collisions between the robot
                # at pose and environment
                if not self._collision(pose, obstacle_map, ud.pickup_info[i + 1:]):
                    object.add_pickup_location(pose)
                    pose_array_msg.poses.append(pose.pose)

        self._debug_pub.publish(pose_array_msg)

    def execute(self, ud):
        # ud.target_object.add_pickup_location(ud.target_object.get_position())
        rospy.loginfo("Start of Optimisation")
                ## 3 instances of class object_info
        number_objects_detected = LocateObject.REQUIRED_OBJECT_COUNT
        objects_info = []

        for n in range(0,number_objects_detected):
            objects_info.append(copy.deepcopy(ud.pickup_info[n]))
            
        all_seq_objects = list(permutations(objects_info))

       
        ## start position -> Robot in position search_one or search_two
        if ud.nav_goal_index == 1:
             waypoint_two = rospy.get_param("/way_points/search_two")
        #      start = (waypoint_one['search_one']['x'] , waypoint_one['search_one']['y'] )
             start = (waypoint_two['x'] , waypoint_two['y'] )
             
        elif ud.nav_goal_index == 2:
             waypoint_one = rospy.get_param("/way_points/search_one")
        #      start = (waypoint_two['search_two']['x'] , waypoint_two['search_one']['y'] )
             start = (waypoint_one['x'] , waypoint_one['y'] )

        totall_distance = 0
        list_totall_distance = []
        for m,sequence in enumerate(all_seq_objects):
            previous_object_info = 0
            for i,object_info in enumerate(sequence):
                pose_object= object_info.get_position()
                drop_pose = object_info.get_dropoff_point()
                class_object = object_info.get_class()
                # print("Round ", m, "the label is ", class_object)
                distance_to_drop = 0
                # print(i)
                # print("Round", m)
                # print("Pick up the object", class_object)
                # print(totall_distance)
                if i == 0:          # Distance from Start position (search one or two) till first object to pick up 
                    start_distance = np.linalg.norm(np.array([pose_object.point.x, pose_object.point.y]) - start)
                    # drop_pose = rospy.get_param(f"/way_points/drop_{object_info.get_class()}") 
                    distance_to_drop = np.linalg.norm(np.array([drop_pose.pose.position.x , drop_pose.pose.position.y])- np.array([pose_object.point.x, pose_object.point.y]))
                    totall_distance += distance_to_drop + start_distance
                    # print("First round",class_object)
                else:
                                    # need the drop_pose from the previous round -> previous drop_pose is starting point to pick up next objects
                    # print("previous class" , previous_object_info.get_class())
                    # print("actual class", class_object)
                    previous_drop_pose = previous_object_info.get_dropoff_point()
                    distance_back_object = np.linalg.norm(np.array([pose_object.point.x, pose_object.point.y]) -np.array([previous_drop_pose.pose.position.x , previous_drop_pose.pose.position.y]) )     # distance to pick next object (left) from previous_drop_pose
                    distance_to_drop = np.linalg.norm(np.array([drop_pose.pose.position.x , drop_pose.pose.position.y])- np.array([pose_object.point.x, pose_object.point.y]))
                    totall_distance += distance_back_object + distance_to_drop
                previous_object_info = object_info
            print(totall_distance)
            list_totall_distance.insert(m, totall_distance)
            totall_distance = 0

        index_shorest_path = list_totall_distance.index(min(list_totall_distance))

        for z,object_info in enumerate(all_seq_objects[index_shorest_path]):
            ud.pickup_info[z] = object_info    




 


        self._add_pickup_poses(ud)

        return "succeeded"
