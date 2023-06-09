#pragma once

template <typename T, typename U, typename V>
static auto clamp(T x, U lowerlimit, V upperlimit)
{
	return (x < lowerlimit ? lowerlimit : (x > upperlimit ? upperlimit : x));
}

template <typename T, typename U, typename V>
static auto ramp(T x, U edge0, V edge1)
{
	return (x - edge0) / (edge1 - edge0);
}

template <typename T, typename U, typename V>
static auto lerp(T a, U b, V t)
{
	return (1 - t) * a + t * b;
}

template <typename T>
static auto smoothstep(T x)
{
	return x * x * (3 - 2 * x);
}

template <typename T>
static auto smootherstep(T x)
{
	return x * x * x * (x * (x * 6 - 15) + 10);
}
