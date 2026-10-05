// A real (uinput) mouse inside the guest, so Horizon sees an attached pointing device like a Bluetooth mouse.
// Reads commands on stdin: "m <dx> <dy>" move, "d"/"u" left button down/up, "c" click, "r" right click, "w <n>" wheel.
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int fd;
static void ev(int type, int code, int value) { struct input_event e = {0}; e.type = type; e.code = code; e.value = value; write(fd, &e, sizeof e); }
static void syn(void) { ev(EV_SYN, SYN_REPORT, 0); }
int main(void) {
    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("uinput"); return 1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY); ioctl(fd, UI_SET_KEYBIT, BTN_LEFT); ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT); ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_EVBIT, EV_REL); ioctl(fd, UI_SET_RELBIT, REL_X); ioctl(fd, UI_SET_RELBIT, REL_Y); ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    struct uinput_setup us = {0}; us.id.bustype = BUS_BLUETOOTH; us.id.vendor = 0x046d; us.id.product = 0xb016; strcpy(us.name, "EmuXR2 Mouse");
    ioctl(fd, UI_DEV_SETUP, &us); ioctl(fd, UI_DEV_CREATE);
    setvbuf(stdout, NULL, _IONBF, 0); printf("ready\n");
    char line[64];
    while (fgets(line, sizeof line, stdin)) {
        int a = 0, b = 0;
        if (sscanf(line, "m %d %d", &a, &b) == 2) { ev(EV_REL, REL_X, a); ev(EV_REL, REL_Y, b); syn(); }
        else if (line[0] == 'd') { ev(EV_KEY, BTN_LEFT, 1); syn(); }
        else if (line[0] == 'u') { ev(EV_KEY, BTN_LEFT, 0); syn(); }
        else if (line[0] == 'c') { ev(EV_KEY, BTN_LEFT, 1); syn(); usleep(60000); ev(EV_KEY, BTN_LEFT, 0); syn(); }
        else if (line[0] == 'r') { ev(EV_KEY, BTN_RIGHT, 1); syn(); usleep(60000); ev(EV_KEY, BTN_RIGHT, 0); syn(); }
        else if (sscanf(line, "w %d", &a) == 1) { ev(EV_REL, REL_WHEEL, a); syn(); }
    }
    ioctl(fd, UI_DEV_DESTROY); close(fd);
}
