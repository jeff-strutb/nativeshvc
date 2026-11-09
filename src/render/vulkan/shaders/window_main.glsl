layout(location = 0) out vec4 o;
void main() {
    uint c = pb_window(uint(gl_FragCoord.x), uint(gl_FragCoord.y));
    o = vec4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), 255.0) / 255.0;
}
