# Research scenes: measures

speedo (extended) against the glTF research set's path traced references, written by scripts/researchcompare.ps1 with the captures beside it (<scene>.jpg, at the reference's size, JPEG quality 92). Captured and timed on macOS 27.2.1, Metal, Apple M4 Pro, hardware ray tracing; 2026-11-27, commit 6476eef7.

- **Blurred** and **sharp**: the mean |log2| difference of the two images' linear luminances, in stops (1 is twice or half the light), blurred over 8 pixels (the lighting's error rather than its noise) and per pixel. The scenes are ordered by the blurred one, the most unlike first.
- **Light**: log2 of speedo's mean linear luminance over the reference's, in stops: the light overall (+1 twice as bright, -1 half). The chart shows it as a share brighter or darker.
- **Mean**: the mean channel difference, in levels of 255.
- **Frame**: the client's median frame time at 2560x1440 with balanced upscaling after the lighting settled, and the costliest passes' gpu times, in ms.

| Scene | Blurred | Sharp | Light | Mean | Frame | Costliest passes |
|---|---:|---:|---:|---:|---:|---|
| glass-of-water | 0.738 | 0.930 | -0.242 | 25.80 | 105.59 | Main Transmissive 63.02, Reflection Trace 13.96, Main Visibility 3.04 |
| classroom2 | 0.657 | 0.836 | -0.187 | 21.58 | 54.59 | Reflection Trace 13.25, GI 9.87, Main Transmissive 6.04 |
| crown | 0.487 | 0.741 | -0.446 | 16.76 | 61.03 | Main Transmissive 21.95, GI 9.25, Reflection Trace 8.65 |
| cornell-caustic | 0.474 | 0.478 | +0.073 | 14.87 | 38.89 | Main Transmissive 19.43, GI 6.10, Main Visibility 1.94 |
| bmw-m6 | 0.388 | 0.433 | -0.214 | 7.68 | 62.28 | Reflection Trace 18.99, Main Transmissive 14.06, Main Visibility 6.58 |
| coffee-maker | 0.352 | 0.403 | +0.059 | 10.05 | 47.01 | Main Transmissive 16.78, Reflection Trace 5.16, Main Visibility 4.59 |
| breakfast-room | 0.314 | 0.526 | -0.039 | 8.26 | 24.40 | GI 5.33, Reflection Trace 3.11, Main Visibility 3.04 |
| living-room | 0.293 | 0.370 | -0.033 | 9.70 | 45.40 | Reflection Trace 11.67, Main Transmissive 8.44, GI 6.21 |
| classroom | 0.291 | 0.438 | -0.056 | 14.05 | 39.27 | Reflection Trace 17.58, Main Visibility 3.71, GI Near 3.58 |
| kitchen | 0.267 | 0.345 | -0.072 | 9.38 | 34.83 | Reflection Trace 6.33, GI 4.49, Main Transmissive 4.44 |
| bathroom | 0.263 | 0.407 | -0.072 | 12.22 | 37.74 | Reflection Trace 9.88, GI 6.01, Main Visibility 4.22 |
| veach-mis | 0.243 | 0.267 | -0.087 | 6.14 | 21.14 | Reflection Trace 4.08, Main Visibility 3.21, GI 2.27 |
| modern-hall | 0.209 | 0.239 | -0.135 | 8.43 | 24.20 | Main Visibility 3.95, GI 3.37, Reflection Trace 3.31 |
| teapot-full | 0.206 | 0.250 | +0.026 | 9.92 | 52.61 | Main Transmissive 37.34, Main Visibility 3.57, Shadow Classify 2.42 |
| dragon | 0.202 | 0.352 | +0.070 | 2.07 | 14.39 | GI 6.35, GI Sides 1.29, Upscale Accumulate 1.09 |
| bathroom2 | 0.190 | 0.229 | -0.075 | 9.44 | 34.30 | Reflection Trace 5.88, Main Transmissive 4.71, GI 4.12 |
| lamp | 0.186 | 0.228 | +0.047 | 6.88 | 26.78 | Main Visibility 4.34, Main Transmissive 2.66, Reflection Trace 2.32 |
| spaceship | 0.180 | 0.218 | -0.196 | 3.70 | 39.03 | Main Transmissive 9.49, Reflection Trace 5.42, GI 4.57 |
| pontiac-gto | 0.179 | 0.223 | -0.098 | 5.04 | 26.57 | Main Transmissive 7.46, Reflection Trace 4.07, Main Visibility 3.25 |
| staircase | 0.174 | 0.246 | -0.009 | 5.80 | 38.12 | Reflection Trace 7.65, GI 7.63, Main Transmissive 4.56 |
| vintage-car | 0.170 | 0.219 | -0.147 | 5.36 | 64.40 | Main Transmissive 36.18, Reflection Trace 5.82, Main Visibility 4.75 |
| veach-bidir | 0.157 | 0.176 | -0.065 | 4.70 | 22.87 | GI 5.67, Main Transmissive 2.20, Main Visibility 2.15 |
| living-room-2 | 0.124 | 0.173 | -0.065 | 6.20 | 39.95 | Reflection Trace 10.81, GI 6.22, Main Visibility 4.27 |
| veach-ajar | 0.107 | 0.126 | -0.014 | 6.41 | 26.44 | Main Transmissive 9.79, Main Visibility 3.29, GI 2.36 |
| house | 0.094 | 0.167 | +0.002 | 5.70 | 17.13 | GI 2.29, Main Transmissive 2.12, Main Visibility 1.99 |
| teapot | 0.079 | 0.085 | -0.050 | 4.83 | 18.75 | Main Visibility 3.60, GI Sides 2.64, Shadow Classify 2.38 |
| material-testball | 0.070 | 0.083 | -0.039 | 3.21 | 18.25 | Main Visibility 3.35, Shadow Classify 2.54, Reflection Trace 1.86 |
| cornell-box | 0.057 | 0.066 | -0.008 | 1.14 | 16.85 | GI 7.12, Main Visibility 1.87, GI Sides 1.29 |
