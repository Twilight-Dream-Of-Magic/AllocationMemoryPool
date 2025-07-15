#include "global_allocator_api.hpp"
#include "stl_allocator.hpp"
#include "safe_memory_leak_reporter.hpp"

#include <iostream>
#include <algorithm>
#include <vector>
#include <array>
#include <random>
#include <thread>
#include <chrono>

/*
 * Detailed Explanation of Two Key Lines for C++ I/O Optimization
 * 1. Disable C/C++ Stream Synchronization
 *    std::ios::sync_with_stdio(false);
 *
 *    • Benefits:
 *      - Eliminates mutex-like synchronization between C++ streams (std::cin/std::cout)
 *        and C stdio (printf/scanf), reducing OS-level function call overhead.
 *      - Can yield up to 2× speedup in pure C++ I/O scenarios, especially when reading
 *        or writing large volumes of data.
 *
 *    • Drawbacks:
 *      - After disabling, mixing printf/scanf with std::cout/std::cin leads to unpredictable
 *        ordering or missing output, complicating debugging.
 *      - On some platforms or compilers, default I/O may already be optimized, so gains
 *        may be negligible in light I/O workloads.
 *
 * 2. Untie std::cin from std::cout
 *    std::cin.tie(nullptr);
 *
 *    • Benefits:
 *      - Prevents std::cin from flushing std::cout before every input operation,
 *        saving a few microseconds per flush in read‑heavy loops.
 *      - In tight loops of alternating read/write, cumulative savings can reach
 *        tens or hundreds of microseconds.
 *
 *    • Drawbacks:
 *      - Prompts or previous outputs may remain buffered and not appear before input,
 *        requiring explicit std::cout.flush() or use of std::endl at key points.
 *      - If your logic relies on automatic flushing for user prompts, you must now
 *        manage flushes manually to maintain correct UX.
 *
 * Tips for Safe Use:
 * - If mixing C and C++ I/O, do not disable sync, or restrict these optimizations
 *   to pure C++ sections.
 * - In multithreaded contexts, protect shared streams with an external mutex;
 *   these calls do not alter the standard’s per‑operation thread safety guarantees.
 *
 *
 * C++ I/O 优化两行关键代码详解
 * 1. 禁用 C 与 C++ 流的同步
 *    std::ios::sync_with_stdio(false);
 *
 *    • 好处：
 *      - 消除 C++ 流（std::cin/std::cout）与 C stdio（printf/scanf）之间的“互斥”同步，
 *        减少系统调用开销。
 *      - 在纯 C++ I/O 场景下，可获得最高约 2 倍的性能提升，尤其是处理大文件或海量数据时。
 *
 *    • 坏处：
 *      - 解除同步后，若混用 printf/scanf 和 std::cout/std::cin，输出顺序可能错乱或丢失，
 *        调试难度增加。
 *      - 某些平台/编译器默认已优化 I/O，轻量级 I/O 场景下提升有限。
 *
 * 2. 解除 std::cin 与 std::cout 的绑定
 *    std::cin.tie(nullptr);
 *
 *    • 好处：
 *      - 防止 std::cin 在每次输入前自动刷新 std::cout，避免每次 flush 带来的几微秒开销。
 *      - 在交替读写的循环中，可累计节省几十到几百微秒。
 *
 *    • 坏处：
 *      - 提示信息或前一轮输出可能停留在缓冲区中，需手动调用 std::cout.flush() 或使用 std::endl。
 *      - 如果依赖自动刷新来确保提示先行显示，需自行管理刷新时机，否则影响用户体验。
 *
 * 安全使用建议：
 * - 混合使用 C/C++ I/O 时，应避免禁用同步，或仅在纯 C++ 区块内应用上述优化。
 * - 多线程环境下，共享流对象仍需使用外部互斥锁保护；这些调用不改变 C++ 标准
 *   对单次流操作的线程安全保证。
 */
void cpp_io_optimization()
{
	std::cout.sync_with_stdio( false );
	std::cin.tie( nullptr );
}

