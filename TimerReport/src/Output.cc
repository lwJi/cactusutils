/*@@
  @file      Output.c
  @date      July 6 2003
  @author    Gabrielle Allen
  @desc
             Functions to report the timers
  @enddesc
@@*/

#include <cctk.h>
#include <cctk_Arguments.h>
#include <cctk_Parameters.h>
#include <cctk_Schedule.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>
using namespace std;

#ifdef CCTK_MPI
#include <mpi.h>
#endif

namespace TimerReport {

/********************************************************************
 *********************     Local Data Types   ***********************
 ********************************************************************/

struct timer_stats {
  vector<string> names;       // timer names
  vector<CCTK_REAL> secs_avg; // global average
  vector<CCTK_REAL> secs_min; // global minimum
  vector<CCTK_REAL> secs_max; // global maximum
  int ntimers() const { return names.size(); }
};

/********************************************************************
 ********************* Local Routine Prototypes *********************
 ********************************************************************/

void Output(CCTK_ARGUMENTS);
void PrintTimes(CCTK_ARGUMENTS);
void OutputAllTimers(CCTK_ARGUMENTS);
void OutputAllTimersTogether(CCTK_ARGUMENTS);
void OutputAllTimersPerIteration(CCTK_ARGUMENTS);
void OutputAllTimersReadable(CCTK_ARGUMENTS);
void PrintTopTimers(CCTK_ARGUMENTS);

int CollectTimerInfo(const cGH *restrict cctkGH, timer_stats &timers);

string QuoteForCSV(const string &);
string QuoteForTSV(const string &);

/********************************************************************
 *********************  Scheduled Routine Prototypes  ***************
 ********************************************************************/

extern "C" void TimerReport_OutputEvery(CCTK_ARGUMENTS);
extern "C" void TimerReport_OutputTerminate(CCTK_ARGUMENTS);
extern "C" void TimerReport_Checkpoint(CCTK_ARGUMENTS);

/********************************************************************
 ********************* Other Routine Prototypes *********************
 ********************************************************************/

/********************************************************************
 *********************     Local Data   *****************************
 ********************************************************************/

const string sep(70, '=');

/********************************************************************
 ********************    External Routines   ************************
 ********************************************************************/

/*@@
  @routine    TimerReport_OutputEvery
  @date       2008-11-12
  @author     Erik Schnetter
  @desc
  Output the timer table periodically
  @enddesc
@@*/
void TimerReport_OutputEvery(CCTK_ARGUMENTS) {
  DECLARE_CCTK_ARGUMENTS;
  DECLARE_CCTK_PARAMETERS;

  if (next || out_at == cctk_iteration ||
      (out_every && cctk_iteration % out_every == 0)) {
    CCTK_VINFO("Timer Report at iteration %d time %g", cctk_iteration,
               double(cctk_time));
    Output(CCTK_PASS_CTOC);

    if (next) {
      CCTK_ParameterSet("next", CCTK_THORNSTRING, "no");
    }
  }
}

/*@@
  @routine    TimerReport_OutputTerminate
  @date       2008-11-12
  @author     Erik Schnetter
  @desc
  Output the timer table periodically
  @enddesc
@@*/
void TimerReport_OutputTerminate(CCTK_ARGUMENTS) {
  DECLARE_CCTK_ARGUMENTS;
  DECLARE_CCTK_PARAMETERS;

  CCTK_VINFO("Timer Report before terminating at iteration %d time %g",
             cctk_iteration, double(cctk_time));
  Output(CCTK_PASS_CTOC);
}

/*@@
  @routine    TimerReport_Checkpoint
  @date       April 10 2004
  @author     Erik Schnetter
  @desc
  Output the timer table if before_checkpoint is set
  @enddesc
@@*/
void TimerReport_Checkpoint(CCTK_ARGUMENTS) {
  DECLARE_CCTK_ARGUMENTS;
  DECLARE_CCTK_PARAMETERS;

  if (before_checkpoint &&
      (checkpoint_every && cctk_iteration % checkpoint_every == 0)) {
    CCTK_VINFO("Timer Report before checkpointing at iteration %d, time %g",
               cctk_iteration, double(cctk_time));
    Output(CCTK_PASS_CTOC);
  }
}

/********************************************************************
 ********************    Internal Routines   ************************
 ********************************************************************/

void Output(CCTK_ARGUMENTS) {
  DECLARE_CCTK_ARGUMENTS;
  DECLARE_CCTK_PARAMETERS;

  if (output_schedule_timers) {
    PrintTimes(CCTK_PASS_CTOC);
  }

  if (output_all_timers) {
    OutputAllTimers(CCTK_PASS_CTOC);
  }

  if (output_all_timers_together) {
    OutputAllTimersTogether(CCTK_PASS_CTOC);
  }

  if (output_all_timers_per_iteration) {
    OutputAllTimersPerIteration(CCTK_PASS_CTOC);
  }

  if (output_all_timers_readable) {
    OutputAllTimersReadable(CCTK_PASS_CTOC);
  }

  if (n_top_timers > 0) {
    PrintTopTimers(CCTK_PASS_CTOC);
  }
}

void PrintTimes(CCTK_ARGUMENTS) {
  DECLARE_CCTK_ARGUMENTS;
  DECLARE_CCTK_PARAMETERS;

  if (CCTK_EQUALS(out_filename, "")) {
    // Print to stdout.
    CCTK_SchedulePrintTimes(NULL);
  } else {
    // Print to a file.
    ostringstream filename_buf;
    int myproc = CCTK_MyProc(cctkGH);
    filename_buf << out_dir << "/" << out_filename << "." << setw(6)
                 << setfill('0') << myproc << ".txt";
    string filename = filename_buf.str();
    // truncate or append
    static bool first_time = true;
    string flags = first_time && IO_TruncateOutputFiles(cctkGH) ? "w" : "a";
    first_time = false;

    FILE *file = fopen(filename.c_str(), flags.c_str());
    if (file) {
      // Print the schedule to the file
      fprintf(file, "Timer Report at iteration %d time %g:\n\n", cctk_iteration,
              double(cctk_time));
      CCTK_SchedulePrintTimesToFile(NULL, file);
      fprintf(file, "\n********************************************************"
                    "************************\n");
      fclose(file);
    } else {
      CCTK_VWARN(CCTK_WARN_ALERT,
                 "Could not open timer report output file \"%s\"",
                 filename.c_str());
    }
  }
}

void OutputAllTimers(CCTK_ARGUMENTS) {
  DECLARE_CCTK_ARGUMENTS;
  DECLARE_CCTK_PARAMETERS;

  static bool first_time = true;
  static int last_ntimers = -1;

  int ntimers = CCTK_NumTimers();
  assert(ntimers >= 0);
  cTimerData *td = CCTK_TimerCreateData();

  ostringstream filename_buf;
  filename_buf << out_dir << "/"
               << "AllTimers"
               << "." << setw(6) << setfill('0') << CCTK_MyProc(cctkGH)
               << ".txt";
  string filename = filename_buf.str();

  // truncate or append
  string flags = first_time && IO_TruncateOutputFiles(cctkGH) ? "w" : "a";

  FILE *file = fopen(filename.c_str(), flags.c_str());
  if (file) {
    if (first_time) {
      fprintf(file, "# Clock %s\n", all_timers_clock);
      fprintf(file, "# Unit seconds\n");
    }
    // If the number of timers has changed, output the header containing the
    // timer names again. It would be better to track creation and deletion
    // of timers in the flesh, but this always works as long as timers are
    // never deleted.
    if (first_time || last_ntimers != ntimers) {
      fprintf(file, "# Column 1 iteration\n");
      fprintf(file, "# Column 2 simulation time\n");
      for (int i = 0; i < ntimers; i++) {
        const char *name = CCTK_TimerName(i);

        if (name == nullptr)
          name = "";

        fprintf(file, "# Column %d %s\n", i + 3, name);
      }
    }

    fprintf(file, "%d\t%.15g", cctk_iteration, double(cctk_time));

    for (int i = 0; i < ntimers; i++) {
      CCTK_TimerI(i, td);

      const cTimerVal *tv = CCTK_GetClockValue(all_timers_clock, td);

      double timer_secs;
      if (tv != nullptr) {
        timer_secs = CCTK_TimerClockSeconds(tv);
      } else {
        const char *name = CCTK_TimerName(i);
        if (name == nullptr)
          name = "(null)";
        CCTK_VWARN(CCTK_WARN_ALERT,
                   "Clock \"%s\" not found for timer #%d \"%s\"",
                   all_timers_clock, i, name);
        timer_secs = -1;
      }

      fprintf(file, "\t%.15g", timer_secs);
    }
    fprintf(file, "\n");
    fclose(file);
  } else {
    CCTK_VWARN(CCTK_WARN_ALERT,
               "Could not open timer report output file \"%s\"",
               filename.c_str());
  }
  CCTK_TimerDestroyData(td);
  first_time = false;
  last_ntimers = ntimers;
}

void OutputAllTimersTogether(CCTK_ARGUMENTS) {
  DECLARE_CCTK_ARGUMENTS;
  DECLARE_CCTK_PARAMETERS;

  timer_stats timers;
  if (!CollectTimerInfo(cctkGH, timers))
    return;

  if (CCTK_MyProc(cctkGH) == 0) {
    static bool first_time = true;
    static int last_ntimers = -1;

    ostringstream filename_buf, filename_csv_buf, filename_tsv_buf;
    filename_buf << out_dir << "/"
                 << "AllTimers"
                 << ".txt";
    filename_csv_buf << out_dir << "/"
                     << "AllTimers"
                     << ".csv";
    filename_tsv_buf << out_dir << "/"
                     << "AllTimers"
                     << ".tsv";
    string filename = filename_buf.str();
    string filename_csv = filename_csv_buf.str();
    string filename_tsv = filename_tsv_buf.str();

    // truncate or append
    string flags = first_time && IO_TruncateOutputFiles(cctkGH) ? "w" : "a";

    FILE *file = fopen(filename.c_str(), flags.c_str());
    FILE *file_csv = fopen(filename_csv.c_str(), flags.c_str());
    FILE *file_tsv = fopen(filename_tsv.c_str(), flags.c_str());
    if (file) {
      // if (first_time) {
      //   fprintf(file, "# Clock %s\n", all_timers_clock);
      //   fprintf(file, "# Unit seconds\n");
      //   string all_timers_clock_csv = QuoteForCSV(all_timers_clock);
      //   fprintf(file_csv, "\"Clock %s\",", all_timers_clock_csv.c_str());
      //   fprintf(file_csv, "\"Unit seconds\"\n");
      //   string all_timers_clock_tsv = QuoteForTSV(all_timers_clock);
      //   fprintf(file_tsv, "Clock %s\t", all_timers_clock_tsv.c_str());
      //   fprintf(file_tsv, "Unit seconds\n");
      // }
      // If the number of timers has changed, output the header containing the
      // timer names again.  It would be better to track creation and deletion
      // of timers in the flesh, but this method here works as long as timers
      // are never deleted.
      if (last_ntimers != timers.ntimers()) {
        fprintf(file, "# Column 1 iteration\n");
        fprintf(file, "# Column 2 simulation time\n");
        fprintf(file, "# For all following columns:\n");
        fprintf(file, "#    Column 3n   average of all processes\n");
        fprintf(file, "#    Column 3n+1 minimum of all processes\n");
        fprintf(file, "#    Column 3n+2 maximum of all processes\n");
        fprintf(file_csv, "\"iteration\",\"simulation time\"");
        fprintf(file_tsv, "iteration\tsimulation time");
        for (int i = 0; i < timers.ntimers(); i++) {
          const string &name = timers.names[i];
          fprintf(file, "# Column %d %s\n", 3 * i + 3, name.c_str());
          fprintf(file_csv, ",%s,%s,%s",
                  QuoteForCSV(name + " (average)").c_str(),
                  QuoteForCSV(name + " (minimum)").c_str(),
                  QuoteForCSV(name + " (maximum)").c_str());
          fprintf(file_tsv, "\t%s\t%s\t%s",
                  QuoteForTSV(name + " (average)").c_str(),
                  QuoteForTSV(name + " (minimum)").c_str(),
                  QuoteForTSV(name + " (maximum)").c_str());
        }
        fprintf(file_csv, "\n");
        fprintf(file_tsv, "\n");
      }

      fprintf(file, "%d\t%.15g", cctk_iteration, double(cctk_time));
      fprintf(file_csv, "%d,%.15g", cctk_iteration, double(cctk_time));
      fprintf(file_tsv, "%d\t%.15g", cctk_iteration, double(cctk_time));

      for (int i = 0; i < timers.ntimers(); i++) {
        fprintf(file, "\t%.15g %.15g %.15g", double(timers.secs_avg[i]),
                double(timers.secs_min[i]), double(timers.secs_max[i]));
        fprintf(file_csv, ",%.15g,%.15g,%.15g", double(timers.secs_avg[i]),
                double(timers.secs_min[i]), double(timers.secs_max[i]));
        fprintf(file_tsv, "\t%.15g\t%.15g\t%.15g", double(timers.secs_avg[i]),
                double(timers.secs_min[i]), double(timers.secs_max[i]));
      }
      fprintf(file, "\n");
      fprintf(file_csv, "\n");
      fprintf(file_tsv, "\n");
      fclose(file);
      fclose(file_csv);
      fclose(file_tsv);
    } else {
      CCTK_VWARN(
          CCTK_WARN_ALERT,
          "Could not open timer report output files \"%s\", \"%s\", and \"%s\"",
          filename.c_str(), filename_csv.c_str(), filename_tsv.c_str());
    }

    first_time = false;
    last_ntimers = timers.ntimers();

  } // if root processor
}

void OutputAllTimersPerIteration(CCTK_ARGUMENTS) {
  DECLARE_CCTK_ARGUMENTS;
  DECLARE_CCTK_PARAMETERS;

  timer_stats timers;
  if (!CollectTimerInfo(cctkGH, timers))
    return;

  if (CCTK_MyProc(cctkGH) == 0) {
    ostringstream filename_buf, filename_csv_buf, filename_tsv_buf;
    filename_buf << out_dir << "/"
                 << "AllTimers"
                 << ".it" << setw(10) << setfill('0') << cctk_iteration
                 << ".txt";
    filename_csv_buf << out_dir << "/"
                     << "AllTimers"
                     << ".it" << setw(10) << setfill('0') << cctk_iteration
                     << ".csv";
    filename_tsv_buf << out_dir << "/"
                     << "AllTimers"
                     << ".it" << setw(10) << setfill('0') << cctk_iteration
                     << ".tsv";
    string filename = filename_buf.str();
    string filename_csv = filename_csv_buf.str();
    string filename_tsv = filename_tsv_buf.str();

    FILE *file = fopen(filename.c_str(), "w");
    FILE *file_csv = fopen(filename_csv.c_str(), "w");
    FILE *file_tsv = fopen(filename_tsv.c_str(), "w");
    if (file) {
      fprintf(file, "# Iteration: %d\n", cctk_iteration);
      fprintf(file, "# Simulation time: %.15g\n", double(cctk_time));
      fprintf(file, "#\n");
      fprintf(file, "# Column 1: timer name\n");
      fprintf(file, "# Column 2: average of all processes\n");
      fprintf(file, "# Column 3: minimum of all processes\n");
      fprintf(file, "# Column 4: maximum of all processes\n");
      for (int i = 0; i < timers.ntimers(); i++) {
        fprintf(file, "%s %.15g %.15g %.15g\n", timers.names[i].c_str(),
                double(timers.secs_avg[i]), double(timers.secs_min[i]),
                double(timers.secs_max[i]));
        fprintf(file_csv, "%s,%.15g,%.15g,%.15g\n",
                QuoteForCSV(timers.names[i]), double(timers.secs_avg[i]),
                double(timers.secs_min[i]), double(timers.secs_max[i]));
        fprintf(file_tsv, "%s\t%.15g\t%.15g\t%.15g\n",
                QuoteForTSV(timers.names[i]), double(timers.secs_avg[i]),
                double(timers.secs_min[i]), double(timers.secs_max[i]));
      }
      fclose(file);
      fclose(file_csv);
      fclose(file_tsv);
    } else {
      CCTK_VWARN(
          CCTK_WARN_ALERT,
          "Could not open timer report output files \"%s\", \"%s\", and \"%s\"",
          filename.c_str(), filename_csv.c_str(), filename_tsv.c_str());
    }
  } // if root processor
}

void OutputAllTimersReadable(CCTK_ARGUMENTS) {
  DECLARE_CCTK_ARGUMENTS;
  DECLARE_CCTK_PARAMETERS;

  timer_stats timers;
  if (!CollectTimerInfo(cctkGH, timers))
    return;

  if (CCTK_MyProc(cctkGH) == 0) {
    static bool first_time = true;
    static int last_ntimers = -1;

    ostringstream filename_buf;
    filename_buf << out_dir << "/"
                 << "AllTimersReadable"
                 << ".txt";
    string filename = filename_buf.str();

    // truncate or append
    string flags = first_time && IO_TruncateOutputFiles(cctkGH) ? "w" : "a";

    FILE *file = fopen(filename.c_str(), flags.c_str());
    if (file) {
      // if (first_time) {
      //   fprintf(file, "# Clock %s\n", all_timers_clock);
      //   fprintf(file, "# Unit seconds\n");
      // }
      // If the number of timers has changed, output the header
      // containing the timer names again. It would be better to track
      // creation and deletion of timers in the flesh, but this method
      // here works as long as timers are never deleted.
      if (last_ntimers != timers.ntimers()) {
        fprintf(file, "# Column 1 iteration\n");
        fprintf(file, "# Column 2 simulation time\n");
        fprintf(file, "# Column 3 timer number\n");
        fprintf(
            file,
            "# Column 4,5,6 average, minimum, maximum over all processors\n");
        fprintf(file, "# Column 7+ timer name\n");
      }
      for (int i = 0; i < timers.ntimers(); i++) {
        const string &name = timers.names[i];
        fprintf(file, "%d %.15g\t%d\t%.15g %.15g %.15g\t%s\n", cctk_iteration,
                double(cctk_time), i, double(timers.secs_avg[i]),
                double(timers.secs_min[i]), double(timers.secs_max[i]),
                name.c_str());
      }
      fprintf(file, "\n");
      fclose(file);
    } else {
      CCTK_VWARN(CCTK_WARN_ALERT,
                 "Could not open timer report output file \"%s\"",
                 filename.c_str());
    }

    first_time = false;
    last_ntimers = timers.ntimers();

  } // if root processor
}

void PrintTopTimers(CCTK_ARGUMENTS) {
  DECLARE_CCTK_ARGUMENTS;
  DECLARE_CCTK_PARAMETERS;

  // Collect timing information from all processes
  timer_stats timers;
  if (!CollectTimerInfo(cctkGH, timers))
    return;

  // Output the times only on the root process (because they are not reduced
  // onto the other processes anyway)
  if (CCTK_MyProc(cctkGH) != 0)
    return;

  // Sort timers (by average), largest times first
  vector<int> idx(timers.ntimers());
  for (int i = 0; i < timers.ntimers(); ++i)
    idx[i] = i;
  assert(int(timers.secs_avg.size()) == timers.ntimers());
  sort(idx.begin(), idx.end(), [&](int ia, int ib) {
    // compare average times
    return timers.secs_avg[ia] > timers.secs_avg[ib];
  });

  CCTK_VINFO("Top timers at iteration %d time %g", cctk_iteration,
             double(cctk_time));

  printf("%s\n", sep.c_str());
  // printf("%5s   %7s %7s %7s   %s (%s)\n", "%", "Time/s", "Min/s", "Max/s",
  //        "Timer", all_timers_clock);
  printf("%5s   %7s %7s %7s   %s\n", "%", "Time/s", "Min/s", "Max/s", "Timer");
  printf("%s\n", sep.c_str());

  // This should be the "CCTK total time" timer
  int total_idx = 0;
  assert(timers.ntimers() > total_idx);

  CCTK_REAL max_time = timers.secs_max[idx[total_idx]];
  int digits = lrint(floor(log10(max_time) + 1.0e-4)) + 1;

  // Output timing results
  for (int i = 0; i < min(timers.ntimers(), n_top_timers); ++i) {
    double percent =
        100.0 * timers.secs_avg[idx[i]] / timers.secs_avg[idx[total_idx]];

    // field widths: 5+3 + 7+1 + 7+1 + 7+3 + n = 80
    printf("%5.1f   %7.*f %7.*f %7.*f   %s\n", percent, 6 - digits,
           double(timers.secs_avg[idx[i]]), 6 - digits,
           double(timers.secs_min[idx[i]]), 6 - digits,
           double(timers.secs_max[idx[i]]), timers.names[idx[i]].c_str());
  }
  printf("%s\n", sep.c_str());
}

// Collect timer information onto the root processor
int CollectTimerInfo(const cGH *restrict cctkGH, timer_stats &timers) {
  DECLARE_CCTK_PARAMETERS;

  // Determine local timer names and their clock values
  // Note: Timer names are truncated for simplicity
  const int TIMERVALUENAME_LENGTH =
      201; // this includes a terminating NUL character
  typedef array<char, TIMERVALUENAME_LENGTH> timervaluename_t;
  vector<timervaluename_t> my_timervaluenames;
  vector<double> my_timervalues;
  int my_ntimers = CCTK_NumTimers();
  // Create structure to hold timer data
  cTimerData *td = CCTK_TimerCreateData();
  for (int n = 0; n < my_ntimers; ++n) {
    const char *timername = CCTK_TimerName(n);
    int ierr = CCTK_TimerI(n, td);
    assert(!ierr);
    if (*top_timers_clock == '\0') {
      // This function is misnamed; it returns the number of values, not the
      // number of clocks. (A clock can have multiple values.)
      int my_nvalues = CCTK_NumTimerClocks(td);
      assert(my_nvalues >= 0);
      assert(my_nvalues > 0);

      for (int v = 0; v < my_nvalues; ++v) {
        const cTimerVal *tv = CCTK_GetClockValueI(v, td);
        if (tv) {
          const char *clockname = CCTK_TimerClockName(tv);

          ostringstream buf;
          if (timername)
            buf << timername;
          else
            buf << "DESTROYED TIMER #" << n;
          assert(clockname);
          buf << " [" << clockname << "]";
          timervaluename_t tvn;
          snprintf(&tvn[0], TIMERVALUENAME_LENGTH, "%s", buf.str().c_str());
          my_timervaluenames.push_back(tvn);

          double val = CCTK_TimerClockSeconds(tv);
          my_timervalues.push_back(val);
        }
      }
    } else {
      const cTimerVal *tv = CCTK_GetClockValue(top_timers_clock, td);
      if (tv) {
        ostringstream buf;
        if (timername)
          buf << timername;
        else
          buf << "DESTROYED TIMER #" << n;
        buf << " [" << top_timers_clock << "]";
        timervaluename_t tvn;
        snprintf(&tvn[0], TIMERVALUENAME_LENGTH, "%s", buf.str().c_str());
        my_timervaluenames.push_back(tvn);

        double val = CCTK_TimerClockSeconds(tv);
        my_timervalues.push_back(val);
      }
    }
  }
  CCTK_TimerDestroyData(td);
  td = nullptr;
  int my_ntimervalues = my_timervalues.size();

  // Gather number of timers from each process
  int myproc = CCTK_MyProc(cctkGH);
  int nprocs = CCTK_nProcs(cctkGH);
  vector<int> all_ntimervalues;
  if (myproc == 0)
    all_ntimervalues.resize(nprocs);
#ifdef CCTK_MPI
  MPI_Gather(&my_ntimervalues, 1, MPI_INT, &all_ntimervalues[0], 1, MPI_INT, 0,
             MPI_COMM_WORLD);
#else
  assert(myproc == 0);
  assert(nprocs == 1);
  all_ntimervalues[0] = my_ntimervalues;
#endif
  int total_ntimervalues = 0;
  if (myproc == 0) {
    for (int p = 0; p < nprocs; ++p)
      total_ntimervalues += all_ntimervalues[p];
  }

  // Gather timer value names and timer values from each process
  vector<timervaluename_t> all_timervaluenames;
  vector<double> all_timervalues;
  if (myproc == 0) {
    all_timervaluenames.resize(total_ntimervalues);
    all_timervalues.resize(total_ntimervalues);
  }
  vector<int> name_displacements, value_displacements, name_counts,
      value_counts;
  if (myproc == 0) {
    name_displacements.resize(nprocs);
    value_displacements.resize(nprocs);
    name_counts.resize(nprocs);
    value_counts.resize(nprocs);
    name_displacements[0] = 0;
    value_displacements[0] = 0;
    name_counts[0] = all_ntimervalues[0] * TIMERVALUENAME_LENGTH;
    value_counts[0] = all_ntimervalues[0];
    for (int p = 1; p < nprocs; ++p) {
      name_displacements[p] = name_displacements[p - 1] +
                              all_ntimervalues[p - 1] * TIMERVALUENAME_LENGTH;
      value_displacements[p] =
          value_displacements[p - 1] + all_ntimervalues[p - 1];
      name_counts[p] = all_ntimervalues[p] * TIMERVALUENAME_LENGTH;
      value_counts[p] = all_ntimervalues[p];
    }
    assert(name_displacements[nprocs - 1] + name_counts[nprocs - 1] ==
           total_ntimervalues * TIMERVALUENAME_LENGTH);
    assert(value_displacements[nprocs - 1] + value_counts[nprocs - 1] ==
           total_ntimervalues);
  }
#ifdef CCTK_MPI
  MPI_Gatherv(&my_timervaluenames[0], my_ntimervalues * TIMERVALUENAME_LENGTH,
              MPI_CHAR, &all_timervaluenames[0], &name_counts[0],
              &name_displacements[0], MPI_CHAR, 0, MPI_COMM_WORLD);
  MPI_Gatherv(&my_timervalues[0], my_ntimervalues, MPI_DOUBLE,
              &all_timervalues[0], &value_counts[0], &value_displacements[0],
              MPI_DOUBLE, 0, MPI_COMM_WORLD);
#else
  assert(all_timervaluenames.size() == my_timervaluenames.size());
  assert(all_timervalues.size() == my_timervalues.size());
  copy(my_timervaluenames.begin(), my_timervaluenames.end(),
       all_timervaluenames.begin());
  copy(my_timervalues.begin(), my_timervalues.end(), all_timervalues.begin());
#endif

  if (myproc == 0) {

    // Construct global list of timer values
    struct stats {
      static CCTK_REAL min_neutral() {
        return numeric_limits<CCTK_REAL>::has_infinity
                   ? numeric_limits<CCTK_REAL>::infinity()
                   : numeric_limits<CCTK_REAL>::max();
      }
      CCTK_REAL count, sum, min, max;
      stats(CCTK_REAL x) : count(1), sum(x), min(x), max(x) {}
      stats() : count(0), sum(0), min(min_neutral()), max(0) {}
      stats(const stats &x, const stats &y)
          : count(x.count + y.count), sum(x.sum + y.sum),
            min(std::min(x.min, y.min)), max(std::max(x.max, y.max)) {}
      CCTK_REAL avg() const { return count == 0 ? 0 : sum / count; }
    };
    typedef map<string, stats> unique_timervalues_t;
    unique_timervalues_t unique_timervalues;
    for (int i = 0; i < total_ntimervalues; ++i) {
      auto name = &all_timervaluenames[i][0];
      auto val = all_timervalues[i];
      auto valstats = stats(val);
      auto pos = unique_timervalues.find(name);
      if (pos == unique_timervalues.end())
        unique_timervalues[name] = valstats;
      else
        unique_timervalues[name] = stats(pos->second, valstats);
    }

    // Allocate timer data structure
    for (const auto &nv : unique_timervalues) {
      const auto &name = nv.first;
      const auto &statval = nv.second;

      timers.names.push_back(name);
      timers.secs_avg.push_back(statval.avg());
      timers.secs_min.push_back(statval.min);
      timers.secs_max.push_back(statval.max);
    }
  }

  return 1;
}

// Quote a string so that it can be output as CSV entry.
string QuoteForCSV(const string &str) {
  ostringstream buf;
  // Begin with a quote
  buf << '"';
  // Copy string into result, quoting as necessary
  for (string::const_iterator it = str.begin(); it != str.end(); ++it) {
    char ch = *it;
    // Quote all quote characters by doubling them (so <"hi" there> becomes
    // <""hi"" there>
    if (ch == '"')
      buf << '"';
    buf << ch;
  }
  // End with a quote
  buf << '"';
  return buf.str();
}

// Quote a string so that it can be output as TSV entry.
string QuoteForTSV(const string &str) {
  ostringstream buf;
  // Copy string into result, replacing tabs as necessary
  for (string::const_iterator it = str.begin(); it != str.end(); ++it) {
    char ch = *it;
    if (ch == '\t')
      buf << ' ';
    else
      buf << ch;
  }
  return buf.str();
}
}
