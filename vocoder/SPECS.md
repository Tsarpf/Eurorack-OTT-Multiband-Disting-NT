"MIMXRT1062" is simply NXP's official alphanumeric part number for ordering and marking the silicon. In their naming convention, "M" stands for Microcontroller, "IMX" is the i.MX product family, "RT" denotes the Real-Time crossover series, and "1062" is the specific model. 

Here is the revised documentation with the exact hardware specifications included:

***

### Expert Sleepers Disting NT: DSP Architecture & Optimization Guide

#### Processor Overview
The Disting NT is powered by the **NXP i.MX RT1062 (Part No. MIMXRT1062)** crossover microcontroller. This specific chip houses an **ARM Cortex-M7** core running at 600 MHz with a hardware Floating-Point Unit (FPv5-D16). It does **not** support NEON SIMD instructions, which are exclusive to the Cortex-A series. All firmware/plugin compilation for this target relies on the `arm-none-eabi-gcc` cross-compiler toolchain.

#### Relevant Integer DSP Accelerations
While lacking NEON, the Cortex-M7 core features the ARMv7E-M DSP instruction set. This provides dedicated hardware extensions engineered specifically for fixed-point integer math, making it ideal for a high-channel-count bandpass filter bank like a 40-band vocoder.

**1. 16-Bit Integer SIMD (Packed Data)**
To achieve 2-way parallel processing, two 16-bit fractional numbers (Q15 format) can be packed into a single 32-bit register.
* **`SMLAD` (Signed Multiply Accumulate Dual):** Multiplies two sets of 16-bit values and adds both products to a 32-bit accumulator in a single clock cycle. This is the primary engine for executing a 40-band IIR/FIR filter bank efficiently.
* **`SMUAD` (Signed Multiply Add Dual):** Multiplies two 16-bit values and adds the products together without an accumulator.

**2. Hardware Saturation Arithmetic**
Preventing audio overflow clipping typically requires expensive conditional branching. The NXP chip eliminates this pipeline overhead with dedicated hardware instructions.
* **`QADD` / `QSUB`:** Saturating 32-bit integer addition and subtraction.
* **`SSAT`:** Single-cycle signed saturation to an arbitrary bit depth.

**3. Single-Cycle 32-Bit MACs**
If 16-bit precision introduces too much quantization noise or limit-cycle oscillations for the bandpass filters, 32-bit (Q31) math can be used without massive performance penalties. The processor includes a hardware multiplier that executes 32x32-bit into 64-bit multiply-accumulate operations (`SMLAL`) in 1 to 2 clock cycles.

#### Implementation Recommendation: CMSIS-DSP
Because the Disting NT SDK compiles via the ARM toolchain, the project has native access to the ARM CMSIS-DSP library. Custom C++ loops should be avoided for the bandpass banks. Instead, rely on the library's biquad cascade functions:
* **16-bit implementation:** `arm_biquad_cascade_df1_fast_q15`
* **32-bit implementation:** `arm_biquad_cascade_df1_q31`

These are hand-optimized in assembly to pipeline `SMLAD` and `QADD` instructions to their maximum theoretical throughput on the MIMXRT1062 architecture.


# Next steps
see `vocoder/implementation_plan.md.resolved`