#include <cctk.h>
#include <cctk_Parameters.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <string>
#include <sstream>
#include <vector>

#ifdef HAVE_CAPABILITY_MPI
#include <mpi.h>

#if defined __bgq__
// The processor names on a Blue Gene/Q include the MPI rank, and thus
// do not uniquely identify the host name. We therefore roll our own.
// See <https://wiki.alcf.anl.gov/parts/index.php/Blue_Gene/Q>.
#include <mpix.h>
namespace {
void MPI_Get_processor_name1(char *name, int *resultlen) {
  MPIX_Hardware_t hw;
  MPIX_Hardware(&hw);
  *resultlen =
      snprintf(name, MPI_MAX_PROCESSOR_NAME, "(%u,%u,%u,%u,%u)", hw.Coords[0],
               hw.Coords[1], hw.Coords[2], hw.Coords[3], hw.Coords[4]);
  // ignoring hw.Coords[5], which is the core number inside a node
}
} // namespace
#define MPI_Get_processor_name MPI_Get_processor_name1
#endif
#endif

#ifdef _OPENMP
#include <omp.h>
#else
#include <sys/time.h>
int omp_get_max_threads() { return 1; }
int omp_get_num_threads() { return 1; }
int omp_get_thread_num() { return 0; }
#endif

#include <hwloc.h>
// On a Blue Gene/Q, hwloc reports per-process hardware information
// instead of per-node hardware information. We need to correct for
// this.
#ifdef __bgq__
#define HWLOC_PER_PROCESS
#endif

using namespace std;

namespace {
int divup(int a, int b) {
  assert(a >= 0);
  assert(b > 0);
  return (a + b - 1) / b;
}

bool is_pow2(int a) {
  if (a <= 0)
    return false;
  while (a != 1) {
    if (a % 2)
      return false;
    a /= 2;
  }
  return true;
}
} // namespace

namespace {

// Check that OpenMP counts and numbers threads as expected
void check_openmp() {
  bool found_inconsistency = false;

  // Count OpenMP threads
  int num_threads_direct = 0;
#pragma omp parallel reduction(+ : num_threads_direct)
  { ++num_threads_direct; }
  int num_threads_omp = -1;
#pragma omp parallel
  {
#pragma omp master
    { num_threads_omp = omp_get_num_threads(); }
  }
  if (num_threads_direct != num_threads_omp) {
    found_inconsistency = true;
    CCTK_VWarn(CCTK_WARN_ALERT, __LINE__, __FILE__, CCTK_THORNSTRING,
               "Number of OpenMP threads is inconsistent: counting %d threads, "
               "but OpenMP run-time reports %d threads",
               num_threads_direct, num_threads_omp);
  }

  // Check OpenMP thread numbers
  vector<int> thread_nums;
#pragma omp parallel
  {
#pragma omp critical
    { thread_nums.push_back(omp_get_thread_num()); }
  }
  // Prevent insanity
  assert(int(thread_nums.size()) == num_threads_direct);
  int max_thread_num = -1;
  for (int i = 0; i < int(thread_nums.size()); ++i) {
    max_thread_num = max(max_thread_num, thread_nums.at(i));
  }
  vector<int> thread_counts(max_thread_num + 1, 0);
  for (int i = 0; i < int(thread_nums.size()); ++i) {
    ++thread_counts.at(thread_nums.at(i));
  }
  int num_threads_direct_again = 0;
  for (size_t i = 0; i < thread_counts.size(); ++i) {
    num_threads_direct_again += thread_counts.at(i);
  }
  // Prevent insanity
  assert(num_threads_direct_again == num_threads_direct);
  bool thread_counts_bad = int(thread_counts.size()) < num_threads_direct;
  for (int i = 0; i < int(thread_counts.size()); ++i) {
    thread_counts_bad =
        thread_counts_bad or thread_counts.at(i) != (i < num_threads_direct);
  }
  if (thread_counts_bad) {
    found_inconsistency = true;
    printf("OpenMP thread numbers:");
    for (int i = 0; i < int(thread_counts.size()); ++i) {
      for (int j = 0; j < thread_counts.at(i); ++j) {
        printf(" %d", i);
      }
    }
    printf("\n");
    CCTK_WARN(CCTK_WARN_ALERT, "OpenMP threads are numbered inconsistently");
  }

  if (found_inconsistency) {
    CCTK_ERROR("Severe OpenMP inconsistency detected -- aborting");
  }
}
} // namespace

namespace {

struct mpi_host_mapping_t {
  int mpi_num_procs, mpi_proc_num;
  int mpi_num_hosts, mpi_host_num;
  int mpi_num_procs_on_host, mpi_proc_num_on_host;

