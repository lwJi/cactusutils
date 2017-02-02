#include <cctk.h>
#include <cctk_Arguments.h>

extern "C" void ST_OutputAlignment(CCTK_ARGUMENTS) {
  DECLARE_CCTK_ARGUMENTS;
  CCTK_VINFO("Grid function cache alignment: gsh=[%d,%d,%d] lsh=[%d,%d,%d] "
             "ash=[%d,%d,%d] alignment=%d offset=%d",
             cctk_gsh[0], cctk_gsh[1], cctk_gsh[2], cctk_lsh[0], cctk_lsh[1],
             cctk_lsh[2], cctk_ash[0], cctk_ash[1], cctk_ash[2], cctk_alignment,
             cctk_alignment_offset);
}
