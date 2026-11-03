// Mpeg2Simd kernels against their scalar versions on random data. On x86 both
// are the scalar code; on ARM with NEON this checks the NEON kernels (the
// decoder also runs the check at startup, see CMpegHle::Enable).

#include <cstdio>
#include "Mpeg2Simd.h"

int main()
{
	unsigned int failures = Mpeg2Simd::SelfTest(20000);
#if defined(__ARM_NEON)
	const char* kind = "NEON";
#else
	const char* kind = "scalar";
#endif
	std::printf("%s kernels: %u mismatches\n", kind, failures);
	return (failures == 0) ? 0 : 1;
}
