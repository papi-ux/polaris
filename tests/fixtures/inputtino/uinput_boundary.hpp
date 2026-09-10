#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

int inputtino_test_open(const char *, int, ...);
int inputtino_test_ioctl(int, unsigned long, ...);
int inputtino_test_close(int);
int inputtino_test_create(const libevdev *, int, libevdev_uinput **);
void inputtino_test_destroy(libevdev_uinput *);
// All metadata setup uses the real libevdev implementation. Only the device
// creation boundary is replaced, before the actual dependency code is parsed.
#define open inputtino_test_open
#define ioctl inputtino_test_ioctl
#define close inputtino_test_close
#define libevdev_uinput_create_from_device inputtino_test_create
#define libevdev_uinput_destroy inputtino_test_destroy
