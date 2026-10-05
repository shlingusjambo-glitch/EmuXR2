#!/system/bin/sh
# Stationary desktop headset pose through Meta's supported injection service.
while ! service check TrackingDataInjection | grep -q found; do sleep 1; done
service call TrackingDataInjection 5 i32 1 >/dev/null
while true; do
    service call TrackingDataInjection 2 i32 1 i32 4 f 0 f 0 f 0 f 1 >/dev/null
    service call TrackingDataInjection 2 i32 0 i32 3 f 0 f 1.6 f 0 >/dev/null
    sleep .05
done
