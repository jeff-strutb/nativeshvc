#version 450
#extension GL_GOOGLE_include_directive : require
/* combine: the picture's pixels from the screens already drawn, the colour
   maths done on them exactly as the console's capability defines them (each
   channel in 5-bit steps, or eighths of one for colours past 15 bits: added
   or taken away, halved, kept to the range), then brightness and blanking.
   An instance a rectangle of five words: x0, y0, x1, y1 (floats, screen
   pixels), 1 if inside the colour window.
   i[0].x: the rectangles' first word
   i[1]: subtract, half, the fixed colour in place of the sub screen, any layer taking part
   i[2]: the clip region (0 never, 1 outside, 2 inside, 3 always), the prevent region, brightness, blank
   i[3]: hires, the fixed colour (15-bit BGR)
   tc0: the main screen (alpha 1 where its layer takes part); tc1: the sub
   screen; tc2: the main screen alone, for a hires row's even half-columns
   (alpha 1 where a layer is, 0 its backdrop) */
#include "common.glsl"
layout(location = 0) out vec2 v_p;
layout(location = 1) flat out int v_in;
void main()
{
    int k = u.i[0].x + gl_InstanceIndex * 5;
    vec2 p0 = vec2(wordf(k), wordf(k + 1)), p1 = vec2(wordf(k + 2), wordf(k + 3));
    v_p = mix(p0, p1, corner(gl_VertexIndex));
    v_in = int(word(k + 4));
    gl_Position = clip_at(v_p, 0.0);
}
