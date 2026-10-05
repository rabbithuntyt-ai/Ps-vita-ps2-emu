#pragma once

// First-fit allocator with coalescing over one fixed region of executable
// memory. The Vita only hands out RWX memory in large blocks (1MB minimum,
// sceKernelAllocMemBlockForVM), while Play!'s recompiler wants one small
// allocation per translated block, so translated code is carved from a pool.

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>

class CJitPool
{
public:
	static constexpr size_t ALIGNMENT = 16;

	void Init(void* base, size_t size)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_base = reinterpret_cast<uintptr_t>(base);
		m_size = size & ~(ALIGNMENT - 1);
		m_free.clear();
		m_used.clear();
		m_free[m_base] = m_size;
		m_usedBytes = 0;
	}

	void* Alloc(size_t size)
	{
		if(size == 0) size = 1;
		size = (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
		std::lock_guard<std::mutex> lock(m_mutex);
		for(auto it = m_free.begin(); it != m_free.end(); ++it)
		{
			if(it->second < size) continue;
			uintptr_t address = it->first;
			size_t remaining = it->second - size;
			m_free.erase(it);
			if(remaining != 0) m_free[address + size] = remaining;
			m_used[address] = size;
			m_usedBytes += size;
			return reinterpret_cast<void*>(address);
		}
		return nullptr;
	}

	void Free(void* ptr)
	{
		if(!ptr) return;
		std::lock_guard<std::mutex> lock(m_mutex);
		auto used = m_used.find(reinterpret_cast<uintptr_t>(ptr));
		if(used == m_used.end()) return;
		uintptr_t address = used->first;
		size_t size = used->second;
		m_used.erase(used);
		m_usedBytes -= size;

		auto next = m_free.lower_bound(address);
		if((next != m_free.end()) && (address + size == next->first))
		{
			size += next->second;
			next = m_free.erase(next);
		}
		if(next != m_free.begin())
		{
			auto prev = std::prev(next);
			if(prev->first + prev->second == address)
			{
				prev->second += size;
				return;
			}
		}
		m_free[address] = size;
	}

	size_t GetUsedBytes()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_usedBytes;
	}

	size_t GetFreeRangeCount()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_free.size();
	}

	size_t GetCapacity() const
	{
		return m_size;
	}

private:
	std::mutex m_mutex;
	uintptr_t m_base = 0;
	size_t m_size = 0;
	std::map<uintptr_t, size_t> m_free;
	std::map<uintptr_t, size_t> m_used;
	size_t m_usedBytes = 0;
};
