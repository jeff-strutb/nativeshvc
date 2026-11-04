/* host_psp.h: the PSP host's screen, for a renderer that draws on it
   itself (render/psp): host_window_handle's, once the screen is open. */
#ifndef PORT_HOST_PSP_H
#define PORT_HOST_PSP_H

#define HOST_PSP_STRIDE 512                  /* pixels a VRAM row (0xAABBGGRR each) */
#define HOST_PSP_VRAM_USED (3 * HOST_PSP_STRIDE * 272 * 4)   /* the screen's three buffers, from VRAM's start */

typedef struct HostPspScreen {
    /* the VRAM buffer the next frame is drawn into (its address on the
       GE's bus): neither the one on screen nor the one waiting to be */
    void *(*target)(void);
    /* that buffer shown from the next vertical blank, without waiting for it */
    void (*show)(void);
    /* the buffer last shown */
    void *(*shown)(void);
    /* the GE's state as the renderer left it, where the game drew on it
       itself (host_ge_vram: put back at the renderer's next frame) */
    void (*ge_back)(void);
} HostPspScreen;

#endif
