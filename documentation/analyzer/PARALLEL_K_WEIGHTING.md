# Parallel K-weighting in ASFW

Reference: Haotian Zhai and Bernd-Peter Paris, **Parallel Cascaded Recursive Filtering on Multi-Core CPUs and GPUs**, arXiv:2607.23763v1 [eess.SP], 26 July 2026. https://arxiv.org/abs/2607.23763v1

The implementation applies the zero-state/homogeneous-response decomposition and inter-group state recurrence described in Section II-F, equations (24)–(25). It is an original adaptation, not a reproduction of the paper's complete GPU algorithm or its performance claims.

For each of the existing two biquads, contiguous blocks of 32 frames compute their local response with zero *output* history. The input history remains real: two preceding inputs come from the current packet or the committed cascade state. Each block publishes its terminal pair. A bounded affine prefix scan resolves the initial output pair for every block; a parallel correction applies the homogeneous response. The corrected shelf output feeds the high-pass section. A separate reduction assigns one 256-lane threadgroup to each 480-frame energy chunk, respecting the unfinished chunk carried from the preceding packet. Each lane visits at most two PCM frames. The final pass visits at most 13 compact summaries, writes completed chunks, and preserves unfinished energy/peaks and transactional filter/FIR histories; it does not walk the PCM packet.

Unlike the paper, this version uses short direct recurrences locally, a threadgroup scan instead of PH/cyclic reduction, and multiple compute passes instead of CUDA decoupled lookback. No inter-threadgroup spin waits are used. All passes remain in the original analysis command buffer.

The homogeneous state uses `(y, delta-y)` coordinates to reduce cancellation near the high-pass unit poles; the feed-forward term uses input differences and explicit fused multiply-adds. Persistent state retains the existing input/output history layout.

Transition-matrix powers are calculated once in Double from the existing Float-rounded filter coefficients. GPU calculations remain Float. Equivalent transfer functions do not imply bit-identical rounding; buffer-only regression tests compare the filtered samples and 10-ms energy chunks against an independent scalar cascade, including irregular packet sizes, near-DC input, noise, impulse/silence and ring wrap. Tests require less than 0.02 dB energy difference above the BS.1770 absolute gate, bound absolute energy error below that gate, and bound peak sample error to 3e-4 full scale and relative RMS error to 2e-4. These are numerical acceptance tolerances, not a claim of bitwise equivalence. No test audio is sent to hardware.

With Kernel timings enabled, K-weight GPU spans the first local pass through the final energy/state pass. It includes the dispatch gaps between those passes, not just their arithmetic. Profiling overhead and Apple GPU clock scaling remain part of the observed result.
