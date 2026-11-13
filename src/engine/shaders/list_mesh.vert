#version 450
#extension GL_GOOGLE_include_directive : require
/* list_mesh: a draw list's instanced mesh (nshvc_draw_mesh): an instance for each place, the mesh's triangles once in
   each, projected here, its fragment stage list.frag's (and a game shader's), whose inputs match these outputs.
   ti1: the mesh's vertices, six words each (x, y, z, u, v, 0xAARRGGBB) from the first word of a texture 1024 words wide
   i[2].x: the first word of the view's twenty (eye 0-2, rot 3-11, f, cx, cy, near 12-15, depth_a, depth_b, depth_max
           16-18), the instances' four each (x, y, z, size) after them
   f[2]: the rectangle it is kept to (the draw's scissor); f[3].x: alpha written
   f[4], f[5]: the material's eight inputs
   f[6]: screen pixels a list pixel across and down, the depth of z 0 and of z 1
   i[0].yzw, i[1].x: the image, as list.vert's

   The projection is worked in homogeneous coordinates, so that nothing in it divides by Z but the screen position
   passed on (v_p) and a vertex behind the eye or on its plane is as good a vertex as any, for the clipper to cut:
     Z = view.z; clip w = Z; clip xy = (screen px * 2 / size - 1) * Z; clip z = (z0 + (z1 - z0) * (depth_a - depth_b / Z)) * Z
   and the last is z0 * Z + (z1 - z0) * (depth_a * Z - depth_b), linear in the view's coordinates. The rasteriser divides by
   w and interpolates z in screen space, which for z = depth_a - depth_b / Z is exact (1 / Z is affine in the screen),
   as nshvc_draw_tris' vertices carrying that z have it. Cut by three clip distances, all linear in the same
   coordinates: Z - near, the list's z (depth_a - depth_b / Z) not below 0, and not above depth_max: whichever way the
   list's order runs (z rising with Z for depth_b above 0, falling for depth_b below 0) */
#include "common.glsl"
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_colour;
layout(location = 2) noperspective out vec2 v_p;
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[3]; };

uint mesh_word(int k) { return texelFetch(ti1, ivec2(k & 1023, k >> 10), 0).r; }
float mesh_float(int k) { return uintBitsToFloat(mesh_word(k)); }

void main()
{
    int vb = u.i[2].x, k = gl_VertexIndex * 6, ib = vb + 20 + gl_InstanceIndex * 4;
    vec3 model = vec3(mesh_float(k), mesh_float(k + 1), mesh_float(k + 2));
    uint c = mesh_word(k + 5);
    vec4 inst = vec4(wordf(ib), wordf(ib + 1), wordf(ib + 2), wordf(ib + 3));
    vec3 d = model * inst.w + inst.xyz - vec3(wordf(vb), wordf(vb + 1), wordf(vb + 2));
    vec3 view = vec3(dot(vec3(wordf(vb + 3), wordf(vb + 4), wordf(vb + 5)), d),
                     dot(vec3(wordf(vb + 6), wordf(vb + 7), wordf(vb + 8)), d),
                     dot(vec3(wordf(vb + 9), wordf(vb + 10), wordf(vb + 11)), d));
    float f = wordf(vb + 12), near = wordf(vb + 15), da = wordf(vb + 16), db = wordf(vb + 17), dmax = wordf(vb + 18);
    vec2 centre = vec2(wordf(vb + 13), wordf(vb + 14));
    float Z = view.z;
    /* the screen position times Z, in the picture's pixels, and the position itself for the fragment stage (finite
       for any Z but 0: the clipper's interpolation of a noperspective value takes each end's w into account) */
    vec2 pz = (centre * Z + f * view.xy) * u.f[6].xy;
    vec2 p = abs(Z) > 1e-6 ? pz / Z : vec2(0.0);
    float dz = da * Z - db;                          /* the list's z times Z */
    /* clip z = (z0 + (z1 - z0) * (depth_a - depth_b / Z)) * Z, written as K * Z - (z1 - z0) * depth_b with K = z0 +
       (z1 - z0) * depth_a. With a depth of its own z0 = 0 and z1 = 1 (the z given, as it is), and K = depth_a exactly: clip
       z = depth_a * Z - depth_b, nothing more; with depth_a = 0 and depth_b = -near (z = near / Z, the order NEAR_1) it is
       near itself, and the stored depth near / Z has float's steps at their finest where Z is large. Without one, the
       picture's slice (z0 to z1, swapped for NEAR_1) */
    float z01 = u.f[6].w - u.f[6].z, zc = (u.f[6].z + z01 * da) * Z - z01 * db;
    vec4 g = oriented(pz * (2.0 / u.f[1].xy) - vec2(Z), zc);
    g.w = Z;
    gl_Position = g;
    gl_ClipDistance[0] = Z - near;
    gl_ClipDistance[1] = dz;
    gl_ClipDistance[2] = (dmax - da) * Z + db;
    v_uv = vec2(mesh_float(k + 3), mesh_float(k + 4));
    v_p = p;
    v_colour = vec4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), float(c >> 24)) / 255.0;
}
