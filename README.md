# Mali-G610 Performance Monitor for RK3588

A lightweight, high-performance C++ command-line utility designed to monitor internal hardware counters of the **Arm Mali-G610 GPU** on **Rockchip RK3588/RK3588S** SoCs.

This application uses the classic **HWCPipe (v1.x)** API to implement an advanced Top-Down style microarchitectural analysis tailored to mobile GPU topologies, bypassing physical hardware limitations to monitor 14 raw parameters and 4 real-time derived indicators.

---

## Key Features

* **Explicit Slot Multiplexing**: Manually groups queries into 5 explicitly defined, sequentially rotated hardware bundles. This forces co-dependent metrics into the same physical sampling window and prevents register conflicts common to the Valhall v3 CSF architecture.
* **Multi-Core Pipeline Normalization**: Factors in a structural normalization divisor (accounting for the 4-core multi-pipeline topology) to anchor the absolute execution engine starvation rate.
* **Transient Outlier Filtering**: Implements a time-locked historical cache to verify memory bandwidth counter increments across multiplex cycles.
* **Precise Clock Synchronization**: Latches delta timestamps immediately via `std::chrono::steady_clock` to mitigate thread-sleep scheduling jitter.

---

## System Requirements & Prerequisites

### 1. Hardware
* Rockchip RK3588 or RK3588S SoC (e.g., Orange Pi 5, Radxa Rock 5B, Indiedroid Nova, Khadas Edge2).

### 2. Software & Libraries
* Linux kernel running the proprietary Arm Mali kernel-space driver (`mali_kbase`).
* Installed `hwcpipe` v1.x headers and linked shared libraries.
* GCC/G++ supporting C++17 or higher.

### 3. Driver Permissions (Critical)
By default, the Linux kernel restricts raw performance event queries for unprivileged users. Run this on your target board to allow counter sampling:
```bash
sudo sysctl -w kernel.perf_event_paranoid=1
```

---

## Compilation & Execution

Build the application using `g++`, ensuring you explicitly link `libhwcpipe`, `libdevice`, and `libpthread`:

```bash
make
```

### Running the Monitor
Run the application with `sudo` to ensure unobstructed access to the GPU hardware device nodes. You can optionally pass an integer argument to define the slot interval delay in seconds (defaults to `1`):

```bash
# Run with a 1-second refresh rate
sudo ./valkyrja

# Run with a 3-second refresh rate
sudo ./valkyrja 3
```

---

## 📊 Architectural Bounds Analysis (Top-Down Method)

Traditional CPU metrics like Branch Misprediction and Intel-style TMAM bounds do not map directly to GPU topologies due to hardware latency hiding and thread-group lockstep warps. This monitor translates Mali metrics into logical architectural equivalents:

             [ GPU_UTILIZATION (Top-Level Job Activity) ]
               /                                     \
[ PIXEL_ACTIVE (3D Graphics) ]                   [ COMPUTE_ACTIVE (Compute/ML) ]
               \                                     /
               [ EXEC_CORE_ACTIVE (Shader Execution) ]
              /                                      \
[ ALU_UTILIZATION ]                              [ LS_STALL / VARYING_INSTRS ]
            |      \                                   |                     \
[ ALU_LOAD_DERIVED ] [ FRONTEND_STARVE_RATE ]    [ MEMORY_LOAD_BOUND ] [ L2_CACHE_HIT_RATE ]
                          (Front-End Bound)        (Back-End Bound)


### Advanced Top-Down Metrics

#### 1. `FRONTEND_STARVE_RATE` (Front-End Bound Equivalent)
* **Formula**: `(ALU_STALL_BUBBLE / EXEC_CORE_ACTIVE)`
* **Interpretation**: Measures the ratio of active core runtime where execution lanes sat completely idle because instructions were unavailable. On multi-core Valhall v3 configurations (like the Mali-G610), raw pipeline event accumulators count across multiple internal parallel pipelines simultaneously. If parallel stalling events exceed baseline active clock ticks, the application dynamically reformats the percentage output into an **Intensity Saturation Coefficient Factor** (e.g., `2.12x Intensity (SATURATED)`). This coefficient acts as a direct multiplier tracking instruction-delivery bottlenecks—meaning a `2.12x` factor implies that arithmetic units encounter an average of 2.12 structural starvation bubbles for every single active core clock cycle.

#### 2. `MEMORY_LOAD_BOUND` (Back-End Bound Equivalent)
* **Formula**: `LS_STALL * (L2_CACHE_MISS / 100.0)`
* **Interpretation**: Evaluates severe memory engine constraints by weighing overall Load/Store execution stalls against L2 cache miss rates. High boundaries indicate that the execution core is choked waiting for external main system RAM.

#### 3. `ALU_LOAD_DERIVED`
* **Formula**: `(ALU_UTILIZATION / GPU_UTILIZATION) * 100`
* **Interpretation**: Isolates math operational density from idle clock ticks. If `ALU_UTILIZATION` is high but `GPU_UTILIZATION` is low, the workload runs demanding code in short bursts. If both metrics spike together, the pipeline is intensely math-bound.

#### 4. `L2_CACHE_HIT_RATE`
* **Formula**: `100.0 - L2_CACHE_MISS`
* **Interpretation**: Highlights data locality. Healthy pipelines maintain a hit rate `>85%`. Lower numbers imply poor memory access indexing patterns within custom OpenCL, Vulkan Compute, or ML inference shaders.

---

### Raw Counter Diagnostic Guide

* **`GPU_UTILIZATION`**: General frontend job manager availability indicator.
* **`PIXEL_ACTIVE`**: Active cycles inside fragment/pixel paths. Indicates traditional 3D rendering overhead.
* **`COMPUTE_ACTIVE`**: Compute queue transaction metrics. Tracks ML layer executions (OpenCL/Vulkan Compute) and geometric matrices.
* **`BRANCH_DIVERGE_R`**: Tracks the occurrence of branch divergence within 16-thread hardware warps. Higher rates trigger lane-masking overhead, forcing the GPU to execute both conditional paths sequentially instead of concurrently.
* **`VARYING_INSTRS`**: Total vector attribute/interpolator actions. **Note:** This metric safely reads `0` during OpenCL, machine learning, or pure compute tasks since compute threads entirely bypass traditional graphics primitive rasterization hardware units.
* **`BUS_READ_SPEED` / `BUS_WRITE_SPEED`**: Corrected instantaneous external main memory interface throughput formatted cleanly in **MB/s**. Use this to cross-reference system bandwidth bottlenecks on the shared RK3588 memory bus.

---

## Limitations & Troubleshooting

* **Initial Gathering Delay**: Due to the 5-way multiplex rotating loop layout, the metrics cache requires a baseline window of 5 sampling epochs (approx. 5–15 seconds depending on your chosen interval pass parameter) to initially gather, validate, and smooth out its differential throughput calculations.
* **Permissions Denied Error**: If the profile fails to initialize, verify you are launching with `sudo` and check that `kernel.perf_event_paranoid` has been set to `1`.
