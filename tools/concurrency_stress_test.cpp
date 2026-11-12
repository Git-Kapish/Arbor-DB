// ./bin/concurrency_stress_test --transactions 50 --iterations 10000
//   --transactions N : number of concurrent worker threads (each runs its own stream of transactions)
//   --iterations   M : total transactions executed across all workers
// Exits 0 only if no lost update, no dirty/inconsistent read, and the bank invariant all held.
#include <cstring>
#include <cstdlib>

#include "stress_workloads.h"

using namespace arbordb;

int main(int argc, char** argv) {
    int threads = 50;
    long iterations = 10000;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--transactions")) threads = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--iterations")) iterations = std::atol(argv[i + 1]);
        else { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    char dir[] = "/tmp/arbor_stress_XXXXXX";
    if (!mkdtemp(dir)) return 2;
    std::printf("strict 2PL stress: %d concurrent workers, %ld transactions\n", threads, iterations);
    int rc;
    {
        StressEnv env(dir, 50, 1000);
        StressStats st;
        StressResult r = RunMixedStress(env, st, threads, iterations);
        std::printf("%s\n", r.report.c_str());
        std::printf("deadlocks detected by lock manager: %zu\n", env.cc.Locks().DeadlocksDetected());
        std::printf("%s\n", r.ok ? "PASS: no lost updates, no dirty reads, invariant held" : "FAIL");
        rc = r.ok ? 0 : 1;
    }
    std::string cmd = std::string("rm -rf ") + dir;
    (void)!system(cmd.c_str());
    return rc;
}
