# Research scenes: measures

speedo (extended) against the glTF research set's path traced references, written by scripts/researchcompare.ps1 with the chart beside it (overview.jpg). Captured and timed on macOS 27.2.1, Metal, Apple M4 Pro, hardware ray tracing; 2026-11-25, commit 5030a883.

- **Blurred** and **sharp**: the mean |log2| difference of the two images' linear luminances, in stops (1 is twice or half the light), blurred over 8 pixels (the lighting's error rather than its noise) and per pixel. The scenes are ordered by the blurred one, the most unlike first.
- **Light**: log2 of speedo's mean linear luminance over the reference's, in stops: the light overall (+1 twice as bright, -1 half). The chart shows it as a share brighter or darker.
- **Mean**: the mean channel difference, in levels of 255.
- **Frame**: the client's median frame time at 2560x1440 with balanced upscaling after the lighting settled, and the costliest passes' gpu times, in ms.

| Scene | Blurred | Sharp | Light | Mean | Frame | Costliest passes |
|---|---:|---:|---:|---:|---:|---|
| glass-of-water | 0.743 | 0.923 | -0.250 | 25.59 | 142.71 | Main Transmissive 100.72, Reflection Trace 15.67, GI 4.49 |
| crown | 0.509 | 0.728 | -0.498 | 16.43 | 77.03 | Main Transmissive 25.61, GI 15.55, Reflection Trace 9.66 |
| cornell-caustic | 0.473 | 0.476 | +0.075 | 14.81 | 52.06 | Main Transmissive 28.05, GI 12.84, Main Visibility 1.93 |
| bmw-m6 | 0.394 | 0.429 | -0.217 | 7.53 | 85.31 | Reflection Trace 26.63, Main Transmissive 23.49, Main Visibility 7.03 |
| coffee-maker | 0.354 | 0.405 | +0.059 | 10.08 | 64.30 | Main Transmissive 28.91, Reflection Trace 7.58, GI 5.60 |
| classroom2 | 0.331 | 0.482 | -0.015 | 14.17 | 67.28 | Reflection Trace 18.81, GI 11.99, Main Visibility 8.84 |
| breakfast-room | 0.319 | 0.531 | -0.046 | 8.26 | 31.93 | GI 12.19, Reflection Trace 5.19, GI Sides 3.18 |
| living-room | 0.292 | 0.358 | -0.015 | 9.91 | 58.89 | Reflection Trace 16.74, Main Transmissive 11.70, GI 10.47 |
| classroom | 0.272 | 0.401 | +0.006 | 12.79 | 39.02 | Reflection Trace 16.15, GI Sides 3.48, Main Visibility 3.44 |
| pontiac-gto | 0.253 | 0.282 | -0.197 | 5.29 | 43.36 | Main Transmissive 19.10, Reflection Trace 5.96, GI Sides 3.94 |
| bathroom | 0.242 | 0.378 | -0.050 | 11.62 | 53.22 | Reflection Trace 18.30, GI 7.78, Main Transmissive 4.96 |
| veach-mis | 0.242 | 0.268 | -0.086 | 6.15 | 26.77 | Reflection Trace 6.64, GI 3.42, Main Visibility 3.17 |
| kitchen | 0.226 | 0.302 | -0.034 | 8.27 | 46.10 | Reflection Trace 10.00, GI 9.89, Main Transmissive 6.97 |
| veach-bidir | 0.226 | 0.243 | -0.107 | 6.44 | 28.18 | GI 14.97, Main Transmissive 3.06, Main Visibility 2.11 |
| living-room-2 | 0.220 | 0.253 | +0.133 | 9.06 | 53.33 | Reflection Trace 17.89, GI 10.18, Main Visibility 4.18 |
| veach-ajar | 0.213 | 0.224 | +0.142 | 12.26 | 33.76 | Main Transmissive 14.47, GI 3.55, Main Visibility 3.28 |
| teapot-full | 0.206 | 0.250 | +0.026 | 9.92 | 55.16 | Main Transmissive 39.02, Main Visibility 3.26, Shadow Classify 2.42 |
| dragon | 0.204 | 0.352 | +0.076 | 2.09 | 18.52 | GI 11.18, GI Sides 1.87, Upscale Accumulate 1.08 |
| spaceship | 0.194 | 0.217 | -0.233 | 3.65 | 48.89 | Main Transmissive 13.13, GI 8.26, Reflection Trace 6.67 |
| lamp | 0.185 | 0.228 | +0.050 | 6.86 | 33.13 | Main Transmissive 4.33, Main Visibility 4.23, Reflection Trace 3.92 |
| vintage-car | 0.179 | 0.225 | -0.163 | 5.52 | 67.85 | Main Transmissive 35.87, Reflection Trace 6.84, Main Visibility 4.52 |
| bathroom2 | 0.171 | 0.209 | -0.074 | 8.77 | 47.41 | Reflection Trace 10.72, GI 8.38, Main Transmissive 8.08 |
| modern-hall | 0.169 | 0.199 | -0.103 | 7.04 | 31.29 | Reflection Trace 6.11, GI 5.46, Main Visibility 3.91 |
| staircase | 0.122 | 0.185 | +0.025 | 4.90 | 50.96 | Reflection Trace 13.77, GI 11.61, Main Transmissive 6.36 |
| house | 0.114 | 0.183 | -0.023 | 6.37 | 22.75 | Main Transmissive 5.06, GI 4.13, GI Sides 2.77 |
| material-testball | 0.078 | 0.091 | -0.043 | 3.47 | 21.61 | GI Sides 3.22, Main Visibility 3.18, GI 3.13 |
| teapot | 0.077 | 0.083 | -0.051 | 4.83 | 22.09 | GI Sides 3.85, Main Visibility 3.40, Reflection Trace 2.80 |
| cornell-box | 0.058 | 0.066 | -0.011 | 1.12 | 21.89 | GI 14.03, Main Visibility 1.90, GI Sides 1.87 |
