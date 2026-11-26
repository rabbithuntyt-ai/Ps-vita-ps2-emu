// Re4Vu1::ClipRejects against Re4Vu1::ClipTriangle: on random world triangles (positions around
// and far outside the clip volume, random clip matrices and scales), a triangle ClipRejects
// rejects must be one ClipTriangle returns nothing for. Also counts how many of the triangles
// the clipper empties are caught early (for information).
#include <cstdio>
#include <random>
#include "Re4Vu1Batch.h"

int main()
{
	std::mt19937 rng(4321);
	std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
	uint64_t triangles = 0, rejected = 0, empty = 0, failures = 0;
	for(int trial = 0; trial < 2000; trial++)
	{
		Re4Vu1::WORLD_BATCH batch;
		batch.scale = (trial % 3 == 0) ? 1.0f : 0.25f + (rng() % 16) / 8.0f;
		//A clip matrix near a perspective one, with noise
		for(int r = 0; r < 4; r++) batch.clip[r] = {unit(rng) * 0.1f, unit(rng) * 0.1f, unit(rng) * 0.1f, unit(rng) * 0.1f};
		batch.clip[0].x += 1.0f;
		batch.clip[1].y += 1.0f;
		batch.clip[2].z += 1.0f;
		batch.clip[2].w += 1.0f;
		batch.clip[3].w += 0.5f * unit(rng);
		batch.projection[0].x = batch.projection[1].y = batch.projection[2].z = batch.projection[3].w = 1.0f;
		batch.count = 48;
		batch.raw.assign(batch.count * 12, 0);
		float range = (trial % 4 == 0) ? 10.0f : (trial % 4 == 1) ? 200.0f : (trial % 4 == 2) ? 3000.0f : 60000.0f;
		for(uint32_t v = 0; v < batch.count; v++)
		{
			int32_t* r = batch.raw.data() + v * 12;
			for(int k = 0; k < 3; k++) r[k] = static_cast<int32_t>(unit(rng) * range);
			r[3] = 1;
			r[6] = 1;
		}
		for(uint32_t i = 0; i + 2 < batch.count; i++)
		{
			triangles++;
			Re4Vu1::WORLD_VERTEX last3[3] = {Re4Vu1::ReadWorldVertex(batch, i), Re4Vu1::ReadWorldVertex(batch, i + 1), Re4Vu1::ReadWorldVertex(batch, i + 2)};
			Re4Vu1::WORLD_VERTEX polygon[12];
			uint32_t count = Re4Vu1::ClipTriangle(last3, polygon);
			bool isEmpty = count < 3;
			bool reject = Re4Vu1::ClipRejects(batch, i);
			if(isEmpty) empty++;
			if(reject) rejected++;
			if(reject && !isEmpty)
			{
				if(failures < 5) std::printf("  FAIL trial %d triangle %u: rejected, but the clipper keeps %u vertices\n", trial, i, count);
				failures++;
			}
		}
	}
	std::printf("  %s %llu triangles: clipper empties %llu, rejected early %llu, wrongly rejected %llu\n", failures ? "FAIL" : "ok  ",
	            (unsigned long long)triangles, (unsigned long long)empty, (unsigned long long)rejected, (unsigned long long)failures);
	return failures ? 1 : 0;
}
