#include "global_allocator_api.hpp"
#include "safe_memory_leak_reporter.hpp"

#include <random>
#include <iostream>
#include <vector>
#include <algorithm>
#include <thread>
#include <chrono>

/// @brief 全局析构监视器 / Monitor for global destruction
class AtExitDestructionMonitor
{
public:
	~AtExitDestructionMonitor()
	{
		SafeMemoryLeakReporter::in_global_destruction = true;
	}
};

static AtExitDestructionMonitor global_destruction_monitor;	 //!< 全局实例，用于监视析构 / Global instance to monitor destruction

// 测试nothrow分配场景 / Test nothrow allocation scenario
void test_nothrow()
{
	// 尝试分配8GB / Attempt to allocate 8GB
	void* first_allocation_pointer = ALLOCATE_NOTHROW( 1024ULL * 1024ULL * 1024ULL * 8ULL);
	if ( !first_allocation_pointer )
	{
		std::cout << "Nothrow allocation failed as expected" << std::endl;
	}
	else
	{
		os_memory::api::my_deallocate( first_allocation_pointer );
	}

	// 测试普通分配（会抛出异常） / Test regular allocation (throws on failure)
	try
	{
		// 尝试分配8GB / Attempt to allocate 8GB
		void* second_allocation_pointer = ALLOCATE( 1024ULL * 1024ULL * 1024ULL * 8ULL);
		os_memory::api::my_deallocate( second_allocation_pointer );
	}
	catch ( const std::bad_alloc& exception_reference )
	{
		std::cout << "Caught bad_alloc: " << exception_reference.what() << std::endl;
	}
}

// 测试内存泄漏场景 / Test memory leak scenario
void test_memory_leak()
{
	// 分配带调试信息的内存 / Allocate memory with debug info
	int*	first_int_pointer = static_cast<int*>( ALLOCATE( 1024 ) );
	double* first_aligned_double_pointer = static_cast<double*>( ALLOCATE_ALIGNED( 256, 64 ) );

	// 故意泄漏一个内存块 / Intentionally leak one block
	// void* intentional_leak = ALLOC(512);

	// 释放部分内存 / Deallocate some blocks
	os_memory::api::my_deallocate( first_int_pointer );
	os_memory::api::my_deallocate( first_aligned_double_pointer );
}

/// @brief 分配器碎片化场景 / Fragmentation stress test
void test_fragmentation()
{
	std::mt19937_64						  random_engine( static_cast<unsigned long long>( std::chrono::steady_clock::now().time_since_epoch().count() ) );
	std::uniform_int_distribution<size_t> small_size_distribution( 16, 256 );
	std::uniform_int_distribution<size_t> medium_size_distribution( 257, 4096 );
	std::uniform_int_distribution<size_t> large_size_distribution( 4097, 16384 );
	std::vector<void*>					  allocation_pointer_list;
	allocation_pointer_list.reserve( 2000 );

	// 交替分配小/中/大块并随机对齐 / Alternate allocations of small/medium/large blocks with random alignment
	std::vector<size_t> alignment_options = { 8, 16, 32, 64, 128, 256 };
	for ( int iteration_index = 0; iteration_index < 1200; ++iteration_index )
	{
		size_t allocation_size;
		if ( iteration_index % 3 == 0 )
			allocation_size = small_size_distribution( random_engine );
		else if ( iteration_index % 3 == 1 )
			allocation_size = medium_size_distribution( random_engine );
		else
			allocation_size = large_size_distribution( random_engine );

		size_t allocation_alignment = alignment_options[ random_engine() % alignment_options.size() ];
		void*  allocation_pointer = ALLOCATE_ALIGNED_NOTHROW( allocation_size, allocation_alignment );
		if ( allocation_pointer )
			allocation_pointer_list.push_back( allocation_pointer );
	}

	// 随机释放一半以制造空洞 / Randomly free half to create holes
	std::shuffle( allocation_pointer_list.begin(), allocation_pointer_list.end(), random_engine );
	for ( size_t release_index = 0; release_index < allocation_pointer_list.size() / 2; ++release_index )
	{
		DEALLOCATE( allocation_pointer_list[ release_index ] );
		allocation_pointer_list[ release_index ] = nullptr;
	}

	// 再次分配填充碎片 / Reallocate to fill fragmentation
	for ( int refill_index = 0; refill_index < 600; ++refill_index )
	{
		size_t allocation_size = static_cast<size_t>( ( refill_index * 37 ) % 1024 ) + 1;
		void*  allocation_pointer = ALLOCATE( allocation_size );
		if ( allocation_pointer )
			allocation_pointer_list.push_back( allocation_pointer );
	}

	// 释放所有残留指针 / Free all remaining pointers
	for ( void* allocation_pointer : allocation_pointer_list )
	{
		if ( allocation_pointer )
			DEALLOCATE( allocation_pointer );
	}
}

