/* packet.h: what crosses from the game thread to the render thread for one
 * picture: the changes to the GPU's store since the last picture, and the
 * picture's description (render/picture_body.h). */
#ifndef ENGINE_PACKET_H
#define ENGINE_PACKET_H

#include <stddef.h>
#include <stdint.h>

/* a draw list as it crosses: vertices then commands in the packet's list data */
#define ENG_DRAW_WORDS 5        /* x, y, u, v as float bits, colour */
#define ENG_CMD_WORDS 15        /* first vertex, count, 0 tris / 1 lines, image store base, w, h, shader, 8 inputs */
typedef struct {
    uint32_t w, h;
    uint32_t samples;           /* asked for: 1, 2, 4, 8 or 16 */
    size_t verts, nverts, cmds, ncmds;  /* word offsets and counts in list_data */
} eng_list;

/* a shader made since the last picture: the backend's own compiled form */
#define ENG_SHADER_LOADS 16
typedef struct { uint32_t id; const void *code; size_t size; } eng_shader_load;

typedef struct {
    uint32_t *desc;
    size_t desc_words, desc_cap;
    uint32_t *ops;              /* each: at, words, then that many words */
    size_t ops_words, ops_cap;
    uint32_t *list_data;
    size_t list_words, list_cap;
    eng_list lists[4];          /* by layer; w 0: none */
    eng_shader_load shaders[ENG_SHADER_LOADS];
    uint32_t nshaders;
    size_t store_words;         /* the store's size for this picture */
    int width, height;          /* the picture's */
    int keep;                   /* an interlaced picture keeps the other field's rows */
    uint64_t update;
    float between;
} eng_packet;

/* the game thread: the packet being filled */
void eng_picture_begin(void);   /* draw is about to run */
eng_packet *eng_picture_end(void);   /* draw has returned: the finished packet */
void eng_picture_init(void);

#endif