  void load();
};

mpi_host_mapping_t *mpi_host_mapping = NULL;

#ifdef HAVE_CAPABILITY_MPI

void mpi_host_mapping_t::load() {
  CCTK_INFO("MPI process-to-host mapping:");

  MPI_Comm comm = MPI_COMM_WORLD;
  int const root = 0;

  // Basic global rank/size, the same on every process
  MPI_Comm_size(comm, &mpi_num_procs);
  MPI_Comm_rank(comm, &mpi_proc_num);
  printf("This is MPI process %d of %d\n", mpi_proc_num, mpi_num_procs);

  // Step 1: figure out which processes share a physical host.
  //
  // Every process reports its hostname; these are gathered onto the
  // root process (root is the only one that needs the full list, so
  // only root allocates the receive buffer). The root then groups
  // processes into hosts by matching identical hostnames, assigning
  // each distinct hostname a host index 0, 1, 2, ... in the order it
  // is first seen. host2procs1 collects, for each host index, the list
  // of global MPI ranks that reported that hostname.
  char procname[MPI_MAX_PROCESSOR_NAME];
  int procnamelen;
  MPI_Get_processor_name(procname, &procnamelen);
  vector<char> procnames;
  if (mpi_proc_num == root) {
    procnames.resize(MPI_MAX_PROCESSOR_NAME * mpi_num_procs);
  }
  MPI_Gather(procname, MPI_MAX_PROCESSOR_NAME, MPI_CHAR, &procnames[0],
             MPI_MAX_PROCESSOR_NAME, MPI_CHAR, root, comm);
  vector<int> host_byproc;
  map<int, list<int> > host2procs1;
  if (mpi_proc_num == root) {
    map<string, int> hostname2host;
    vector<string> hostnames;
    hostnames.reserve(mpi_num_procs);
    host_byproc.resize(mpi_num_procs);
    mpi_num_hosts = 0;
    for (int proc = 0; proc < mpi_num_procs; ++proc) {
      string hostname(&procnames[MPI_MAX_PROCESSOR_NAME * proc]);
      if (hostname2host.count(hostname) == 0) {
        // First time we have seen this hostname: assign it the next
        // free host index
        hostname2host[hostname] = mpi_num_hosts;
        hostnames.push_back(hostname);
        ++mpi_num_hosts;
      }
      int host = hostname2host[hostname];
      host_byproc[proc] = host;
      host2procs1[host].push_back(proc);
    }
    printf("MPI hosts:\n");
    for (int host = 0; host < mpi_num_hosts; ++host) {
      printf("  %d: %s\n", host, hostnames[host].c_str());
    }
  }
  // Tell every process how many hosts there are in total, and which
  // host index it was assigned above (host_byproc[proc] scattered to
  // process proc's mpi_host_num)
  MPI_Bcast(&mpi_num_hosts, 1, MPI_INT, root, comm);
  MPI_Scatter(&host_byproc[0], 1, MPI_INT, &mpi_host_num, 1, MPI_INT, root,
              comm);
  printf("This MPI process runs on host %d of %d\n", mpi_host_num,
         mpi_num_hosts);

  // Step 2: number each host's processes 0, 1, 2, ... in order of
  // increasing global rank. This numbering is used elsewhere (e.g. to
  // lay out OpenMP threads across a host's processes). Note that
  // global rank 0 is always process 0 on host 0: host 0 is defined
  // above as whichever host reports the same hostname as rank 0, and
  // rank 0 -- being the smallest possible rank -- is necessarily the
  // smallest (and thus "process 0") among any set of ranks it belongs
  // to. So no special-casing is needed to identify "the" root process.
  //
  // host2procs1 (built above, root process only) maps each host index
  // to the list of ranks on it, in the order those ranks happened to
  // report their hostname -- not necessarily sorted. Each such list is
  // copied into a vector and sorted by rank, so "process number on
  // host" is well defined and reproducible.
  vector<int> num_procs_on_host_byproc;
  vector<int> proc_num_on_host_byproc;
  if (mpi_proc_num == root) {
    vector<vector<int> > host2procs(mpi_num_hosts);
    vector<int> num_procs_on_host_byhost(mpi_num_hosts);
    num_procs_on_host_byproc.resize(mpi_num_procs);
    proc_num_on_host_byproc.resize(mpi_num_procs);
    for (int host = 0; host < mpi_num_hosts; ++host) {
      list<int> const &host_procs1 = host2procs1[host];
      vector<int> &host_procs = host2procs[host];
      host_procs.reserve(host_procs1.size());
      for (list<int>::const_iterator iproc = host_procs1.begin();
           iproc != host_procs1.end(); ++iproc) {
        host_procs.push_back(*iproc);
      }
      sort(host_procs.begin(), host_procs.end());
      int num_procs_on_host = host_procs.size();
      num_procs_on_host_byhost[host] = num_procs_on_host;
      for (int proc_num_on_host = 0; proc_num_on_host < num_procs_on_host;
           ++proc_num_on_host) {
        int proc = host_procs[proc_num_on_host];
        num_procs_on_host_byproc[proc] = num_procs_on_host;
        proc_num_on_host_byproc[proc] = proc_num_on_host;
      }
    }
  }
  // Scatter each process's own entry out of the per-process arrays
  // computed above (which only exist on root)
  MPI_Scatter(&num_procs_on_host_byproc[0], 1, MPI_INT, &mpi_num_procs_on_host,
              1, MPI_INT, root, comm);
  MPI_Scatter(&proc_num_on_host_byproc[0], 1, MPI_INT, &mpi_proc_num_on_host, 1,
              MPI_INT, root, comm);
  printf("On this host, this is MPI process %d of %d\n", mpi_proc_num_on_host,
         mpi_num_procs_on_host);
}

#else

void mpi_host_mapping_t::load() {
  mpi_num_procs = 1;
  mpi_proc_num = 0;
  mpi_num_hosts = 1;
  mpi_host_num = 0;
  mpi_num_procs_on_host = 1;
  mpi_proc_num_on_host = 0;
}

#endif
} // namespace

// Inspired by code in hwloc's documentation

