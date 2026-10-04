#pragma once

// IDI_APPICON is the lowest id on purpose: Explorer shows the first icon group as
// the exe's own icon. It is the three-display picture; the tray swaps between the
// IDI_DISPLAYS_* icons to match how many displays are connected.
#define IDI_APPICON    101
#define IDI_DISPLAYS_1 102
#define IDI_DISPLAYS_2 103
#define IDI_DISPLAYS_3 104
#define IDI_DISPLAYS_4 105

#define IDM_LEFT    40001
#define IDM_LOG     40002
#define IDM_ABOUT   40003
#define IDM_EXIT    40004
#define IDM_RIGHT   40005
#define IDM_STARTUP 40006
