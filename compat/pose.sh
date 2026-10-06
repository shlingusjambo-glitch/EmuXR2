#!/system/bin/sh
# Stationary desktop headset pose through Meta's supported injection service.
while ! service check TrackingDataInjection | grep -q found; do sleep 1; done
service call TrackingDataInjection 5 i32 1 >/dev/null
while true; do
    pose_qx=0 pose_qy=0 pose_qz=0 pose_qw=1
    if [ -r /data/local/tmp/macvr-head ]; then
        # Accept desktop quaternions and the older position + quaternion record.
        read pose_a pose_b pose_c pose_d pose_e pose_f pose_g < /data/local/tmp/macvr-head
        if [ -n "$pose_g" ]; then
            pose_qx=$pose_d pose_qy=$pose_e pose_qz=$pose_f pose_qw=$pose_g
        elif [ -n "$pose_d" ]; then
            pose_qx=$pose_a pose_qy=$pose_b pose_qz=$pose_c pose_qw=$pose_d
        fi
    fi
    service call TrackingDataInjection 2 i32 1 i32 4 f "$pose_qx" f "$pose_qy" f "$pose_qz" f "$pose_qw" >/dev/null
    service call TrackingDataInjection 2 i32 0 i32 3 f 0 f 0 f 0 >/dev/null   # raw y 0: the runtime's standing eye height
    sleep .05
done
