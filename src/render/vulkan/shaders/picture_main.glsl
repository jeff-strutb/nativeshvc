layout(location = 0) out uint o;
void main() { if (pb_row_kept(uint(gl_FragCoord.y)) != 0u) discard; o = pb_pixel(uint(gl_FragCoord.x), uint(gl_FragCoord.y)); }
