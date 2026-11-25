/*
 * rnn_kern.h — freestanding (kernel) replacements for the libc/libm calls RNNoise v0.1.1 uses.
 * Part of the VoodooHDA SOF port; same license as RNNoise (see COPYING).
 *
 * The kernel has no libm: sqrt/fabs map to the SSE instructions, log10 uses a short
 * atanh series (~1e-12 relative), sin/cos are only used to build the FFT twiddles, the
 * analysis window and the DCT table once, and use a range-reduced Taylor series (~1e-13).
 */
#ifndef RNN_KERN_H
#define RNN_KERN_H

#include <stddef.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline double rnnk_sqrt(double x) { return __builtin_sqrt(x); }
static inline double rnnk_fabs(double x) { return __builtin_fabs(x); }

static inline double rnnk_floor(double x)
{
	double t = (double)(long long)x;
	return t > x ? t - 1.0 : t;
}

static inline double rnnk_log10(double x)
{
	union { double d; unsigned long long u; } v;
	int e;
	double m, s, s2, ln;

	if (!(x > 0))
		return -308.0;
	v.d = x;
	e = (int)((v.u >> 52) & 0x7FF) - 1023;
	if (e == -1023)			/* subnormal: not reached by RNNoise (inputs >= 1e-2) */
		return -308.0;
	v.u = (v.u & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;
	m = v.d;			/* [1, 2) */
	if (m > 1.41421356237309504880) {
		m *= 0.5;
		e++;
	}
	s = (m - 1.0) / (m + 1.0);	/* |s| <= 0.1716 */
	s2 = s * s;
	ln = 2.0 * s * (1.0 + s2 * (1.0 / 3 + s2 * (1.0 / 5 + s2 * (1.0 / 7 + s2 * (1.0 / 9 +
	     s2 * (1.0 / 11 + s2 * (1.0 / 13 + s2 * (1.0 / 15))))))));
	ln += e * 0.69314718055994530942;
	return ln * 0.43429448190325182765;	/* 1 / ln(10) */
}

/* sin(x) for |x| <= pi by Taylor series up to x^25 */
static inline double rnnk_sin_pi(double x)
{
	double x2 = x * x, term = x, sum = x;
	for (int n = 1; n <= 12; n++) {
		term *= -x2 / ((2.0 * n) * (2.0 * n + 1.0));
		sum += term;
	}
	return sum;
}

static inline double rnnk_reduce(double x)
{
	const double twopi = 6.28318530717958647692;
	double k = rnnk_floor(x / twopi + 0.5);
	return x - k * twopi;		/* [-pi, pi] */
}

static inline double rnnk_sin(double x) { return rnnk_sin_pi(rnnk_reduce(x)); }
static inline double rnnk_cos(double x) { return rnnk_sin_pi(rnnk_reduce(x + 1.57079632679489661923)); }

/* one-time allocations (FFT state) come from a static arena; nothing is ever freed */
void *rnnk_alloc(size_t size);

#define sqrt(x)		rnnk_sqrt(x)
#define fabs(x)		rnnk_fabs(x)
#define floor(x)	rnnk_floor(x)
#define log10(x)	rnnk_log10(x)
#define sin(x)		rnnk_sin(x)
#define cos(x)		rnnk_cos(x)
#define abs(x)		__builtin_abs(x)

#endif /* RNN_KERN_H */
