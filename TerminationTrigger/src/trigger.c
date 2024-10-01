#include "cctk.h"
#include "cctk_Parameters.h"
#include "cctk_Arguments.h"

#ifdef HAVE_CAPABILITY_MPI
#include "mpi.h"
#endif

void TerminationTrigger_TriggerTermination(CCTK_ARGUMENTS)
{
  DECLARE_CCTK_PARAMETERS;
  DECLARE_CCTK_ARGUMENTS_CHECKED(TerminationTrigger_TriggerTermination);

# ifdef HAVE_CAPABILITY_MPI
  if (CCTK_IsThornActive("MPI")) {
    int send_buf = *triggered;
    int recv_buf;
    MPI_Reduce(&send_buf, &recv_buf, 1, MPI_INT, MPI_MAX, 0, MPI_COMM_WORLD);
    *triggered = recv_buf;
  }
# endif
  if (*triggered) {
    CCTK_TerminateNext(cctkGH);
  }
}

void TerminationTrigger_ResetTrigger(CCTK_ARGUMENTS)
{
  DECLARE_CCTK_ARGUMENTS_CHECKED(TerminationTrigger_ResetTrigger);

  *triggered = 0;
}
