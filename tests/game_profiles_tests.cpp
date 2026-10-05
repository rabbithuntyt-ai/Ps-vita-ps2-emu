#include <cstdio>
#include "AutoCycleRate.h"
#include "GameProfiles.h"

namespace
{
	int g_failures = 0;
	void Check(bool condition, const char* what)
	{
		if(!condition)
		{
			std::printf("  FAIL %s\n", what);
			g_failures++;
		}
	}
}

int main(int argc, char** argv)
{
	CGameProfiles profiles;
	profiles.Parse("# comment\n[SLUS-20228]\nname = Silent Hill 2\nthreaded_vu1=1\n\n[sces_500.51]\nee_cycle_rate=75\nbroken line\n");
	Check(profiles.GetCount() == 2, "two profiles");
	auto sh2 = profiles.Find("SLUS_202.28");
	Check(sh2 && sh2->at("name") == "Silent Hill 2", "serial forms match, values trimmed");
	Check(sh2 && sh2->at("threaded_vu1") == "1", "value");
	auto other = profiles.Find("SCES-50051");
	Check(other && other->at("ee_cycle_rate") == "75", "case-insensitive section");
	Check(profiles.Find("SLUS-99999") == nullptr, "unknown game");

	// The bundled file must parse and contain its entries.
	if(argc > 1)
	{
		CGameProfiles bundled;
		Check(bundled.Load(argv[1]), "bundled file loads");
		Check(bundled.Find("SLUS-20228") != nullptr, "bundled file has entries");
	}

	// Auto EE cycle rate.
	CAutoCycleRate autoRate;
	Check(autoRate.GetRate() == 100, "auto starts at 100");
	Check(!autoRate.Update(0.0f, 30, 60), "one slow second is not enough");
	Check(autoRate.Update(0.0f, 30, 60) && autoRate.GetRate() == 90, "steps down when EE bound");
	for(int i = 0; i < 20; i++) autoRate.Update(0.0f, 30, 60);
	Check(autoRate.GetRate() == 60, "floor");
	autoRate.Update(0.0f, 30, 60);
	Check(!autoRate.Update(0.01f, 60, 60), "full speed does not step down");
	Check(autoRate.GetRate() == 60, "holds at full speed without idle");
	for(int i = 0; i < 4; i++) autoRate.Update(0.5f, 60, 60);
	Check(autoRate.GetRate() == 60, "climbs slowly");
	autoRate.Update(0.5f, 60, 60);
	Check(autoRate.GetRate() == 75, "climbs back with headroom");
	autoRate.Reset();
	Check(autoRate.GetRate() == 100, "reset");

	std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
	return g_failures ? 1 : 0;
}
