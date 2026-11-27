# Research scenes: measures

speedo (extended) against the glTF research set's path traced references, written by scripts/researchcompare.ps1 with the captures beside it (<scene>.jpg, at the reference's size, JPEG quality 92). Captured and timed on macOS 27.2.1, Metal, Apple M4 Pro, hardware ray tracing; 2026-11-27, commit e558f310.

- **Blurred** and **sharp**: the mean |log2| difference of the two images' linear luminances, in stops (1 is twice or half the light), blurred over 8 pixels (the lighting's error rather than its noise) and per pixel. The scenes are ordered by the blurred one, the most unlike first.
- **Light**: log2 of speedo's mean linear luminance over the reference's, in stops: the light overall (+1 twice as bright, -1 half). The chart shows it as a share brighter or darker.
- **Mean**: the mean channel difference, in levels of 255.
- **Frame**: the client's median frame time at 2560x1440 with balanced upscaling after the lighting settled, and the costliest passes' gpu times, in ms.

| Scene | Blurred | Sharp | Light | Mean | Frame | Costliest passes |
|---|---:|---:|---:|---:|---:|---|
| glass-of-water | 0.738 | 0.930 | -0.242 | 25.80 | 108.41 | Main Transmissive 65.44, Reflection Trace 13.97, Main Visibility 3.04 |
| classroom2 | 0.656 | 0.835 | -0.187 | 21.57 | 55.41 | Reflection Trace 13.26, GI 9.85, Main Transmissive 6.62 |
| crown | 0.487 | 0.741 | -0.446 | 16.76 | 74.17 | Main Transmissive 35.04, GI 9.27, Reflection Trace 8.67 |
| cornell-caustic | 0.474 | 0.478 | +0.073 | 14.87 | 46.87 | Main Transmissive 27.73, GI 6.10, Main Visibility 1.94 |
| bmw-m6 | 0.387 | 0.431 | -0.216 | 7.63 | 70.24 | Main Transmissive 22.47, Reflection Trace 18.98, Main Visibility 6.59 |
| coffee-maker | 0.352 | 0.403 | +0.058 | 10.06 | 55.99 | Main Transmissive 25.96, Reflection Trace 5.16, Main Visibility 4.57 |
| breakfast-room | 0.314 | 0.526 | -0.039 | 8.26 | 24.39 | GI 5.34, Reflection Trace 3.11, Main Visibility 3.03 |
| living-room | 0.293 | 0.370 | -0.033 | 9.70 | 48.88 | Main Transmissive 11.82, Reflection Trace 11.67, GI 6.20 |
| classroom | 0.291 | 0.438 | -0.056 | 14.05 | 39.20 | Reflection Trace 17.51, Main Visibility 3.71, GI Near 3.58 |
| kitchen | 0.265 | 0.343 | -0.070 | 9.33 | 37.43 | Main Transmissive 6.88, Reflection Trace 6.35, GI 4.49 |
| bathroom | 0.261 | 0.406 | -0.071 | 12.19 | 39.64 | Reflection Trace 9.89, GI 6.00, Main Transmissive 5.11 |
| veach-mis | 0.243 | 0.267 | -0.087 | 6.14 | 21.13 | Reflection Trace 4.04, Main Visibility 3.22, GI 2.27 |
| modern-hall | 0.209 | 0.239 | -0.134 | 8.41 | 24.71 | Main Visibility 3.94, GI 3.38, Reflection Trace 3.31 |
| teapot-full | 0.206 | 0.250 | +0.026 | 9.92 | 54.23 | Main Transmissive 38.94, Main Visibility 3.56, Shadow Classify 2.42 |
| dragon | 0.202 | 0.352 | +0.070 | 2.07 | 14.38 | GI 6.34, GI Sides 1.29, Upscale Accumulate 1.09 |
| bathroom2 | 0.190 | 0.230 | -0.076 | 9.46 | 37.21 | Main Transmissive 7.49, Reflection Trace 5.87, GI 4.12 |
| lamp | 0.186 | 0.228 | +0.047 | 6.87 | 28.73 | Main Transmissive 4.60, Main Visibility 4.35, Reflection Trace 2.33 |
| pontiac-gto | 0.181 | 0.221 | -0.111 | 4.99 | 32.72 | Main Transmissive 13.59, Reflection Trace 4.05, Main Visibility 3.27 |
| spaceship | 0.181 | 0.219 | -0.196 | 3.70 | 46.87 | Main Transmissive 17.31, Reflection Trace 5.42, GI 4.58 |
| staircase | 0.174 | 0.246 | -0.009 | 5.80 | 39.93 | Reflection Trace 7.67, GI 7.65, Main Transmissive 6.41 |
| vintage-car | 0.171 | 0.218 | -0.152 | 5.33 | 75.14 | Main Transmissive 47.41, Reflection Trace 5.81, Main Visibility 4.77 |
| veach-bidir | 0.157 | 0.176 | -0.065 | 4.71 | 23.99 | GI 5.66, Main Transmissive 3.20, Main Visibility 2.15 |
| living-room-2 | 0.127 | 0.175 | -0.072 | 6.30 | 41.31 | Reflection Trace 10.79, GI 6.20, Main Visibility 4.26 |
| veach-ajar | 0.099 | 0.118 | -0.001 | 6.06 | 31.35 | Main Transmissive 14.66, Main Visibility 3.29, GI 2.37 |
| house | 0.094 | 0.167 | +0.002 | 5.70 | 18.87 | Main Transmissive 3.85, GI 2.29, Main Visibility 2.00 |
| teapot | 0.079 | 0.085 | -0.050 | 4.83 | 18.69 | Main Visibility 3.60, GI Sides 2.64, Shadow Classify 2.38 |
| material-testball | 0.070 | 0.083 | -0.039 | 3.21 | 18.24 | Main Visibility 3.34, Shadow Classify 2.54, Reflection Trace 1.86 |
| cornell-box | 0.057 | 0.066 | -0.008 | 1.14 | 16.86 | GI 7.12, Main Visibility 1.87, GI Sides 1.29 |
