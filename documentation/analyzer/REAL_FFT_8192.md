# Experimental 8192-point real FFT

Each window is packed as `z[n] = window[2n] * x[2n] + i * window[2n+1] * x[2n+1]`. An N/2 complex FFT is reconstructed into N/2+1 unique real-spectrum bins, including DC and Nyquist with single-sided endpoint normalization. Periodic Hann, Hamming and Blackman retain their existing coherent-gain calibration.

Dynamic threadgroup memory is 4*N bytes: 4, 8, 16 and 32 KiB for N=1024, 2048, 4096 and 8192. Renderers reject unsupported sizes or devices without sufficient threadgroup memory. Spectrum smoothing/peak arrays are sized to the selected bin count.

For stereo power, the two channels run sequentially within the same threadgroup, reusing scratch. The first channel's amplitudes are retained in the spectrum output buffer or a per-slice STFT device scratch buffer; the second transform produces `sqrt((L^2 + R^2)/2)`. A group/device memory barrier precedes scratch reuse. Opposite-phase channels therefore remain visible. Mid and Side transform the PCM pair before real packing.

STFT scratch holds at most 16 simultaneous slices, matching the bounded timeline catch-up; groups write disjoint ranges. Existing ring textures, history duration, hop scheduling, waterfall rendering and driver geometry remain unchanged. The 48-kHz hop stays 512 frames, giving 93.75% overlap for N=8192.

At 48 kHz, N=8192 spans 170.67 ms with a center offset of 85.33 ms and frequency-bin spacing of 5.859375 Hz. The 12288-frame active ring leaves 4096 frames (85.33 ms) beyond this window; the timeline still reserves 40 ms for asynchronous execution. This is headroom, not a guaranteed deadline under scheduling stalls.

Buffer-only tests check every supported size and window, opposite-phase stereo, DC/Nyquist, independently computed DFT bins through ring wrap, full-bin smoothing/peak output, and batched STFT history. No audio is played during tests. Runtime performance and appearance remain experimental until verified in the app.