namespace {

#define OUTPUT_SUPPORT(FIELD)                                                  \
  printf("  %-41s: %s\n", #FIELD, topology_support->FIELD ? "yes" : "no")

void output_support(hwloc_topology_t topology) {
  CCTK_INFO("Topology support:");
  hwloc_topology_support const *topology_support =
      hwloc_topology_get_support(topology);
  printf("Discovery support:\n");
  OUTPUT_SUPPORT(discovery->pu);
  printf("CPU binding support:\n");
  OUTPUT_SUPPORT(cpubind->set_thisproc_cpubind);
  OUTPUT_SUPPORT(cpubind->get_thisproc_cpubind);
  OUTPUT_SUPPORT(cpubind->set_proc_cpubind);
  OUTPUT_SUPPORT(cpubind->get_proc_cpubind);
  OUTPUT_SUPPORT(cpubind->set_thisthread_cpubind);
  OUTPUT_SUPPORT(cpubind->get_thisthread_cpubind);
  OUTPUT_SUPPORT(cpubind->set_thread_cpubind);
  OUTPUT_SUPPORT(cpubind->get_thread_cpubind);
  OUTPUT_SUPPORT(cpubind->get_thisproc_last_cpu_location);
  OUTPUT_SUPPORT(cpubind->get_proc_last_cpu_location);
  OUTPUT_SUPPORT(cpubind->get_thisthread_last_cpu_location);
  printf("Memory binding support:\n");
  OUTPUT_SUPPORT(membind->set_thisproc_membind);
  OUTPUT_SUPPORT(membind->get_thisproc_membind);
  OUTPUT_SUPPORT(membind->set_proc_membind);
  OUTPUT_SUPPORT(membind->get_proc_membind);
  OUTPUT_SUPPORT(membind->set_thisthread_membind);
  OUTPUT_SUPPORT(membind->get_thisthread_membind);
  OUTPUT_SUPPORT(membind->set_area_membind);
  OUTPUT_SUPPORT(membind->get_area_membind);
  OUTPUT_SUPPORT(membind->alloc_membind);
  OUTPUT_SUPPORT(membind->firsttouch_membind);
  OUTPUT_SUPPORT(membind->bind_membind);
  OUTPUT_SUPPORT(membind->interleave_membind);
#if (HWLOC_API_VERSION < 0x00020000)
  OUTPUT_SUPPORT(membind->replicate_membind);
#endif
  OUTPUT_SUPPORT(membind->nexttouch_membind);
  OUTPUT_SUPPORT(membind->migrate_membind);
}

void output_object(hwloc_topology_t topology, hwloc_obj_t obj, int depth) {
  char type_buf[1000], attr_buf[1000];
  hwloc_obj_type_snprintf(type_buf, sizeof type_buf, obj, 1);
  hwloc_obj_attr_snprintf(attr_buf, sizeof attr_buf, obj, ", ", 1);
  printf("%*s%s L#%d: (P#%d%s%s)\n", 2 * depth, "", type_buf,
         obj->logical_index, obj->os_index, strlen(attr_buf) == 0 ? "" : ", ",
         attr_buf);

  for (unsigned i = 0; i < obj->arity; ++i) {
    // TODO: output index (physical, logical?)
    output_object(topology, obj->children[i], depth + 1);
  }
}

void output_objects(hwloc_topology_t topology) {
  CCTK_INFO("Hardware objects in this node:");
  output_object(topology, hwloc_get_root_obj(topology), 0);
}

void output_distances(hwloc_topology_t topology) {
  CCTK_INFO("Distances between hardware objects:");
#if (HWLOC_API_VERSION >= 0x00020000)
  unsigned nr = 0;
  hwloc_distances_get(topology, &nr, NULL, 0, 0);
  if (nr == 0) {
    printf("  (none reported)\n");
    return;
  }
  vector<hwloc_distances_s *> distances(nr);
  hwloc_distances_get(topology, &nr, &distances[0], 0, 0);
  for (unsigned i = 0; i < nr; ++i) {
    hwloc_distances_s const *const dist = distances[i];
    char type_buf[1000];
    hwloc_obj_type_snprintf(type_buf, sizeof type_buf, dist->objs[0], 1);
    printf("  Distance matrix between %u %s objects (%s):\n", dist->nbobjs,
           type_buf,
           dist->kind & HWLOC_DISTANCES_KIND_MEANS_BANDWIDTH ? "bandwidth"
                                                              : "latency");
    for (unsigned j = 0; j < dist->nbobjs; ++j) {
      printf("   ");
      for (unsigned k = 0; k < dist->nbobjs; ++k) {
        printf(" %6llu",
               (unsigned long long)dist->values[j * dist->nbobjs + k]);
      }
      printf("\n");
    }
    hwloc_distances_release(topology, distances[i]);
  }
#else
  printf("  (distance-matrix reporting requires hwloc >= 2.0)\n");
#endif
}

void output_bindings(hwloc_topology_t topology,
                     mpi_host_mapping_t const &host_mapping) {
  hwloc_topology_support const *topology_support =
      hwloc_topology_get_support(topology);
  if (not topology_support->cpubind->get_thisthread_cpubind) {
    CCTK_INFO("Cannot determine thread CPU bindings");
    return;
  }

  CCTK_INFO("Thread CPU bindings:");
  // Output all information about host 0, gathered by global rank 0,
  // which is always process 0 on host 0 (host 0 is defined as
  // whichever host rank 0 is on, and rank 0 -- being the smallest
  // possible rank -- is always the smallest, i.e. "process 0", on
  // whatever host it shares)
  int const root = 0;
  int const host = 0;
  bool const is_aggregator = host_mapping.mpi_proc_num == root;
  if (host_mapping.mpi_host_num == host) {
    ostringstream buf;
    buf << "  "
        << "MPI process " << host_mapping.mpi_proc_num << " "
        << "on host " << host_mapping.mpi_host_num << " "
        << "(process " << host_mapping.mpi_proc_num_on_host << " "
        << "of " << host_mapping.mpi_num_procs_on_host << " "
        << "on this host)\n";

    int const pu_depth = hwloc_get_type_or_below_depth(topology, HWLOC_OBJ_PU);
    if (not(pu_depth >= 0))
      CCTK_VERROR("Could not determine hwloc depth of PUs (got %d)", pu_depth);
    int const num_pus = hwloc_get_nbobjs_by_depth(topology, pu_depth);
    if (not(num_pus > 0))
      CCTK_VERROR("Could not determine number of PUs (got %d)", num_pus);
#pragma omp parallel
    {
      int const num_threads = omp_get_num_threads();
      for (int thread = 0; thread < num_threads; ++thread) {
        if (thread == omp_get_thread_num()) {
          hwloc_cpuset_t cpuset = hwloc_bitmap_alloc();
          if (cpuset) {
            int const ierr =
                hwloc_get_cpubind(topology, cpuset, HWLOC_CPUBIND_THREAD);
            if (not ierr) {
              hwloc_cpuset_t lcpuset = hwloc_bitmap_alloc();
              if (lcpuset) {
                for (int pu_num = 0; pu_num < num_pus; ++pu_num) {
                  hwloc_obj_t const pu_obj =
                      hwloc_get_obj_by_depth(topology, pu_depth, pu_num);
                  if (hwloc_bitmap_isset(cpuset, pu_obj->os_index)) {
                    hwloc_bitmap_set(lcpuset, pu_num);
                  }
                }
                char lcpuset_buf[1000];
                hwloc_bitmap_list_snprintf(lcpuset_buf, sizeof lcpuset_buf,
                                           lcpuset);
                hwloc_bitmap_free(lcpuset);
                char cpuset_buf[1000];
                hwloc_bitmap_list_snprintf(cpuset_buf, sizeof cpuset_buf,
                                           cpuset);
                // printf("OpenMP thread %d: PU set L#{%s} P#{%s}\n",
                //        thread, lcpuset_buf, cpuset_buf);
                buf << "    "
                    << "OpenMP thread " << thread << ": "
                    << "PU set L#{" << lcpuset_buf << "} "
                    << "P#{" << cpuset_buf << "}\n";
              } else {
                CCTK_VWarn(CCTK_WARN_ALERT, __LINE__, __FILE__,
                           CCTK_THORNSTRING,
                           "Could not allocate bitmap for CPU bindings");
              }
            } else {
              CCTK_VWarn(CCTK_WARN_ALERT, __LINE__, __FILE__, CCTK_THORNSTRING,
                         "Could not obtain CPU binding for thread %d", thread);
            }
            hwloc_bitmap_free(cpuset);
          } else {
            CCTK_VWarn(CCTK_WARN_ALERT, __LINE__, __FILE__, CCTK_THORNSTRING,
                       "Could not allocate bitmap for CPU bindings");
          }
        }
#pragma omp barrier
      }
    }

    // Collect all output
    string const bufstr = buf.str();
#ifdef HAVE_CAPABILITY_MPI
    if (is_aggregator) {
      // Output for the aggregating process itself
      printf("%s", bufstr.c_str());
      // Receive from all other processes on this host
      for (int proc = 1; proc < host_mapping.mpi_num_procs_on_host; ++proc) {
        int rbuflen;
        MPI_Recv(&rbuflen, 1, MPI_INT, MPI_ANY_SOURCE, proc, MPI_COMM_WORLD,
                 MPI_STATUS_IGNORE);
        vector<char> rbufstr(rbuflen + 1);
        MPI_Recv(&rbufstr[0], rbuflen, MPI_CHAR, MPI_ANY_SOURCE, proc,
                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        rbufstr[rbuflen] = '\0';
        printf("%s", &rbufstr[0]);
      }
    } else {
      // Send to the aggregating process (global rank 0)
      int const buflen = bufstr.size();
      MPI_Send(const_cast<int *>(&buflen), 1, MPI_INT, root,
               host_mapping.mpi_proc_num_on_host, MPI_COMM_WORLD);
      MPI_Send(const_cast<char *>(bufstr.c_str()), buflen, MPI_CHAR, root,
               host_mapping.mpi_proc_num_on_host, MPI_COMM_WORLD);
    }
#else
    // No MPI: this process is trivially its own aggregator
    printf("%s", bufstr.c_str());
#endif

  } // if on root host
}

void set_bindings(hwloc_topology_t topology,
                  mpi_host_mapping_t const &host_mapping) {
  DECLARE_CCTK_PARAMETERS;

  hwloc_topology_support const *topology_support =
      hwloc_topology_get_support(topology);
  if (not topology_support->cpubind->set_thisthread_cpubind) {
    CCTK_INFO("Cannot set thread CPU bindings");
    return;
  }

  bool dense_layout = true;
  if (CCTK_EQUALS(thread_layout, "dense")) {
    dense_layout = true;
  } else if (CCTK_EQUALS(thread_layout, "loose")) {
    dense_layout = false;
  } else {
    CCTK_VERROR("Illegal value \"%s\" for parameter \"thread_layout\"",
                thread_layout);
  }

  // The memory binding policy is a property of the machine, not of any
  // particular simulation, so it is controlled by an environment
  // variable -- following the convention of e.g. CACTUS_NUM_THREADS --
  // rather than by a steerable parameter.
  bool do_set_membind = topology_support->membind->set_thisthread_membind;
  hwloc_membind_policy_t membind_policy = HWLOC_MEMBIND_BIND;
  char const *const CACTUS_MEMBIND_POLICY = getenv("CACTUS_MEMBIND_POLICY");
  if (not do_set_membind) {
    CCTK_INFO("Cannot set thread memory bindings");
  } else if (CACTUS_MEMBIND_POLICY and
             CCTK_EQUALS(CACTUS_MEMBIND_POLICY, "none")) {
    do_set_membind = false;
    CCTK_INFO("Thread memory bindings disabled via "
              "CACTUS_MEMBIND_POLICY=none");
  } else {
    bool policy_supported = false;
    if (not CACTUS_MEMBIND_POLICY or
        CCTK_EQUALS(CACTUS_MEMBIND_POLICY, "bind")) {
      membind_policy = HWLOC_MEMBIND_BIND;
      policy_supported = topology_support->membind->bind_membind;
    } else if (CCTK_EQUALS(CACTUS_MEMBIND_POLICY, "firsttouch")) {
      membind_policy = HWLOC_MEMBIND_FIRSTTOUCH;
      policy_supported = topology_support->membind->firsttouch_membind;
    } else if (CCTK_EQUALS(CACTUS_MEMBIND_POLICY, "interleave")) {
      membind_policy = HWLOC_MEMBIND_INTERLEAVE;
      policy_supported = topology_support->membind->interleave_membind;
    } else {
      CCTK_VERROR("Illegal value \"%s\" for environment variable "
                  "CACTUS_MEMBIND_POLICY (must be \"bind\", \"firsttouch\", "
                  "\"interleave\", or \"none\")",
                  CACTUS_MEMBIND_POLICY);
    }
    if (not policy_supported) {
      do_set_membind = false;
      CCTK_VWarn(CCTK_WARN_ALERT, __LINE__, __FILE__, CCTK_THORNSTRING,
                 "Memory binding policy \"%s\" is not supported on this "
                 "system; not setting thread memory bindings",
                 CACTUS_MEMBIND_POLICY ? CACTUS_MEMBIND_POLICY : "bind");
    } else {
      CCTK_VINFO("Setting thread memory bindings with policy \"%s\"",
                 CACTUS_MEMBIND_POLICY ? CACTUS_MEMBIND_POLICY : "bind");
    }
  }

  // TODO: use hwloc_distribute instead
  CCTK_INFO("Setting thread CPU bindings:");
#pragma omp parallel
  {
    // All quantities are per host
    int const core_depth =
        hwloc_get_type_or_below_depth(topology, HWLOC_OBJ_CORE);
    if (not(core_depth >= 0))
      CCTK_VERROR("Could not determine hwloc depth of cores (got %d)",
                  core_depth);
    int const num_cores = hwloc_get_nbobjs_by_depth(topology, core_depth);
    if (not(num_cores > 0))
      CCTK_VERROR("Could not determine number of cores (got %d)", num_cores);
    int const pu_depth = hwloc_get_type_or_below_depth(topology, HWLOC_OBJ_PU);
    if (not(pu_depth >= 0))
      CCTK_VERROR("Could not determine hwloc depth of PUs (got %d)", pu_depth);
    int const num_pus = hwloc_get_nbobjs_by_depth(topology, pu_depth);
    if (not(num_pus > 0))
      CCTK_VERROR("Could not determine number of PUs (got %d)", num_pus);
    if (not(num_pus % num_cores == 0))
      CCTK_VERROR(
          "Number of PUs (%d) is not a multiple of number of cores (%d)",
          num_pus, num_cores);
    int const max_smt_threads = num_pus / num_cores;
    int const num_threads_in_proc = omp_get_num_threads();
    int const num_procs = host_mapping.mpi_num_procs_on_host;
    int const num_threads = num_threads_in_proc * num_procs;
    int const num_smt_threads = divup(num_threads, num_cores);
    int const core_spacing =
        dense_layout ? 1 : num_cores / (num_threads / num_smt_threads);
    int const proc_num = host_mapping.mpi_proc_num_on_host;
    int const thread_offset = num_threads_in_proc * proc_num;
    // Bind thread to exactly one PU
    for (int thread_num_in_proc = 0; thread_num_in_proc < num_threads_in_proc;
         ++thread_num_in_proc) {
      if (thread_num_in_proc == omp_get_thread_num()) {
        int const thread_num = thread_offset + thread_num_in_proc;
        // Map requested threads to existing cores, oversubscribing
        // or spacing if necessary
        int const core_num = thread_num / num_smt_threads * core_spacing;
        if (not(core_num < num_cores))
          CCTK_VERROR("Computed core number %d is out of range (there are "
                      "only %d cores) -- inconsistent thread/core layout",
                      core_num, num_cores);
        // Map requested SMT threads to existing PUs,
        // oversubscribing if necessary
        int const pu_offset =
            thread_num % num_smt_threads * max_smt_threads / num_smt_threads;
        if (not(pu_offset < max_smt_threads))
          CCTK_VERROR("Computed PU offset %d is out of range (there are "
                      "only %d SMT threads per core)",
                      pu_offset, max_smt_threads);
        int const pu_num = core_num * max_smt_threads + pu_offset;
        if (not(pu_num < num_pus))
          CCTK_VERROR("Computed PU number %d is out of range (there are "
                      "only %d PUs)",
                      pu_num, num_pus);
        hwloc_obj_t pu_obj = hwloc_get_obj_by_depth(topology, pu_depth, pu_num);
        if (not pu_obj)
          CCTK_VERROR("hwloc returned no object for PU number %d", pu_num);
        // hwloc_cpuset_t cpuset = hwloc_bitmap_dup(pu_obj->cpuset);
        hwloc_cpuset_t cpuset = pu_obj->cpuset;
        int ierr;
        ierr = hwloc_set_cpubind(topology, cpuset,
                                 HWLOC_CPUBIND_THREAD | HWLOC_CPUBIND_STRICT);
        if (ierr) {
          ierr = hwloc_set_cpubind(topology, cpuset, HWLOC_CPUBIND_THREAD);
          if (not ierr) {
            CCTK_VWarn(CCTK_WARN_ALERT, __LINE__, __FILE__, CCTK_THORNSTRING,
                       "Could not strictly bind thread %d to its target PU; "
                       "fell back to a non-strict binding, which the OS may "
                       "silently ignore if this process's CPU set is "
                       "externally restricted (e.g. by a job scheduler)",
                       thread_num_in_proc);
          }
        }
        if (ierr) {
          CCTK_VWarn(CCTK_WARN_ALERT, __LINE__, __FILE__, CCTK_THORNSTRING,
                     "Could not set CPU binding for thread %d "
                     "(both strict and non-strict binding attempts failed)",
                     thread_num_in_proc);
        }
        // Bind this thread's memory allocations to the same PU's local
        // memory, using the policy chosen above (or bound to nothing,
        // and NUMA-local by first touch, per the OS default, if
        // CACTUS_MEMBIND_POLICY=none)
        if (not ierr and do_set_membind) {
          int const mierr = hwloc_set_membind(
              topology, cpuset, membind_policy, HWLOC_MEMBIND_THREAD);
          if (mierr) {
            CCTK_VWarn(CCTK_WARN_ALERT, __LINE__, __FILE__, CCTK_THORNSTRING,
                       "Could not set memory binding for thread %d",
                       thread_num_in_proc);
          }
        }
        // hwloc_bitmap_free(cpuset);
      }
#pragma omp barrier
    }
  }
}
} // namespace

namespace {

enum memory_t { mem_cache = 0, mem_local = 1, mem_global = 2 };
struct node_topology_info_t {
  int num_smt_threads; // threads per core
  int max_smt_threads; // pus per core
  struct cache_info_t {
    char const *name;
    memory_t type;
    ptrdiff_t size; // data cache size in bytes (0 if unknown)
    int linesize;   // data cache line size in bytes (0 if unknown)
    int stride;     // data cache stride in bytes (0 if unknown)
    int num_pus;    // number of PUs which share this cache
  };
  vector<cache_info_t> cache_info;

  void load(hwloc_topology_t const &topology,
            mpi_host_mapping_t const &mpi_host_mapping);
};

node_topology_info_t *node_topology_info = NULL;

void node_topology_info_t::load(hwloc_topology_t const &topology,
                                mpi_host_mapping_t const &host_mapping) {
  CCTK_INFO("Extracting CPU/cache/memory properties:");
  // All quantities are per host
  int const core_depth =
      hwloc_get_type_or_below_depth(topology, HWLOC_OBJ_CORE);
  if (not(core_depth >= 0))
    CCTK_VERROR("Could not determine hwloc depth of cores (got %d)",
                core_depth);
  int num_cores = hwloc_get_nbobjs_by_depth(topology, core_depth);
#ifdef HWLOC_PER_PROCESS
  num_cores *= host_mapping.mpi_num_procs_on_host;
#endif
  if (not(num_cores > 0))
    CCTK_VERROR("Could not determine number of cores (got %d)", num_cores);
  int const pu_depth = hwloc_get_type_or_below_depth(topology, HWLOC_OBJ_PU);
  if (not(pu_depth >= 0))
    CCTK_VERROR("Could not determine hwloc depth of PUs (got %d)", pu_depth);
  int num_pus = hwloc_get_nbobjs_by_depth(topology, pu_depth);
#ifdef HWLOC_PER_PROCESS
  num_pus *= host_mapping.mpi_num_procs_on_host;
#endif
  if (not(num_pus > 0))
    CCTK_VERROR("Could not determine number of PUs (got %d)", num_pus);
  if (not(num_pus % num_cores == 0))
    CCTK_VERROR("Number of PUs (%d) is not a multiple of number of cores (%d)",
                num_pus, num_cores);
  max_smt_threads = num_pus / num_cores;
  printf("  There are %d PUs per core (aka hardware SMT threads)\n",
         max_smt_threads);
  int const num_threads_in_proc = omp_get_max_threads();
  int const num_procs = host_mapping.mpi_num_procs_on_host;
  int const num_threads = num_threads_in_proc * num_procs;
  // TODO: calculate this instead by looking at logical core numbers
  // for each thread
  num_smt_threads = divup(num_threads, num_cores);
  printf("  There are %d threads per core (aka SMT threads used)\n",
         num_smt_threads);
  if (num_smt_threads > max_smt_threads) {
    printf("WARNING: This is larger than the number of hardware SMT threads\n");
  }
  if (num_threads_in_proc % num_smt_threads != 0) {
    printf("WARNING: This does not evenly divide the number of threads per "
           "process\n");
  }
  if (not(num_smt_threads > 0))
    CCTK_VERROR("Computed number of SMT threads used is not positive (got %d)",
                num_smt_threads);

  if (not cache_info.empty())
    CCTK_VERROR("Internal error: cache_info is not empty at start of load()");
  for (size_t cache_level = 1; true; ++cache_level) {
#if (HWLOC_API_VERSION < 0x00020000)
    int const cache_depth =
        hwloc_get_cache_type_depth(topology, cache_level, HWLOC_OBJ_CACHE_DATA);
#else
    hwloc_obj_type_t const cache_type[] = {HWLOC_OBJ_L1CACHE, HWLOC_OBJ_L2CACHE,
                                           HWLOC_OBJ_L3CACHE, HWLOC_OBJ_L4CACHE,
                                           HWLOC_OBJ_L5CACHE};
    if (cache_level > sizeof(cache_type) / sizeof(cache_type[0]))
      break;
    assert(cache_level >= 1);
    assert(cache_level <= sizeof(cache_type) / sizeof(cache_type[0]));
    int const cache_depth =
        hwloc_get_type_depth(topology, cache_type[cache_level - 1]);
#endif
    if (cache_depth < 0)
      break;
    int const num_caches = hwloc_get_nbobjs_by_depth(topology, cache_depth);
    if (not(num_caches > 0))
      CCTK_VERROR("Could not determine number of caches at depth %d (got %d)",
                  cache_depth, num_caches);
    int const cache_num = 0; // just look at first cache
    hwloc_obj_t const cache_obj =
        hwloc_get_obj_by_depth(topology, cache_depth, cache_num);
    if (not cache_obj)
      CCTK_VERROR("hwloc returned no object for cache depth %d", cache_depth);
#if (HWLOC_API_VERSION < 0x00020000)
    if (not(cache_obj->type == HWLOC_OBJ_CACHE))
      CCTK_VERROR("hwloc object at cache depth %d has unexpected type",
                  cache_depth);
#else
    if (not(cache_obj->type == HWLOC_OBJ_L5CACHE ||
            cache_obj->type == HWLOC_OBJ_L4CACHE ||
            cache_obj->type == HWLOC_OBJ_L3CACHE ||
            cache_obj->type == HWLOC_OBJ_L2CACHE ||
            cache_obj->type == HWLOC_OBJ_L1CACHE))
      CCTK_VERROR("hwloc object at cache depth %d has unexpected type",
                  cache_depth);
#endif
    hwloc_obj_attr_u::hwloc_cache_attr_s const &cache_attr =
        cache_obj->attr->cache;
    char const *const cache_type_str =
        cache_attr.type == HWLOC_OBJ_CACHE_UNIFIED
            ? "unified"
            : cache_attr.type == HWLOC_OBJ_CACHE_DATA
                  ? "data"
                  : cache_attr.type == HWLOC_OBJ_CACHE_INSTRUCTION
                        ? "instruction"
                        : "UNKNOWN";
    ostringstream namebuf;
    switch (cache_attr.type) {
    case HWLOC_OBJ_CACHE_UNIFIED:
      namebuf << "L";
      break;
    case HWLOC_OBJ_CACHE_DATA:
      namebuf << "D";
      break;
    case HWLOC_OBJ_CACHE_INSTRUCTION:
      namebuf << "I";
      break;
    default:
      namebuf << "?";
      break;
    }
    namebuf << cache_attr.depth << " cache";
    string const name = namebuf.str();
    int const cache_stride = (cache_attr.associativity == 0
                                  ? 0
                                  : cache_attr.size / cache_attr.associativity);
    printf("  Cache %s has type \"%s\" depth %u\n"
           "    size %td linesize %u associativity %d stride %d, "
           "for %d PUs\n",
           cache_obj->name ? cache_obj->name : "(unknown name)", cache_type_str,
           cache_attr.depth, (ptrdiff_t)cache_attr.size, cache_attr.linesize,
           cache_attr.associativity, cache_stride, num_pus / num_caches);
    if (cache_attr.type != HWLOC_OBJ_CACHE_INSTRUCTION) {
      // These are sanity checks on data reported by hwloc; a violation
      // indicates unusual (but not necessarily unsafe) hardware-reported
      // data, so we warn rather than abort the run
      if (not(cache_attr.linesize == 0 or is_pow2(cache_attr.linesize)))
        CCTK_VWarn(CCTK_WARN_ALERT, __LINE__, __FILE__, CCTK_THORNSTRING,
                   "hwloc reported a non-power-of-two cache line size (%u) "
                   "for cache \"%s\"",
                   cache_attr.linesize, name.c_str());
      if (not(cache_stride >= 0))
        CCTK_VWarn(CCTK_WARN_ALERT, __LINE__, __FILE__, CCTK_THORNSTRING,
                   "Computed a negative cache stride (%d) for cache \"%s\"",
                   cache_stride, name.c_str());
      // Cache strides may not be powers of two
      cache_info_t new_cache_info;
      new_cache_info.name = strdup(name.c_str());
      new_cache_info.type = mem_cache;
      new_cache_info.size = cache_attr.size;
      new_cache_info.linesize = cache_attr.linesize;
      new_cache_info.stride = cache_stride;
      new_cache_info.num_pus = num_pus / num_caches; // TODO
      cache_info.push_back(new_cache_info);
    }
  }

  // Describe (socket-local) memory as well, creating fake cache
  // entries
  for (int type_idx = 0; type_idx < 2; ++type_idx) {
    hwloc_obj_type_t const obj_type =
        type_idx == 0 ? HWLOC_OBJ_NODE : HWLOC_OBJ_MACHINE;
    int const node_depth = hwloc_get_type_depth(topology, obj_type);
    if (node_depth >= 0) {
      int const num_nodes = hwloc_get_nbobjs_by_depth(topology, node_depth);
      if (not(num_nodes > 0))
        CCTK_VERROR("Could not determine number of NUMA/machine nodes at "
                    "depth %d (got %d)",
                    node_depth, num_nodes);
      int const num_memory_levels = num_nodes == 1 ? 1 : 2;
      int const node_num = 0; // just look at first node
      hwloc_obj_t const node_obj =
          hwloc_get_obj_by_depth(topology, node_depth, node_num);
      if (not node_obj)
        CCTK_VERROR("hwloc returned no object for node depth %d", node_depth);
      if (not(node_obj->type == obj_type))
        CCTK_VERROR("hwloc object at node depth %d has unexpected type",
                    node_depth);
#if (HWLOC_API_VERSION < 0x00020000)
      hwloc_obj_memory_s const &memory_attr = node_obj->memory;
#else
      hwloc_obj_attr_u::hwloc_numanode_attr_s const &memory_attr =
          node_obj->attr->numanode;
#endif
      for (int memory_level = 0; memory_level < num_memory_levels;
           ++memory_level) {
        int const num_memories = memory_level == 0 ? 1 : num_nodes;
        char const *const name =
            memory_level == 0 ? "local memory" : "global memory";
        ptrdiff_t const memory_size = memory_attr.local_memory;
        if (memory_size > 0) {
          ptrdiff_t page_size;
          if (memory_attr.page_types_len > 0) {
            // Use smallest page size
            page_size = memory_attr.page_types[0].size;
          } else {
            page_size = 0;
          }
          printf("  Memory has type \"%s\" depth %d\n"
                 "    size %td pagesize %td, for %d PUs\n",
                 memory_level == 0 ? "local" : "global", node_depth,
                 memory_size * num_memories, page_size,
                 num_pus * num_memories / num_nodes);
          cache_info_t new_cache_info;
          new_cache_info.name = name;
          new_cache_info.type = memory_level == 0 ? mem_local : mem_global;
          new_cache_info.size = memory_size * num_memories;
          new_cache_info.linesize = page_size;
          new_cache_info.stride = 0;
          new_cache_info.num_pus = num_pus * num_memories / num_nodes; // TODO
          cache_info.push_back(new_cache_info);
        }
      }
    }
  }
}
} // namespace

extern "C" CCTK_INT ST_GetNumSMTThreads() {
  return node_topology_info->num_smt_threads;
}

extern "C" CCTK_INT ST_GetMaxSMTThreads() {
  return node_topology_info->max_smt_threads;
}

extern "C" CCTK_INT ST_GetCacheInfo(CCTK_POINTER_TO_CONST *restrict const names,
                                    CCTK_INT *restrict const types,
                                    CCTK_POINTER_TO_CONST *restrict const sizes,
                                    CCTK_INT *restrict const linesizes,
                                    CCTK_INT *restrict const strides,
                                    CCTK_INT *restrict const num_puss,
                                    CCTK_INT const max_num_cache_levels) {
  vector<node_topology_info_t::cache_info_t> const &cache_info =
      node_topology_info->cache_info;

  int const num_levels = min(int(max_num_cache_levels), int(cache_info.size()));
  for (int level = 0; level < num_levels; ++level) {
    if (names)
      names[level] = cache_info[level].name;
    if (types)
      types[level] = cache_info[level].type;
    if (sizes)
      sizes[level] = (void *)(cache_info[level].size);
    if (linesizes)
      linesizes[level] = cache_info[level].linesize;
    if (strides)
      strides[level] = cache_info[level].stride;
    if (num_puss)
      num_puss[level] = cache_info[level].num_pus;
  }
  return cache_info.size();
}

extern "C" CCTK_INT
ST_GetMPIProcessInfo(CCTK_INT *restrict const mpi_num_procs,
                     CCTK_INT *restrict const mpi_proc_num,
                     CCTK_INT *restrict const mpi_num_hosts,
                     CCTK_INT *restrict const mpi_host_num,
                     CCTK_INT *restrict const mpi_num_procs_on_host,
                     CCTK_INT *restrict const mpi_proc_num_on_host) {
  *mpi_num_procs = mpi_host_mapping->mpi_num_procs;
  *mpi_proc_num = mpi_host_mapping->mpi_proc_num;
  *mpi_num_hosts = mpi_host_mapping->mpi_num_hosts;
  *mpi_host_num = mpi_host_mapping->mpi_host_num;
  *mpi_num_procs_on_host = mpi_host_mapping->mpi_num_procs_on_host;
  *mpi_proc_num_on_host = mpi_host_mapping->mpi_proc_num_on_host;
  return 0;
}

namespace {

// Query hwloc's cpukinds API to find the highest-efficiency CPU kind's
// PU set, but only if hwloc can reliably identify one: there must be
// more than one kind, and every kind's efficiency must be known
// (hwloc reports -1 for all kinds, in no particular order, if it
// cannot rank them). Returns true and fills perf_cpuset on success.
bool detect_performance_cores(hwloc_topology_t topology,
                              hwloc_bitmap_t perf_cpuset) {
#if (HWLOC_API_VERSION >= 0x00020400)
  int const num_kinds = hwloc_cpukinds_get_nr(topology, 0);
  if (num_kinds <= 1)
    return false;

  int best_kind = -1;
  int best_efficiency = -1;
  for (int kind = 0; kind < num_kinds; ++kind) {
    int efficiency;
    int const ierr = hwloc_cpukinds_get_info(topology, kind, NULL, &efficiency,
                                             NULL, NULL, 0);
    if (ierr or efficiency < 0)
      return false;
    if (efficiency > best_efficiency) {
      best_efficiency = efficiency;
      best_kind = kind;
    }
  }

  int const ierr = hwloc_cpukinds_get_info(topology, best_kind, perf_cpuset,
                                           &best_efficiency, NULL, NULL, 0);
  return not ierr;
#else
  return false;
#endif
}

// On a heterogeneous (hybrid) CPU, restrict the topology to only the
// highest-efficiency CPU kind (the "performance" cores), if hwloc can
// reliably identify one. Returns true if a restriction was applied.
//
// All nodes used in a run are assumed identical, so only the global
// root process actually queries hwloc's cpukinds API; the result (or
// the fact that none could be reliably identified) is broadcast to
// every other rank, rather than having every process on every node
// redundantly repeat the same query.
bool restrict_to_performance_cores(hwloc_topology_t topology,
                                   char const *const use_performance_cores_only,
                                   mpi_host_mapping_t const &host_mapping) {
  if (not CCTK_EQUALS(use_performance_cores_only, "yes") and
      not CCTK_EQUALS(use_performance_cores_only, "no") and
      not CCTK_EQUALS(use_performance_cores_only, "auto"))
    CCTK_VERROR("Illegal value \"%s\" for environment variable "
                "CACTUS_USE_PERFORMANCE_CORES_ONLY (must be \"yes\", "
                "\"no\", or \"auto\")",
                use_performance_cores_only);

  if (CCTK_EQUALS(use_performance_cores_only, "no"))
    return false;

  bool const force = CCTK_EQUALS(use_performance_cores_only, "yes");
  int const root = 0;

  // status: 0 = do not restrict; 1 = restrict to the cpuset that
  // follows in cpuset_buf; 2 = restriction was mandatory, but hwloc
  // could not reliably identify performance cores
  int status = 0;
  char cpuset_buf[2048] = "";

  if (host_mapping.mpi_proc_num == root) {
    hwloc_bitmap_t const perf_cpuset = hwloc_bitmap_alloc();
    if (perf_cpuset) {
      if (detect_performance_cores(topology, perf_cpuset)) {
        status = 1;
        hwloc_bitmap_list_snprintf(cpuset_buf, sizeof cpuset_buf, perf_cpuset);
      }
      hwloc_bitmap_free(perf_cpuset);
    }
    if (status == 0 and force)
      status = 2;
  }

#ifdef HAVE_CAPABILITY_MPI
  MPI_Bcast(&status, 1, MPI_INT, root, MPI_COMM_WORLD);
  MPI_Bcast(cpuset_buf, sizeof cpuset_buf, MPI_CHAR, root, MPI_COMM_WORLD);
#endif

  if (status == 2)
    CCTK_ERROR("use_performance_cores_only is set to \"yes\", but hwloc "
               "could not reliably identify performance cores (checked "
               "once, on the root process, since all nodes are assumed "
               "identical)");
  if (status == 0)
    return false;

  hwloc_bitmap_t const perf_cpuset = hwloc_bitmap_alloc();
  if (not perf_cpuset) {
    if (force)
      CCTK_ERROR("use_performance_cores_only is set to \"yes\", but could "
                 "not allocate a bitmap to identify performance cores");
    return false;
  }
  hwloc_bitmap_list_sscanf(perf_cpuset, cpuset_buf);
  // Intersect with whatever this process is already restricted to (e.g.
  // by a job scheduler), in case that restriction excludes all
  // performance cores
  hwloc_bitmap_and(perf_cpuset, perf_cpuset,
                   hwloc_topology_get_topology_cpuset(topology));
  if (hwloc_bitmap_iszero(perf_cpuset)) {
    hwloc_bitmap_free(perf_cpuset);
    if (force)
      CCTK_ERROR("use_performance_cores_only is set to \"yes\", but this "
                 "process's allowed CPU set does not include any "
                 "performance cores");
    return false;
  }
  CCTK_INFO("Detected a heterogeneous (hybrid) CPU; restricting to "
             "performance cores only");
  hwloc_topology_restrict(topology, perf_cpuset, 0);
  hwloc_bitmap_free(perf_cpuset);
  return true;
}

} // namespace

extern "C" int ST_system_topology() {
  DECLARE_CCTK_PARAMETERS;

  // Check OpenMP consistency
  check_openmp();

  // Determine MPI (host/process) mapping
  mpi_host_mapping = new mpi_host_mapping_t;
  mpi_host_mapping->load();

  // Determine node topology
  hwloc_topology_t topology;
  hwloc_topology_init(&topology);
  hwloc_topology_load(topology);

  // Determine whether this process is already confined to a subset of
  // the node's PUs -- e.g. by a job scheduler's cpuset/cgroup, or by
  // the MPI launcher's own affinity settings (srun --cpu-bind,
  // mpirun --bind-to, etc.). If so, restrict our view of the topology
  // to that subset, so all subsequent core/PU enumeration, binding,
  // and cache/memory queries only consider PUs we can actually use.
  bool topology_is_restricted = false;
  {
    hwloc_cpuset_t const proc_cpuset = hwloc_bitmap_alloc();
    if (proc_cpuset) {
      int const ierr =
          hwloc_get_cpubind(topology, proc_cpuset, HWLOC_CPUBIND_PROCESS);
      if (not ierr) {
        hwloc_const_cpuset_t const machine_cpuset =
            hwloc_topology_get_topology_cpuset(topology);
        if (not hwloc_bitmap_isequal(proc_cpuset, machine_cpuset)) {
          topology_is_restricted = true;
          CCTK_INFO("This process is confined to a subset of the node's "
                     "PUs (e.g. by a job scheduler or launcher); "
                     "restricting topology accordingly");
          hwloc_topology_restrict(topology, proc_cpuset, 0);
        }
      }
      hwloc_bitmap_free(proc_cpuset);
    }
  }

  // Optionally restrict further to only the performance cores of a
  // heterogeneous (hybrid) CPU. Whether to do so is a property of the
  // machine, not of the simulation, so it is controlled by an
  // environment variable (following the convention of e.g.
  // CACTUS_NUM_THREADS) rather than by a steerable parameter.
  char const *const CACTUS_USE_PERFORMANCE_CORES_ONLY =
      getenv("CACTUS_USE_PERFORMANCE_CORES_ONLY");
  restrict_to_performance_cores(
      topology,
      CACTUS_USE_PERFORMANCE_CORES_ONLY ? CACTUS_USE_PERFORMANCE_CORES_ONLY
                                        : "auto",
      *mpi_host_mapping);

  output_support(topology);
  output_objects(topology);
  output_bindings(topology, *mpi_host_mapping);
  output_distances(topology);

  bool do_set_thread_bindings;
  if (CCTK_EQUALS(set_thread_bindings, "yes")) {
    do_set_thread_bindings = true;
  } else if (CCTK_EQUALS(set_thread_bindings, "no")) {
    do_set_thread_bindings = false;
  } else if (CCTK_EQUALS(set_thread_bindings, "auto")) {
#ifdef HWLOC_PER_PROCESS
    // The calculation is probably wrong, so don't act on it
    do_set_thread_bindings = false;
#else
    // If a job scheduler or launcher has already confined this process
    // to a subset of the node, our whole-node binding arithmetic does
    // not apply -- respect the existing binding instead of fighting it.
    do_set_thread_bindings = not topology_is_restricted;
#endif
  } else {
    CCTK_ERROR("internal error");
  }
  if (do_set_thread_bindings) {
    set_bindings(topology, *mpi_host_mapping);
    output_bindings(topology, *mpi_host_mapping);
  }

  // Capture some information for later use
  node_topology_info = new node_topology_info_t;
  node_topology_info->load(topology, *mpi_host_mapping);

  hwloc_topology_destroy(topology);

  return 0;
}
