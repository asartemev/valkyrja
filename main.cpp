#include <hwcpipe/hwcpipe.hpp>
#include <iostream>
#include <map>
#include <string>
#include <chrono>
#include <thread>
#include <csignal>
#include <iomanip>
#include <set>
#include <vector>
#include <system_error>
#include <sstream>

using namespace std;

volatile sig_atomic_t keep_running = 1;
void signal_handler(int) { keep_running = 0; }

// Helper function to inject ANSI colors safely into string outputs
string colorize(const string& text, const string& ansi_color) {
    return ansi_color + text + "\033[0m"; // \033[0m Resets terminal color back to default
}

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);

    // Parse runtime sampling refresh interval argument
    int seconds = (argc > 1) ? stoi(string(argv[1])) : 1;

    hwcpipe::find_gpus gpus;
    auto it = gpus.begin();
    if (it == gpus.end()) {
        cerr << "Error: No compatible Arm Mali GPU found on this platform. Try to run with sudo." << endl;
        return 1;
    }

    // Copy the object to avoid dangling pointer
    hwcpipe::gpu gpu_dev = *it;
    hwcpipe::sampler_config config(gpu_dev);

    // Mali-G610 (Valhall v3 CSF) IDs
    // check /usr/local/include/hwcpipe/hwcpipe_counter.h to add more
    map<hwcpipe_counter, string> dashboard = {
        {MaliAnyUtil,               "GPU_UTILIZATION           "},
        {MaliCoreUtil,              "SHADER_CORE_UTILIZATION   "},
        {MaliFragUtil,              "FRAGMENT_PHASE_UTILIZATION"},
        {MaliCompQueueActiveCy,     "COMPUTE_ACTIVE            "},
        {MaliCoreActiveCy,          "EXEC_CORE_ACTIVE          "},
        {MaliALUUtil,               "ALU_UTILIZATION           "},
        {MaliEngStarveCy,           "ALU_STALL_BUBBLE          "},
        {MaliVarInstr,              "VARYING_INSTRS            "},
        {MaliLSUtil,                "LS_STALL                  "},
        {MaliL2CacheRdLookup,       "L2_READ_LOOKUP            "},
        {MaliL2CacheRdMissRate,     "L2_CACHE_MISS             "},
        {MaliL2CacheWrMissRate,     "L2_WRITE_MISS             "},
        {MaliExtBusRdStallCy,       "BUS_READ_STALL            "},
        {MaliExtBusRdBy,            "BUS_READ_SPEED            "},
        {MaliExtBusWrBy,            "BUS_WRITE_SPEED           "},
        {MaliEngDivergedInstrRate,  "BRANCH_DIVERGE_R          "},
        {MaliTilerActiveCy,         "TILER_ACTIVE              "},
        {MaliTexUtil,               "TEXTURE_UTILIZATION       "}
    };

    // Global persistence caches for UI and absolute values
    map<hwcpipe_counter, string> live_values;
    map<hwcpipe_counter, double> processed_numeric_values;

    for (const auto& [counter, name] : dashboard) {
        live_values[counter] = "0 (Gathering...)";
        processed_numeric_values[counter] = 0.0;
    }

    // Historical caches for tracking continuous data deltas over time
    map<hwcpipe_counter, double> previous_raw_values;
    map<hwcpipe_counter, chrono::steady_clock::time_point> previous_timestamps;

    // Explicitly bundle counters into 5 safe hardware slices
    vector<set<hwcpipe_counter>> bundle_sets = {
        {MaliCoreActiveCy, MaliEngStarveCy, MaliEngDivergedInstrRate},          // Bundle 0: Starvation + Core Analytics
        {MaliAnyUtil, MaliCompQueueActiveCy, MaliCoreUtil, MaliTilerActiveCy},  // Bundle 1: High level activity
        {MaliALUUtil, MaliFragUtil, MaliLSUtil, MaliVarInstr, MaliTexUtil},     // Bundle 2: Execution Stalls
        {MaliL2CacheRdLookup, MaliL2CacheRdMissRate, MaliL2CacheWrMissRate},    // Bundle 3: Caching
        {MaliExtBusRdBy, MaliExtBusWrBy, MaliExtBusRdStallCy}                   // Bundle 4: Ext Bus Memory
    };

    // Instantiate 5 distinct configuration profiles
    vector<hwcpipe::sampler_config> configs;
    for (const auto& counter_set : bundle_sets) {
        hwcpipe::sampler_config config(gpu_dev);
        for (const auto& counter : counter_set) {
            if (!config.add_counter(counter))
                cerr << "Can't add counter " << counter << endl;
        }
        configs.push_back(config);
    }

    // Lifecycle-persistent history variables for the Starvation calculation
    string stable_starve_rate_str = "0 (Gathering...)";
    double global_prev_core = 0.0;
    double global_prev_starve = 0.0;
    double raw_starve_ratio = 0.0; // Global value holder for color coding

    cout << "\n--- Starting Multiplexed Performance Monitor (Ctrl+C to exit) ---\n" << endl;

    size_t config_turn = 0;

    while (keep_running) {
        // Instantiate the sampler template dynamically for the active configuration bundle
        hwcpipe::sampler<> sampler(configs[config_turn]);

        if (!sampler.start_sampling()) {
            this_thread::sleep_for(chrono::milliseconds(300));

            error_code sample_err = sampler.sample_now();
            auto exact_sample_time = chrono::steady_clock::now();

            if (!sample_err) {
                for (auto counter_enum : bundle_sets[config_turn]) {
                    hwcpipe::counter_sample sample{};
                    if (!sampler.get_counter_value(counter_enum, sample)) {

                        double current_raw_val = 0.0;
                        if (sample.type == hwcpipe::counter_sample::type::float64) {
                            current_raw_val = sample.value.float64;
                        } else if (sample.type == hwcpipe::counter_sample::type::uint64) {
                            current_raw_val = static_cast<double>(sample.value.uint64);
                        }

                        processed_numeric_values[counter_enum] = current_raw_val;

                        // --- MEMORY BUS THROUGHPUT CALCULATION ---
                        if (counter_enum == MaliExtBusRdBy || counter_enum == MaliExtBusWrBy) {
                            if (previous_raw_values.count(counter_enum) > 0 && previous_timestamps.count(counter_enum) > 0) {
                                chrono::duration<double> elapsed = exact_sample_time - previous_timestamps[counter_enum];
                                double seconds_elapsed = elapsed.count();

                                double diff = 0.0;
                                if (current_raw_val >= previous_raw_values[counter_enum]) {
                                    diff = current_raw_val - previous_raw_values[counter_enum];
                                } else {
                                    diff = current_raw_val;
                                }

                                if (seconds_elapsed > 0.0) {
                                    double mbs = (diff / (1024.0 * 1024.0)) / seconds_elapsed;
                                    if (mbs >= 0.0 && mbs < 35000.0) {
                                        stringstream ss;
                                        ss << fixed << setprecision(2) << mbs << " MB/s";
                                        live_values[counter_enum] = ss.str();
                                    }
                                }
                            } else {
                                live_values[counter_enum] = "Calculating...";
                            }
                            previous_raw_values[counter_enum] = current_raw_val;
                            previous_timestamps[counter_enum] = exact_sample_time;
                        }
                        else if (counter_enum == MaliCoreActiveCy || counter_enum == MaliEngStarveCy ||
                                 counter_enum == MaliExtBusRdStallCy || counter_enum == MaliTilerActiveCy) {
                            stringstream ss; ss << fixed << setprecision(0) << current_raw_val;
                            live_values[counter_enum] = ss.str();
                        }
                        else if (counter_enum == MaliAnyUtil ||
                                 counter_enum == MaliFragUtil ||
                                 counter_enum == MaliALUUtil ||
                                 counter_enum == MaliLSUtil ||
                                 counter_enum == MaliL2CacheRdMissRate ||
                                 counter_enum == MaliL2CacheWrMissRate ||
                                 counter_enum == MaliEngDivergedInstrRate ||
                                 counter_enum == MaliCoreUtil ||
                                 counter_enum == MaliTexUtil) {

                            stringstream ss;
                            ss << fixed << setprecision(2) << current_raw_val << " %";
                            live_values[counter_enum] = ss.str();
                        }
                        else {
                            stringstream ss;
                            ss << fixed << setprecision(0) << current_raw_val;
                            live_values[counter_enum] = ss.str();
                        }
                    }
                }

                // --- GLOBAL DIFFERENTIAL ENGAGEMENT FOR FRONTEND STARVE RATE ---
                if (bundle_sets[config_turn].count(MaliCoreActiveCy) > 0 && bundle_sets[config_turn].count(MaliEngStarveCy) > 0) {
                    double current_core_accum = processed_numeric_values[MaliCoreActiveCy];
                    double current_starve_accum = processed_numeric_values[MaliEngStarveCy];

                    if (global_prev_core > 0.0 && global_prev_starve > 0.0) {
                        double core_delta = current_core_accum - global_prev_core;
                        double starve_delta = current_starve_accum - global_prev_starve;

                        if (core_delta > 0.0 && starve_delta >= 0.0) {
                            raw_starve_ratio = (starve_delta / core_delta);
                            double percentage_val = raw_starve_ratio * 100.0;

                            stringstream ss;
                            if (percentage_val > 100.0) {
                                ss << fixed << setprecision(2) << raw_starve_ratio << "x Intensity (SATURATED)";
                            } else {
                                ss << fixed << setprecision(2) << percentage_val << " %";
                            }
                            stable_starve_rate_str = ss.str();
                        }
                    }

                    if (current_core_accum > 0.0) global_prev_core = current_core_accum;
                    if (current_starve_accum > 0.0) global_prev_starve = current_starve_accum;
                }
            }
        }

        // Render dashboard terminal text
        cout << "\033[2J\033[1;1H";
        cout << "=== RK3588 Mali-G610 GPU Performance Dashboard ===" << endl;
        cout << "Multiplex Slot Interval Delay Pattern: " << seconds << "s" << endl;
        cout << setw(25) << left << "Metric Layout" << " | " << "Value" << endl;
        cout << string(45, '-') << endl;
        for (const auto& [counter_enum, name] : dashboard)
            cout << setw(25) << left << name << " : " << live_values[counter_enum] << endl;
        cout << string(45, '-') << endl;

        // --- TOP-DOWN ANALYSIS MATH ---
        double total_cycles = processed_numeric_values[MaliCoreActiveCy];
        double gpu_util = processed_numeric_values[MaliAnyUtil];
        double core_util = processed_numeric_values[MaliCoreUtil];
        double texture_util = processed_numeric_values[MaliTexUtil];
        double alu_util = processed_numeric_values[MaliALUUtil];
        double fragment_util = processed_numeric_values[MaliALUUtil];
        double tiler_cycles = processed_numeric_values[MaliTilerActiveCy];
        double devergence_freq = processed_numeric_values[MaliEngDivergedInstrRate];
        double ls_stall = processed_numeric_values[MaliLSUtil];
        double l2_miss = processed_numeric_values[MaliL2CacheRdMissRate];
        double rd_stalls = processed_numeric_values[MaliExtBusRdStallCy];

        // ALU LOAD DERIVED
        string alu_load_str = "0.00 %";
        double alu_load = 0.0;
        if (gpu_util > 0.001) {
            alu_load = (alu_util / gpu_util) * 100.0;
            if (alu_load > 100.0) alu_load = 100.0;
            stringstream ss; ss << fixed << setprecision(2) << alu_load << " %";
            alu_load_str = ss.str();
        }

        // TILER BOUND RATE
        string geom_util_str = "0.00 %";
        double geom_util = 0.0;
        if (total_cycles > 0.001) {
            geom_util = (tiler_cycles / total_cycles) * 100.0;
            if (geom_util < 0.0) geom_util = 0.0;
            stringstream ss; ss << fixed << setprecision(2) << geom_util << " %";
            geom_util_str = ss.str();
        }

        // WARP DIVERGENCE PENALTY
        double bad_spec_penalty = alu_util * (devergence_freq / 100.0);
        if (bad_spec_penalty < 0.0) bad_spec_penalty = 0.0;
        stringstream ss_bad_spec; ss_bad_spec << fixed << setprecision(2) << bad_spec_penalty << " %";

        // L2 CACHE HIT RATE
        double l2_hit_rate = 100.0 - l2_miss;
        if (l2_hit_rate < 0.0) l2_hit_rate = 0.0;
        stringstream ss_l2; ss_l2 << fixed << setprecision(2) << l2_hit_rate << " %";

        // MEMORY LOAD BOUND
        double memory_load_bound = ls_stall * (l2_miss / 100.0);
        if (memory_load_bound > 100.0) memory_load_bound = 100.0;
        stringstream ss_mlb; ss_mlb << fixed << setprecision(2) << memory_load_bound << " %";

        // MEMORY SYSTEM STALLS
        string l2_stall_rate_str = "0.00 %";
        double l2_stall_rate = 0.0;
        if (total_cycles > 0.001) {
            l2_stall_rate = (rd_stalls / total_cycles) * 100.0;
            if (l2_stall_rate < 0.0) l2_stall_rate = 0.0;
            if (l2_stall_rate > 100.0) l2_stall_rate = 100.0;
            stringstream ss; ss << fixed << setprecision(2) << l2_stall_rate << " %";
            l2_stall_rate_str = ss.str();
        }

        // --- ASSIGN ANSI DYNAMIC COLOR LOGIC TO THE DERIVED OUTPUT ENGINE ---
        string color_alu = "\033[33m"; // Yellow Default
        if (alu_load > 40.0) color_alu = "\033[32m"; // Green: Dense math processing
        else if (alu_load < 20.0 && gpu_util > 50.0) color_alu = "\033[31m"; // Red: GPU busy but running tiny math

        string color_tiler = "\033[32m"; // Green: Baseline geometry pipeline load
        if (geom_util > 75.0) color_tiler = "\033[31m"; // Red: Severe geometry pipeline bottleneck
        else if (geom_util > 50.0) color_tiler = "\033[33m"; // Yellow: Moderate geometry pipeline bottleneck

        string color_badspec = "\033[32m"; // Green Default
        if (devergence_freq > 35.0 && bad_spec_penalty > 15.0) color_badspec = "\033[31m"; // Red: ALU cycles are being burned processing empty, masked-out lane instructions
        else if (devergence_freq > 15.0) color_badspec = "\033[33m"; // Yellow: Rising execution slot waste due to warp divergence serialization

        string color_l2 = "\033[32m"; // Green Default
        if (l2_hit_rate < 75.0) color_l2 = "\033[31m"; // Red: Terribly un-aligned cache layout
        else if (l2_hit_rate < 85.0) color_l2 = "\033[33m"; // Yellow: Cache misses rising

        string color_l2stall = "\033[32m"; // Green: Baseline cache/bus state
        if (l2_stall_rate > 20.0) color_l2stall = "\033[31m"; // Red: Severe system RAM bandwidth starvation / high latency stalls
        else if (l2_stall_rate > 10.0) color_l2stall = "\033[33m"; // Yellow: Increasing memory bus pressure / elevated cache misses

        string color_starve = "\033[32m"; // Green Default
        if (raw_starve_ratio > 1.50) color_starve = "\033[31m"; // Red: Multi-pipeline instruction starvation is heavy
        else if (raw_starve_ratio > 0.80) color_starve = "\033[33m"; // Yellow: Medium bubble presence

        string color_mlb = "\033[32m"; // Green Default
        if (memory_load_bound > 15.0) color_mlb = "\033[31m"; // Red: Major system RAM bottlenecking
        else if (memory_load_bound > 5.0) color_mlb = "\033[33m"; // Yellow: Medium load constraints

        // Display Advanced Top-Down Block Output Layout with Dynamic Colors applied
        cout << "=== Advanced Top-Down Analysis Architectural Boundaries ===" << endl;
        cout << setw(25) << left << "ALU_LOAD_DERIVED" << " : " << colorize(alu_load_str, color_alu) << " (Retiring)" << endl;
        cout << setw(25) << left << "WARP_DIVERGENCE_PENALTY" << " : " << colorize(ss_bad_spec.str(), color_badspec) << " (Bad Speculation)" << endl;
        cout << setw(25) << left << "GEOM_LOAD" << " : " << colorize(geom_util_str, color_tiler) << " (Geometry Front-End Bound)" << endl;
        cout << setw(25) << left << "FRONTEND_STARVE_RATE" << " : " << colorize(stable_starve_rate_str, color_starve) << " (Instruction Front-End Bound)" << endl;
        cout << setw(25) << left << "MEMORY_LOAD_BOUND" << " : " << colorize(ss_mlb.str(), color_mlb) << " (Back-End Bound)" << endl;
        cout << setw(25) << left << "READ_STALL_IMPACT_RATE" << " : " << colorize(l2_stall_rate_str, color_l2stall) << " (Back-End Memory Latency Bound)" << endl;
        cout << setw(25) << left << "L2_CACHE_HIT_RATE" << " : " << colorize(ss_l2.str(), color_l2) << " (Data Locality)" << endl;
        cout << string(45, '-') << endl;

        // Final conclusions
        cout << "===                      Diagnosis                      ===" << endl;
        string color_diag = "\033[37m"; // Green Default
        string str_diag = "Mixed (bottleneck: unidentified)";
        if (core_util > 80.0) {
            if (alu_util < 20.0 && texture_util < 20.0 && ls_stall < 20.0) {
                color_diag = "\033[33m"; // Yellow
                str_diag = "Stalling (bottleneck: instruction dependencies or branch divergence)";
            } else if (alu_util > 80.0) {
                color_diag = "\033[32m"; // Green
                str_diag = "High Retiring (bottleneck: pure math)";
            } else if (texture_util > 80.0) {
                color_diag = "\033[32m"; // Green
                str_diag = "High Retiring (bottleneck: texture sampling execution)";
            } else if (l2_stall_rate < 5.0) {
                color_diag = "\033[32m"; // Green
                str_diag = "Compute Bound (bottleneck: math logic or texture sampling)";
            } else if (l2_stall_rate > 20.0) {
                color_diag = "\033[31m"; // Red
                str_diag = "Backend Memory Bound / Instruction Hidden Latency Failure";
            } else if (geom_util > 75.0 && fragment_util < 35.0) {
                color_diag = "\033[33m"; // Yellow
                str_diag = "Backend Core Bound, Vertex Processing (bottleneck: vertex shader execution saturation, cores are full of vertex math)";
            }
        } else if (core_util < 40.) {
            if (l2_stall_rate > 20.0) {
                color_diag = "\033[31m"; // Red
                str_diag = "Frontend Memory Latency/Bandwidth Bound (bottleneck: memory footprints)";
            } else if (geom_util > 75.0 && fragment_util < 35.0) {
                color_diag = "\033[33m"; // Yellow
                str_diag = "Vertex Front-End Latency / Structural Bound (bottleneck: fixed-function tiler saturation, the shader cores are starving)";
            }
        }
        if (bad_spec_penalty > 40.0 && alu_util > 80.0) {
            color_diag = "\033[31m"; // Red
            str_diag = "Bad Speculation (bottleneck: control-flow serialization)";
        }
        cout << setw(25) << left << "TMAM VERDICT" << " : " << colorize(str_diag, color_diag) << endl;

        // Bottom
        cout << string(45, '-') << endl;
        cout << "Press Ctrl+C to exit" << endl;

        // Shift tracking context pointer to the next bundle set for the upcoming frame cycle
        config_turn = (config_turn + 1) % configs.size();

        // Throttling step using parsed terminal argument logic
        this_thread::sleep_for(chrono::seconds(seconds));
    }
    return 0;
}
