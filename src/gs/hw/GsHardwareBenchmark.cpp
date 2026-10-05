#include "GsHardwareBenchmark.h"
#include "GSH_Hardware.h"
#include "GpuGl.h"

namespace
{
	class CHardwareTarget : public CGSH_Hardware, public IGsBenchmarkTarget
	{
	public:
		CHardwareTarget()
		    : CGSH_Hardware(OPTIONS())
		{
			ResetBase();
			InitializeImpl();
			ResetImpl();
		}
		~CHardwareTarget() override
		{
			ReleaseImpl();
		}
		void Write(uint8 reg, uint64 value) override
		{
			WriteRegisterImpl(reg, value);
		}
		uint8* Ram() override
		{
			return m_pRAM;
		}
		void Sync() override
		{
			MarkNewFrame(); //submits the batched draws
			glFinish();
		}
	};
}

std::vector<GS_BENCHMARK_RESULT> RunGsHardwareBenchmark(double secondsPerWorkload,
                                                        const std::function<void(const GS_BENCHMARK_RESULT&)>& onResult)
{
	return RunGsBenchmarkOn(
	    secondsPerWorkload, []() { return std::make_unique<CHardwareTarget>(); }, onResult);
}
