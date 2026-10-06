"""Self-check for stream.touch_controller (run with the streamer's python)."""
import time
import stream as st

idle = {'flags': 3, 'buttons': 0, 'trigger': 0.0, 'squeeze': 0.0, 'stick': (0.0, 0.0)}
menu = [0, 0]
st.CONTROLLERS = True
f, pressed, touched, *_ = st.touch_controller(dict(idle, trigger=0.8, buttons=1), False, menu)
assert f == 1 and pressed == st.TRIGGER | st.AX and touched & st.TRIGGER, (f, pressed, touched)
st.CONTROLLERS = True
assert st.touch_controller(dict(idle, flags=7), False, menu)[0] == 0          # bare hand: no controller
# Left menu maps directly to guest Oculus/Home, including a brief tap.
assert st.touch_controller(dict(idle, buttons=16), True, menu)[1] == st.HOME
assert st.touch_controller(idle, True, menu)[1] == 0
assert st.touch_controller(dict(idle, buttons=16), False, menu)[1] == 0
assert st.touch_controller(dict(idle, flags=0, trigger=1, buttons=16), True, menu) == (0, 0, 0, 0, 0, 0, 0)
st.CONTROLLERS = False
assert st.touch_controller(dict(idle, trigger=1), False, menu) == (0, 0, 0, 0, 0, 0, 0)
print('ok')

# imu_from_grip inverts the calibrated transform: IMU * GRIP_FROM_IMU gives back the grip pose
import math, quest_proto as qp
g = ((0.1, 1.2, -0.3), (0.0, math.sin(0.4), 0.0, math.cos(0.4)))
(ip, iq) = st.imu_from_grip(*g)
tp, tq = st.GRIP_FROM_IMU
back_q = qp._quat_mul(iq, tq)
back_p = tuple(ip[i] + qp._rot_vec(iq, tp)[i] for i in range(3))
assert max(abs(a - b) for a, b in zip(back_p, g[0])) < 1e-6 and max(abs(a - b) for a, b in zip(back_q, g[1])) < 1e-6
print('imu_from_grip ok')