/// @brief 大对象碎片化场景 / Large object fragmentation stress test
void test_large_fragmentation()
{
	std::mt19937_64		random_engine( static_cast<unsigned long long>( std::chrono::steady_clock::now().time_since_epoch().count() ) );
	std::vector<size_t> large_size_options = { 1 << 20, 2 << 20, 4 << 20, 8 << 20, 16 << 20, 32 << 20, 64 << 20, 128 << 20, 256 << 20, 512 << 20, 1024 << 20 };
	std::vector<void*>	allocation_pointer_list;
	allocation_pointer_list.reserve( 500 );

	// 随机分配大块 / Randomly allocate large blocks
	for ( int allocation_index = 0; allocation_index < 300; ++allocation_index )
	{
		size_t allocation_size = large_size_options[ random_engine() % large_size_options.size() ];
		void*  allocation_pointer = ALLOCATE_NOTHROW( allocation_size );
		if ( allocation_pointer )
			allocation_pointer_list.push_back( allocation_pointer );
	}

	// 随机释放一半以制造大块空洞 / Randomly free half to create large holes
	std::shuffle( allocation_pointer_list.begin(), allocation_pointer_list.end(), random_engine );
	for ( size_t release_index = 0; release_index < allocation_pointer_list.size() / 2; ++release_index )
	{
		DEALLOCATE( allocation_pointer_list[ release_index ] );
		allocation_pointer_list[ release_index ] = nullptr;
	}

	// 再次分配以填补大块空洞 / Reallocate to fill large holes
	for ( int refill_index = 0; refill_index < 150; ++refill_index )
	{
		size_t allocation_size = large_size_options[ ( refill_index * 7 ) % large_size_options.size() ];
		void*  allocation_pointer = ALLOCATE_NOTHROW( allocation_size );
		if ( allocation_pointer )
			allocation_pointer_list.push_back( allocation_pointer );
	}

	// 释放所有 / Free all allocations
	for ( void* allocation_pointer : allocation_pointer_list )
	{
		if ( allocation_pointer )
			DEALLOCATE( allocation_pointer );
	}
}

/// @brief 多线程并发分配/释放场景 / Multithreaded stress test
void worker_thread( int thread_id )
{
	std::mt19937_64						  random_engine( static_cast<unsigned long long>( thread_id ) );
	std::uniform_int_distribution<size_t> size_distribution( 1, 2048 );
	for ( int operation_index = 0; operation_index < 5000; ++operation_index )
	{
		size_t allocation_size = size_distribution( random_engine );
		void*  allocation_pointer = ALLOCATE_NOTHROW( allocation_size );
		if ( !allocation_pointer )
			continue;

		if ( ( random_engine() & 1 ) == 0 )
		{
			DEALLOCATE( allocation_pointer );
		}
		else
		{
			// 模拟短暂工作负载 / Simulate brief workload
			std::this_thread::sleep_for( std::chrono::microseconds( random_engine() % 100 ) );
			DEALLOCATE( allocation_pointer );
		}
	}
}

void test_multithreaded()
{
	unsigned number_of_threads = std::thread::hardware_concurrency();
	if ( number_of_threads == 0 )
		number_of_threads = 4;

	std::vector<std::thread> thread_list;
	for ( unsigned thread_index = 0; thread_index < number_of_threads; ++thread_index )
	{
		thread_list.emplace_back( worker_thread, thread_index + 1 );
	}
	for ( auto& current_thread : thread_list )
	{
		current_thread.join();
	}
}

/// @brief 故意泄漏测试 / Intentional leak test
void test_leak_scenario()
{
	// 漏 10 个小对象 / Leak 10 small objects
	for ( int leak_index = 0; leak_index < 10; ++leak_index )
	{
		( void )ALLOCATE( 128 );
	}
}

int main( int argument_count, char** argument_values )
{
	// 启用内存跟踪 / Enable memory tracking
	os_memory::api::enable_memory_tracking( true );

	test_nothrow();				 // 测试通过 / Test passed
	test_memory_leak();			 // 测试通过 / Test passed
	test_fragmentation();		 // 测试通过 / Test passed
	test_large_fragmentation();	 // 测试通过 / Test passed
	test_multithreaded();		 // 测试通过 / Test passed
	// test_leak_scenario();    // 测试通过 / Test passed

	// 手动报告泄漏（程序退出时会自动报告） / Manual leak report (automatically reported at program exit)
	os_memory::api::report_memory_leaks();

	// 输出当前内存使用情况（已注释） / Output current memory usage (commented out)
	// std::cout << "Current memory usage: " << os::current_memory_usage() << " bytes" << std::endl;

	// 禁用内存跟踪 / Disable memory tracking
	os_memory::api::disable_memory_tracking();

	// 再次报告泄漏，确保禁用后不会有报告 / Report leaks again to ensure none after disabling
	os_memory::api::report_memory_leaks();

	return 0;
}
