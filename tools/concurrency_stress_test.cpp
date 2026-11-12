// ./bin/concurrency_stress_test --transactions 50 --iterations 10000
//   --transactions N : number of concurrent worker threads (each runs its own stream of transactions)
//   --iterations   M : total transactions executed across all workers
// --mvcc runs the same workload under snapshot isolation.
// Exits 0 only if no lost update, no dirty/inconsistent read, and the bank invariant all held.
#include <cstring>
#include <cstdlib>

#include "stress_workloads.h"

using namespace arbordb;

int main(int argc, char** argv) {
    int threads = 50;
    long iterations = 10000;
    bool mvcc = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--mvcc")) mvcc = true;
        else if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", argv[i]); return 2; }
        else if (!std::strcmp(argv[i], "--transactions")) threads = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--iterations")) iterations = std::atol(argv[++i]);
        else { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    char dir[] = "/tmp/arbor_stress_XXXXXX";
    if (!mkdtemp(dir)) return 2;
    std::printf("%s stress: %d concurrent workers, %ld transactions\n", mvcc ? "MVCC (snapshot isolation)" : "strict 2PL",
                threads, iterations);
    int rc;
    {
        StressEnv env(dir, 50, 1000, mvcc);
        StressStats st;
        StressResult r = RunMixedStress(env, st, threads, iterations);
        std::printf("%s\n", r.report.c_str());
        std::printf("deadlocks detected by lock manager: %zu\n", env.Locks().DeadlocksDetected());
        std::printf("%s\n", r.ok ? "PASS: no lost updates, no dirty reads, invariant held" : "FAIL");
        rc = r.ok ? 0 : 1;
    }
    std::string cmd = std::string("rm -rf ") + dir;
    (void)!system(cmd.c_str());
    return rc;
}
