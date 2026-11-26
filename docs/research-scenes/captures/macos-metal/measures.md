# Research scenes: measures

speedo (extended) against the glTF research set's path traced references, written by scripts/researchcompare.ps1 with the captures beside it (<scene>.jpg, at the reference's size, JPEG quality 92). Captured and timed on macOS 27.2.1, Metal, Apple M4 Pro, hardware ray tracing; 2026-11-26, commit 2b893c74.

- **Blurred** and **sharp**: the mean |log2| difference of the two images' linear luminances, in stops (1 is twice or half the light), blurred over 8 pixels (the lighting's error rather than its noise) and per pixel. The scenes are ordered by the blurred one, the most unlike first.
- **Light**: log2 of speedo's mean linear luminance over the reference's, in stops: the light overall (+1 twice as bright, -1 half). The chart shows it as a share brighter or darker.
- **Mean**: the mean channel difference, in levels of 255.
- **Frame**: the client's median frame time at 2560x1440 with balanced upscaling after the lighting settled, and the costliest passes' gpu times, in ms.

| Scene | Blurred | Sharp | Light | Mean | Frame | Costliest passes |
|---|---:|---:|---:|---:|---:|---|
| glass-of-water | 0.738 | 0.931 | -0.242 | 25.82 | 127.26 | Main Transmissive 83.49, Reflection Trace 15.71, GI 4.53 |
| classroom2 | 0.652 | 0.830 | -0.182 | 21.43 | 71.08 | Reflection Trace 19.06, GI 12.66, Main Transmissive 6.64 |
| crown | 0.488 | 0.743 | -0.447 | 16.77 | 95.95 | Main Transmissive 36.98, GI 16.82, Reflection Trace 11.85 |
| cornell-caustic | 0.471 | 0.474 | +0.074 | 14.76 | 55.23 | Main Transmissive 29.20, GI 12.89, Main Visibility 1.93 |
| bmw-m6 | 0.389 | 0.433 | -0.217 | 7.63 | 87.08 | Reflection Trace 27.64, Main Transmissive 22.49, Main Visibility 6.58 |
| coffee-maker | 0.353 | 0.404 | +0.059 | 10.08 | 66.50 | Main Transmissive 28.53, Reflection Trace 7.69, GI 5.67 |
| breakfast-room | 0.310 | 0.520 | -0.036 | 8.20 | 32.70 | GI 12.34, Reflection Trace 5.12, GI Sides 4.01 |
| classroom | 0.305 | 0.459 | -0.059 | 14.03 | 48.37 | Reflection Trace 19.38, GI Sides 7.77, GI 3.80 |
| living-room | 0.297 | 0.375 | -0.030 | 9.72 | 62.19 | Reflection Trace 17.36, Main Transmissive 11.90, GI 10.51 |
| bathroom | 0.259 | 0.404 | -0.075 | 12.06 | 55.42 | Reflection Trace 18.26, GI 8.00, GI Sides 6.68 |
| kitchen | 0.257 | 0.335 | -0.059 | 9.19 | 48.26 | GI 9.97, Reflection Trace 9.94, Main Transmissive 7.01 |
| veach-mis | 0.243 | 0.267 | -0.087 | 6.13 | 27.45 | Reflection Trace 6.81, GI 3.48, Main Visibility 3.21 |
| teapot-full | 0.206 | 0.250 | +0.026 | 9.92 | 59.70 | Main Transmissive 42.94, Main Visibility 3.57, Shadow Classify 2.42 |
| dragon | 0.201 | 0.353 | +0.070 | 2.07 | 19.35 | GI 11.27, GI Sides 2.41, Upscale Accumulate 1.09 |
| modern-hall | 0.200 | 0.230 | -0.129 | 8.10 | 32.66 | Reflection Trace 6.24, GI 5.79, Main Visibility 3.95 |
| lamp | 0.186 | 0.229 | +0.047 | 6.90 | 35.85 | GI Sides 4.77, Main Transmissive 4.63, Main Visibility 4.37 |
| spaceship | 0.182 | 0.221 | -0.196 | 3.73 | 56.77 | Main Transmissive 17.42, GI 8.74, Reflection Trace 8.37 |
| pontiac-gto | 0.178 | 0.219 | -0.121 | 4.91 | 39.82 | Main Transmissive 13.39, GI Sides 5.73, Reflection Trace 5.43 |
| staircase | 0.177 | 0.249 | -0.008 | 5.81 | 52.25 | Reflection Trace 13.50, GI 11.91, Main Transmissive 6.39 |
| bathroom2 | 0.172 | 0.212 | -0.062 | 8.84 | 48.99 | Reflection Trace 10.63, GI 8.54, Main Transmissive 7.52 |
| vintage-car | 0.171 | 0.218 | -0.151 | 5.33 | 87.27 | Main Transmissive 48.65, Reflection Trace 8.07, GI Sides 5.53 |
| veach-bidir | 0.171 | 0.189 | -0.077 | 5.08 | 28.85 | GI 15.01, Main Transmissive 3.23, Main Visibility 2.13 |
| living-room-2 | 0.122 | 0.171 | -0.042 | 6.08 | 56.96 | Reflection Trace 18.17, GI 10.40, GI Sides 5.58 |
| house | 0.094 | 0.168 | +0.004 | 5.71 | 22.82 | GI Sides 4.13, GI 4.10, Main Transmissive 3.84 |
| veach-ajar | 0.093 | 0.111 | +0.009 | 5.77 | 35.23 | Main Transmissive 14.74, GI 3.61, Main Visibility 3.28 |
| teapot | 0.078 | 0.084 | -0.049 | 4.75 | 23.69 | GI Sides 4.89, Main Visibility 3.60, Reflection Trace 2.87 |
| material-testball | 0.070 | 0.083 | -0.039 | 3.20 | 23.03 | GI Sides 4.00, Main Visibility 3.33, GI 3.18 |
| cornell-box | 0.060 | 0.068 | -0.006 | 1.16 | 22.80 | GI 14.06, GI Sides 2.29, Main Visibility 1.87 |