// 测试nothrow分配场景 / Test nothrow allocation scenario
void test_nothrow()
{
	// 尝试分配8GB / Attempt to allocate 8GB
	void* first_allocation_pointer = ALLOCATE_NOTHROW( 1024ULL * 1024ULL * 1024ULL * 8ULL );
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
		void* second_allocation_pointer = ALLOCATE( 1024ULL * 1024ULL * 1024ULL * 8ULL );
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
	allocation_pointer_list.reserve( 200 );

	// 随机分配大块 / Randomly allocate large blocks
	for ( int allocation_index = 0; allocation_index < 100; ++allocation_index )
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
	for ( int refill_index = 0; refill_index < 50; ++refill_index )
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

/// @brief 测试内存边界访问 / Test memory boundary access
void test_memory_boundary_access()
{
	std::cout << "\n=== Testing Memory Boundary Access ===\n";

	// 测试小内存块边界 / Test small memory block boundary
	const size_t small_size = 64;
	char*		 small_ptr = static_cast<char*>( ALLOCATE( small_size ) );
	if ( small_ptr )
	{
		std::cout << "Small allocation (" << small_size << " bytes) at: " << static_cast<void*>( small_ptr ) << "\n";

		// 测试写入和读取边界 / Test writing and reading boundaries
		small_ptr[ 0 ] = 'A';				// 首字节 / First byte
		small_ptr[ small_size - 1 ] = 'Z';	// 末字节 / Last byte

		// 验证 / Verify
		if ( small_ptr[ 0 ] == 'A' && small_ptr[ small_size - 1 ] == 'Z' )
		{
			std::cout << "  Small block boundary access successful\n";
		}
		else
		{
			std::cout << "  ERROR: Small block boundary access failed\n";
		}

		DEALLOCATE( small_ptr );
	}
	else
	{
		std::cout << "Failed to allocate small block\n";
	}

	// 测试大内存块边界 / Test large memory block boundary
	const size_t large_size = 256 * 1024 * 1024;  // 256MB
	char*		 large_ptr = static_cast<char*>( ALLOCATE( large_size ) );
	if ( large_ptr )
	{
		std::cout << "Large allocation (" << large_size << " bytes) at: " << static_cast<void*>( large_ptr ) << "\n";

		// 测试写入和读取边界 / Test writing and reading boundaries
		large_ptr[ 0 ] = 'A';				// 首字节 / First byte
		large_ptr[ large_size - 1 ] = 'Z';	// 末字节 / Last byte

		// 验证 / Verify
		if ( large_ptr[ 0 ] == 'A' && large_ptr[ large_size - 1 ] == 'Z' )
		{
			std::cout << "  Large block boundary access successful\n";
		}
		else
		{
			std::cout << "  ERROR: Large block boundary access failed\n";
		}

		DEALLOCATE( large_ptr );
	}
	else
	{
		std::cout << "Failed to allocate large block\n";
	}

	// 测试对齐内存边界 / Test aligned memory boundary
	const size_t aligned_size = 1024;
	const size_t alignment = 64;
	char*		 aligned_ptr = static_cast<char*>( ALLOCATE_ALIGNED( aligned_size, alignment ) );
	if ( aligned_ptr )
	{
		std::cout << "Aligned allocation (" << aligned_size << " bytes, alignment " << alignment << ") at: " << static_cast<void*>( aligned_ptr ) << "\n";

		// 验证对齐 / Verify alignment
		if ( reinterpret_cast<uintptr_t>( aligned_ptr ) % alignment == 0 )
		{
			std::cout << "  Alignment correct\n";
		}
		else
		{
			std::cout << "  ERROR: Alignment incorrect\n";
		}

		// 测试边界 / Test boundaries
		aligned_ptr[ 0 ] = 'A';
		aligned_ptr[ aligned_size - 1 ] = 'Z';

		if ( aligned_ptr[ 0 ] == 'A' && aligned_ptr[ aligned_size - 1 ] == 'Z' )
		{
			std::cout << "  Aligned block boundary access successful\n";
		}
		else
		{
			std::cout << "  ERROR: Aligned block boundary access failed\n";
		}

		DEALLOCATE( aligned_ptr );
	}
	else
	{
		std::cout << "Failed to allocate aligned block\n";
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

/// @brief 测试直接分配与释放 / Test direct allocate and deallocate
void test_direct_allocate()
{
	using namespace os_memory::allocator;

	STL_Allocator<int>					alloc;
	const STL_Allocator<int>::size_type N = 10;
	int*								data = alloc.allocate( N );
	assert( data != nullptr && "allocate should succeed for small N" );

	// 写入 / 读取
	for ( STL_Allocator<int>::size_type i = 0; i < N; ++i )
	{
		data[ i ] = static_cast<int>( i * i );
	}
	for ( STL_Allocator<int>::size_type i = 0; i < N; ++i )
	{
		assert( data[ i ] == static_cast<int>( i * i ) );
	}
	std::cout << "[direct] allocate & access OK\n";

	alloc.deallocate( data, N );
	std::cout << "[direct] deallocate OK\n";
}

/// @brief 测试与 std::vector 配合 / Test compatibility with std::vector
void test_vector_with_allocator()
{
	using namespace os_memory::allocator;

	std::vector<int, STL_Allocator<int>> vec;
	vec.reserve( 5 );

	for ( int i = 0; i < 5; ++i )
	{
		vec.push_back( i + 1 );
	}

	assert( vec.size() == 5 );
	for ( STL_Allocator<int>::size_type i = 0; i < vec.size(); ++i )
	{
		assert( vec[ i ] == static_cast<int>( i + 1 ) );
	}
	std::cout << "[vector] reserve, push_back & access OK\n";
}

/// @brief 测试对齐设置及 nothrow 模式 / Test alignment and nothrow mode
void test_alignment_and_nothrow()
{
	using namespace os_memory::allocator;

	// 用 char 测对齐 / Alignment test with char
	STL_Allocator<char> char_alloc;
	char*				c1 = char_alloc.allocate( 16 );
	assert( reinterpret_cast<uintptr_t>( c1 ) % alignof( void* ) == 0 );
	char_alloc.deallocate( c1, 16 );

	// 指定合法对齐（16 字节）/ Set valid alignment (16 bytes)
	char_alloc.set_alignment( 16 );
	char* c2 = char_alloc.allocate( 16 );
	assert( reinterpret_cast<uintptr_t>( c2 ) % 16 == 0 );
	assert( c2 != nullptr );
	char_alloc.deallocate( c2, 16 );

	// 指定非法对齐（3 字节）/ Set invalid alignment (3 bytes)
	char_alloc.set_alignment( 3 );
	char* c3 = char_alloc.allocate( 16 );
	assert( reinterpret_cast<uintptr_t>( c3 ) % ( alignof( void* ) * alignof( char ) ) == 0 );
	char_alloc.deallocate( c3, 16 );

	// 测试 nothrow 模式 / Test nothrow mode with large allocation
	STL_Allocator<int> int_alloc;
	int_alloc.set_nothrow( true );

	const STL_Allocator<char>::size_type BIG_COUNT = 100'000'000;  // 约 400 MB / ~400 MB
	int*								 p = int_alloc.allocate( BIG_COUNT );
	if ( !p )
	{
		std::cout << "[nothrow] allocate(" << BIG_COUNT << " ints) returned nullptr as expected\n";
	}
	else
	{
		std::cout << "[nothrow] unexpected: allocation succeeded\n";
		int_alloc.deallocate( p, BIG_COUNT );
	}
}

int main( int argument_count, char** argument_values )
{
	cpp_io_optimization();

	// 启用内存跟踪 / Enable memory tracking
	os_memory::api::enable_memory_tracking( true );

	std::cout << "=== Running STL_Allocator Tests ===\n";
	test_direct_allocate();
	test_vector_with_allocator();
	test_alignment_and_nothrow();
	std::cout << "=== All Tests Passed ===\n";

	std::cout << "=== Running GlobalAllocator or PoolAllocator Tests ===\n";
	test_memory_boundary_access();	// 测试通过 / Test passed
	test_nothrow();					// 测试通过 / Test passed
	test_memory_leak();				// 测试通过 / Test passed
	test_fragmentation();			// 测试通过 / Test passed
	test_large_fragmentation();		// 测试通过 / Test passed
	test_multithreaded();			// 测试通过 / Test passed
	std::cout << "=== All Tests Exexcuted ===\n";

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
