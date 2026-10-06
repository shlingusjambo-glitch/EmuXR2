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
