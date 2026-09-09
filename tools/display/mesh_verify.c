// mesh_verify.c -- exercise the REAL generated quest1_mesh_sample() (not a reimplementation of it)
// against a dense (u,v) grid, so verify_mesh_conversion.py can check the shipped C artifact rather
// than trusting that a Python model of it behaves the same way. research-notes/42/63.
//
// Usage: mesh_verify <header.h is #included at build time via -I, not passed as an argv> <n>
//   n = samples per axis (e.g. 129 gives exact grid vertices at every 4th sample since the mesh is
//   33x33, plus midpoints and finer subdivisions for interpolation-sensitivity checks).
// Output: CSV to stdout, view,u,v,ru,rv,gu,gv,bu,bv

#include <stdio.h>
#include <stdlib.h>
#include "quest1_distortion_mesh.h"

int main(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 129;
  printf("#view,u,v,ru,rv,gu,gv,bu,bv\n");
  for (int view = 0; view < 2; view++) {
    for (int i = 0; i < n; i++) {
      float v = (float)i / (n - 1);
      for (int j = 0; j < n; j++) {
        float u = (float)j / (n - 1);
        float r[2], g[2], b[2];
        if (!quest1_mesh_sample((uint32_t)view, u, v, r, g, b)) continue;
        printf("%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
               view, u, v, r[0], r[1], g[0], g[1], b[0], b[1]);
      }
    }
  }
  return 0;
}
