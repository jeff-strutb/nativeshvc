/*
 * input.c: the native controls onto the console's controllers (the front
 * end's src/front/input.c), checked through the console's own ports: a
 * cartridge of a few lines of C strobes $4016 and reads each port's bits
 * back after the controls are given, and the device each player is using
 * and the names of its controls are checked as a game would ask for them.
 *
 *   input             runs the checks; exit 0 when every one holds
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"
#include "host.h"
#include "input.h"

static int fails;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}

/* port `port`'s first n bits after a strobe, data line `line`, the first
   bit read in bit n - 1 */
static uint32_t port_bits(int port, int n, int line)
{
    uint32_t v = 0;
    int i;
    wr8(0x004016, 1);
    wr8(0x004016, 0);
    for (i = 0; i < n; i++)
        v = v << 1 | (uint32_t)(rd8(port ? 0x004017 : 0x004016) >> line & 1);
    return v;
}

static const uint16_t none[8];
static int heard[8], heard_kind[8];

static void hook(int player, const nshvc_input_source *src)
{
    heard[player]++;
    heard_kind[player] = src->kind;
}

static void abi_reset(void)
{
    host_pad pads[8];
    int on[8] = { 0 }, dot, line;
    nshvc_input_source s;
    uint32_t v;
    static const int at[4] = { 100, 50, 900, 650 };

    memset(pads, 0, sizeof pads);
    wr8(0x004200, 0x00);

    /* a joypad in each port: the keyboard is player 0 */
    input_key('Z', 1);
    input_key(0x26, 1);
    input_frame_pads(pads, on, none);
    v = port_bits(0, 16, 0);
    check(v == (NSHVC_B | NSHVC_UP), "keyboard Z and Up: player 0's joypad reads B and Up");
    check(nshvc_player_source(0, &s) && s.kind == NSHVC_INPUT_KEYBOARD, "player 0 is on the keyboard");
    check(!strcmp(nshvc_player_label(0, NSHVC_B), "Z"), "player 0's B is named Z");
    input_report(hook);
    check(heard[0] == 1 && heard_kind[0] == NSHVC_INPUT_KEYBOARD, "the game hears player 0 is on the keyboard");
    input_key('Z', 0);
    input_key(0x26, 0);

    /* a PlayStation controller as player 1, with no names of its own */
    on[1] = 1;
    pads[1].style = HOST_PAD_PLAYSTATION;
    snprintf(pads[1].name, sizeof pads[1].name, "DualSense");
    pads[1].buttons = 1u << 0 | 1u << 3 | 1u << 15;        /* bottom, top, d-pad left */
    input_frame_pads(pads, on, none);
    v = port_bits(1, 16, 0);
    check(v == (NSHVC_B | NSHVC_X | NSHVC_LEFT), "controller 1's bottom, top, left: player 1 reads B, X, Left");
    check(nshvc_player_source(1, &s) && s.kind == NSHVC_INPUT_GAMEPAD && s.style == NSHVC_PAD_PLAYSTATION &&
              !strcmp(s.name, "DualSense"),
          "player 1 is on a PlayStation controller, DualSense");
    check(!strcmp(nshvc_player_label(1, NSHVC_B), "Cross") && !strcmp(nshvc_player_label(1, NSHVC_X), "Triangle"),
          "player 1's B is Cross, X Triangle");
    /* the controller's own names come first */
    snprintf(pads[1].label[0], sizeof pads[1].label[0], "Button 1");
    pads[1].buttons = 0;
    input_frame_pads(pads, on, none);
    check(!strcmp(nshvc_player_label(1, NSHVC_B), "Button 1"), "a controller's own name for a button is used");
    /* the stick as the cross; opposite directions never both */
    pads[1].x = -0.9f;
    pads[1].buttons = 1u << 13;                             /* d-pad right, stick left */
    input_frame_pads(pads, on, none);
    v = port_bits(1, 16, 0);
    check((v & (NSHVC_LEFT | NSHVC_RIGHT)) == 0, "Left and Right held at once: the joypad reads neither");
    pads[1].x = 0;
    pads[1].buttons = 0;

    /* the game's own buttons join a player's */
    {
        uint16_t gb[8] = { 0 };
        gb[1] = NSHVC_START;
        input_frame_pads(pads, on, gb);
        check(port_bits(1, 16, 0) == NSHVC_START, "the game's buttons for player 1 read on port 1");
    }

    /* a mouse in port 0: the frame's motion and buttons */
    nshvc_plug(0, NSHVC_PORT_MOUSE);
    input_mouse(10, 10, 5, -3, 1);
    input_frame_pads(pads, on, none);
    v = port_bits(0, 32, 0);
    check((v >> 24) == 0 && (v >> 16 & 0xFF) == 0x41, "mouse: left button, speed 0, signature 0001");
    check((v >> 8 & 0xFF) == 0x83 && (v & 0xFF) == 0x05, "mouse: 3 up, 5 right");
    check(nshvc_player_source(0, &s) && s.kind == NSHVC_INPUT_MOUSE, "player 0 is on the mouse");
    check(!strcmp(nshvc_player_label(0, 1), "Left button"), "the mouse's button 1 is the left button");
    input_frame_pads(pads, on, none);
    v = port_bits(0, 32, 0);
    check((v & 0xFFFF) == 0, "mouse: no motion the next frame");

    /* a multitap in port 1: players 1-4, two at once, IOBit picking the pair */
    nshvc_plug(0, NSHVC_PORT_JOYPAD);
    nshvc_plug(1, NSHVC_PORT_MULTITAP);
    on[2] = on[3] = on[4] = 1;
    pads[1].buttons = 1u << 0;                              /* player 1: B */
    pads[2].buttons = 1u << 1;                              /* player 2: A */
    pads[3].buttons = 1u << 4;                              /* player 3: L */
    pads[4].buttons = 1u << 7;                              /* player 4: START */
    input_frame_pads(pads, on, none);
    wr8(0x004201, 0xFF);
    check(port_bits(1, 16, 0) == NSHVC_B && port_bits(1, 16, 1) == NSHVC_A,
          "multitap, IOBit high: players 1 and 2 on lines 1 and 2");
    wr8(0x004201, 0x7F);
    check(port_bits(1, 16, 0) == NSHVC_L && port_bits(1, 16, 1) == NSHVC_START,
          "multitap, IOBit low: players 3 and 4");
    wr8(0x004201, 0xFF);
    check(nshvc_player_source(4, &s) && s.kind == NSHVC_INPUT_GAMEPAD, "player 4 is on controller 4");
    check(!nshvc_player_source(5, &s), "no player 5 with a joypad and a multitap");

    /* where the pointer is on the picture: drawn at (100, 50)-(900, 650)
       on a target 2 pixels a point */
    check(input_point(at, 2, 256, 224, 0, 0, 50, 25, &dot, &line) && dot == 0 && line == 0,
          "the picture's top left corner is dot 0, line 0");
    check(input_point(at, 2, 256, 224, 0, 0, 449.9f, 324.9f, &dot, &line) && dot == 255 && line == 223,
          "its bottom right corner is dot 255, line 223");
    check(input_point(at, 2, 512, 448, 128, 112, 250, 175, &dot, &line) && dot == 128 && line == 112,
          "the console's picture centred on a 512 x 448 screen: the middle is dot 128, line 112");
    check(input_point(at, 2, 512, 448, 0, 0, 50, 25, &dot, &line) && dot == 0 && line == 0,
          "the console's picture at a 512 x 448 screen's top left: its corner is dot 0, line 0");
    check(input_point(at, 2, 512, 448, 256, 224, 250, 175, &dot, &line) && dot == 0 && line == 0,
          "the console's picture at its bottom right: the middle is dot 0, line 0");
    check(!input_point(at, 2, 256, 224, 0, 0, 40, 25, &dot, &line), "left of the picture: off the screen");

    printf("%d check(s) failed\n", fails);
    exit(fails ? 1 : 0);
}

static const uint8_t rom[0x8000] = { [0x7FFC] = 0x00, [0x7FFD] = 0x80, [0x7FD5] = 0x20 };
static const nshvc_routine routines[] = { { 0x008000, abi_reset, "reset", 0 } };

int main(void)
{
    nshvc_cart cart;
    memset(&cart, 0, sizeof cart);
    cart.title = "input";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 1;
    nshvc_power(&cart);
    for (;;)
        nshvc_run_frame();
}
