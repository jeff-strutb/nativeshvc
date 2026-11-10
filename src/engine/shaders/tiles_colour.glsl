/* tiles_colour.glsl: a tile layer's colour of a texel c (not 0) in a tile with palette bits
   pal, as tiles.frag and tiles_solid.frag both read it; u.i[3]: bits a texel, mode 0's
   palette group, direct colour */
vec3 colour(uint c, uint pal, int prow)
{
    int bpp = u.i[3].x;
    if (bpp == 8)
        return u.i[3].z != 0 ? direct(c, pal) : palette(c, prow);
    if (bpp == 4)
        return palette(pal * 16u + c, prow);
    return palette(uint(u.i[3].y) + pal * 4u + c, prow);
}
