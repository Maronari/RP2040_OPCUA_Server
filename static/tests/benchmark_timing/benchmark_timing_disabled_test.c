#include "services/benchmark_timing.h"

#include <assert.h>

int main(void)
{
    const uint64_t phases_us[BENCHMARK_TIMING_DIAG_PHASE_COUNT] = {0};

    benchmark_timing_note_boot_started();
    assert(benchmark_timing_init());
    benchmark_timing_set_control(NULL, NULL);
    benchmark_timing_set_output(NULL, NULL);
    benchmark_timing_sensor_task_process_control();
    assert(benchmark_timing_sensor_poll_begin() == NULL);
    benchmark_timing_sensor_poll_end();
    assert(!benchmark_timing_dump_if_ready());
    assert(benchmark_timing_diag_sample_begin() == 0u);
    assert(benchmark_timing_diag_phase_now_us() == 0u);
    benchmark_timing_diag_sample_set_phase_durations(phases_us);
    benchmark_timing_diag_sample_end(0u, true);
    benchmark_timing_diag_sample_set_opcua_task_stack_high_water(0u);
    return 0;
}
