// The math functions whose results aren't pinned down by any standard, for the whole game to use in place of the system's: the simulation, Lua and the libraries alike.
// Each system's math library rounds these a little differently, which is enough for two machines that play each other in lockstep to drift apart. So these are built from one and the same source everywhere, the routines that SDL comes with.
// They take the place of the system's simply by having the same names, and being part of the executable.
// shortcut: sinh, cosh, tanh and the like are still the system's, as nothing in the game or its scripts uses them. Add them here if a mod that does makes for desyncs.
// shortcut: a call with constant arguments can still be worked out by the compiler, in its own way. None are known to matter.
#include "math_libm.h"

static const double c_InverseLn2 = 1.44269504088896338700e+00;

double sin(double x) { return SDL_uclibc_sin(x); }
double cos(double x) { return SDL_uclibc_cos(x); }
double tan(double x) { return SDL_uclibc_tan(x); }
double atan(double x) { return SDL_uclibc_atan(x); }
double atan2(double y, double x) { return SDL_uclibc_atan2(y, x); }
double exp(double x) { return SDL_uclibc_exp(x); }
double log(double x) { return SDL_uclibc_log(x); }
double log10(double x) { return SDL_uclibc_log10(x); }
double pow(double x, double y) { return SDL_uclibc_pow(x, y); }

// These two come out as not-a-number for anything outside of -1 to 1, as they should, since the square root does.
double asin(double x) { return SDL_uclibc_atan2(x, __builtin_sqrt((1.0 - x) * (1.0 + x))); }
double acos(double x) { return SDL_uclibc_atan2(__builtin_sqrt((1.0 - x) * (1.0 + x)), x); }

double log2(double x) {
	double result = SDL_uclibc_log(x) * c_InverseLn2;
	// Powers of two have to come out exact, which the multiplication doesn't see to.
	double rounded = __builtin_rint(result);
	return (rounded > -1000.0 && rounded < 1000.0 && SDL_uclibc_scalbn(1.0, (int)rounded) == x) ? rounded : result;
}

// The ones for floats work the number out as a double and round that, which gives the same float everywhere just the same.
float sinf(float x) { return (float)SDL_uclibc_sin(x); }
float cosf(float x) { return (float)SDL_uclibc_cos(x); }
float tanf(float x) { return (float)SDL_uclibc_tan(x); }
float atanf(float x) { return (float)SDL_uclibc_atan(x); }
float atan2f(float y, float x) { return (float)SDL_uclibc_atan2(y, x); }
float expf(float x) { return (float)SDL_uclibc_exp(x); }
float logf(float x) { return (float)SDL_uclibc_log(x); }
float log10f(float x) { return (float)SDL_uclibc_log10(x); }
float log2f(float x) { return (float)log2(x); }
float powf(float x, float y) { return (float)SDL_uclibc_pow(x, y); }
float asinf(float x) { return (float)asin(x); }
float acosf(float x) { return (float)acos(x); }

// Compilers turn a sine and a cosine of the same angle into one call to these, which would otherwise get past the ones above.
#if defined(__APPLE__) && defined(__aarch64__)
struct SinCos {
	double Sin;
	double Cos;
};
struct SinCosF {
	float Sin;
	float Cos;
};
struct SinCos __sincos_stret(double x) { return (struct SinCos){SDL_uclibc_sin(x), SDL_uclibc_cos(x)}; }
struct SinCosF __sincosf_stret(float x) { return (struct SinCosF){(float)SDL_uclibc_sin(x), (float)SDL_uclibc_cos(x)}; }
#else
void sincos(double x, double* sine, double* cosine) {
	*sine = SDL_uclibc_sin(x);
	*cosine = SDL_uclibc_cos(x);
}
void sincosf(float x, float* sine, float* cosine) {
	*sine = (float)SDL_uclibc_sin(x);
	*cosine = (float)SDL_uclibc_cos(x);
}
#endif
