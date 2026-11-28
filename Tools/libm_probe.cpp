// Shows whether two machines get the same results out of the math functions that the simulation uses, which playing in lockstep takes.
// Build and run this on each, the way the game is built, and compare what they print:
//     c++ -std=c++20 -O2 -ffp-contract=off Tools/libm_probe.cpp -o libm_probe && ./libm_probe
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

static uint64_t s_State;

// The same numbers on every machine, spread over what the game would pass: angles, lengths and factors.
template <typename Type> static Type NextInput(Type range) {
	s_State = s_State * 6364136223846793005ULL + 1442695040888963407ULL;
	return static_cast<Type>(static_cast<double>(s_State >> 11) / 9007199254740992.0 * 2.0 - 1.0) * range;
}

template <typename Type, typename Function> static void Probe(const char* name, Type range, Function function) {
	s_State = 1;
	uint64_t hash = 1469598103934665603ULL;
	for (int i = 0; i < 2000000; ++i) {
		Type result = function(NextInput(range), NextInput(range));
		unsigned char bytes[sizeof(Type)];
		std::memcpy(bytes, &result, sizeof(Type));
		for (unsigned char byte: bytes) {
			hash = (hash ^ byte) * 1099511628211ULL;
		}
	}
	std::printf("%-8s %016llx\n", name, static_cast<unsigned long long>(hash));
}

int main() {
	Probe<float>("sinf", 100.0F, [](float a, float) { return std::sin(a); });
	Probe<float>("cosf", 100.0F, [](float a, float) { return std::cos(a); });
	Probe<float>("tanf", 100.0F, [](float a, float) { return std::tan(a); });
	Probe<float>("asinf", 1.0F, [](float a, float) { return std::asin(a); });
	Probe<float>("acosf", 1.0F, [](float a, float) { return std::acos(a); });
	Probe<float>("atanf", 100.0F, [](float a, float) { return std::atan(a); });
	Probe<float>("atan2f", 100.0F, [](float a, float b) { return std::atan2(a, b); });
	Probe<float>("expf", 20.0F, [](float a, float) { return std::exp(a); });
	Probe<float>("logf", 1000.0F, [](float a, float) { return std::log(std::fabs(a)); });
	Probe<float>("powf", 20.0F, [](float a, float b) { return std::pow(std::fabs(a), b); });
	Probe<float>("sqrtf", 1000.0F, [](float a, float) { return std::sqrt(std::fabs(a)); });
	Probe<float>("fmodf", 1000.0F, [](float a, float b) { return std::fmod(a, b); });
	Probe<float>("hypotf", 1000.0F, [](float a, float b) { return std::hypot(a, b); });
	Probe<double>("sin", 100.0, [](double a, double) { return std::sin(a); });
	Probe<double>("cos", 100.0, [](double a, double) { return std::cos(a); });
	Probe<double>("tan", 100.0, [](double a, double) { return std::tan(a); });
	Probe<double>("asin", 1.0, [](double a, double) { return std::asin(a); });
	Probe<double>("acos", 1.0, [](double a, double) { return std::acos(a); });
	Probe<double>("atan", 100.0, [](double a, double) { return std::atan(a); });
	Probe<double>("atan2", 100.0, [](double a, double b) { return std::atan2(a, b); });
	Probe<double>("exp", 20.0, [](double a, double) { return std::exp(a); });
	Probe<double>("log", 1000.0, [](double a, double) { return std::log(std::fabs(a)); });
	Probe<double>("log10", 1000.0, [](double a, double) { return std::log10(std::fabs(a)); });
	Probe<double>("pow", 20.0, [](double a, double b) { return std::pow(std::fabs(a), b); });
	Probe<double>("sqrt", 1000.0, [](double a, double) { return std::sqrt(std::fabs(a)); });
	Probe<double>("fmod", 1000.0, [](double a, double b) { return std::fmod(a, b); });
	Probe<double>("floor", 1000.0, [](double a, double) { return std::floor(a); });
	return 0;
}
