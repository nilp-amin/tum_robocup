#!/usr/bin/env python3

import math
import rospy
import smach
import hsrb_interface
import ObjectInfo

from visualization_msgs.msg import MarkerArray

"""This state causes the robot to move to predefined locations on
the map and search for objects in that region. It makes sure the
exact same object is not recorded multiple times.
"""
class LocateObject(smach.State):
    REQUIRED_OBJECT_COUNT = 3
    def __init__(self):
        smach.State.__init__(self, 
                             outcomes=["succeeded", "failed", "aborted"],
                             input_keys=["pickup_info"],
                             output_keys=["pickup_info"])

        # the number of sectors checked for objects
        self._sector_count = 0

    def _rotate_base(self, robot, deg=90) -> None:
        """Rotate the robot by an amount relative to
        its current pose.
        """
        base = robot.get("omni_base")
        base.go_rel(x=0.0, y=0.0, yaw=math.radians(deg)) 

    def _rotate_head(self, robot, pan_deg=0, tilt_deg=0):
        """Rotate the robot head about pan and tilt joints
        to the specified joint angles.
        """
        whole_body = robot.get("whole_body")
        whole_body.move_to_joint_positions(
            head_pan_joint=math.radians(pan_deg),
            head_tilt_joint=math.radians(tilt_deg)
        )

    def _is_unqiue_object(self, ud, new_object_info: ObjectInfo) -> bool:
        """Checks if the passed marker belongs to an object
        which has been already identified.
        """
        for object_info in ud.pickup_info:
            if new_object_info == object_info:
                return False 

        return True

    def look_new_sector(self, robot, move_base=True) -> None:
        """Rotate the robot and tilt its head to 
        the next sector relative to its current pose.
        """
        if move_base: 
            self._rotate_base(robot, 90)
        self._rotate_head(robot, pan_deg=0, tilt_deg=-45)

    def get_object_markers(self, wait=2.0) -> MarkerArray:
        """Returns the text markers of classified objects
        if present. Otherwise returns None
        """
        markers_msg = None
        try:
            markers_msg = rospy.wait_for_message("/text_markers",
                                             MarkerArray,
                                             timeout=wait)
            rospy.loginfo(f"Found {len(markers_msg.markers)} objects in vision.")
        except rospy.ROSException as e:
            rospy.logwarn(f"Timeout reached. No message received within {wait} seconds. \
                Error: {e}")

        return markers_msg

    def execute(self, ud):
        rospy.loginfo("Executing state LocateObject")

        status = "aborted"
        with hsrb_interface.Robot() as robot:
            self.look_new_sector(robot, move_base=False)

            # keep looping until all sectors checked or min num of unique objects found
            while True:
                # check if objects are detected
                marker_array_msg = self.get_object_markers()
                if marker_array_msg is not None:
                    # filter exactly the same objects as specified by their
                    # centroid locations 
                    for marker in marker_array_msg.markers:
                        if len(ud.pickup_info) < LocateObject.REQUIRED_OBJECT_COUNT:
                            object_info = ObjectInfo(marker) 
                            if self._is_unqiue_object(ud, object_info):
                                ud.pickup_info.append(object_info)
                        else:
                            break

                if self._sector_count == 3 and len(ud.pickup_info) < LocateObject.REQUIRED_OBJECT_COUNT:
                    # checked all sectors and < min num of unqiue objects found
                    status = "failed"
                    break
                elif len(ud.pickup_info) == LocateObject.REQUIRED_OBJECT_COUNT:
                    status = "succeeded"
                    break
            
                self.look_new_sector(robot)
                self._sector_count += 1

        return status
