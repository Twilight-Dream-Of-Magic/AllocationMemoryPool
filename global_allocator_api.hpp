/**
 * @file global_allocator.hpp
 * @brief 全局分配器单例接口 / Singleton interface for global allocator
 *
 * @details
 * 1. 提供全局唯一的 InterfaceAllocator 实例 / Provide a single global InterfaceAllocator instance;
 * 2. 默认使用 PoolAllocator，允许动态替换 / Default to PoolAllocator, support swapping allocator;
 * 3. 支持内存泄露检测和统计查询 / Support leak detection and usage reporting.
 *
 * 代码风格说明 / Style Notes
 * ---------------------------------------------------------------------------
 * 1. 完全避免缩写：所有标识符使用完整单词（allocator, instance, tracking...）；
 * 2. PascalCase 用于类型 / PascalCase for types;
 * 3. snake_case 用于函数、变量 / snake_case for functions and variables;
 * 4. SCREAMING_SNAKE_CASE 用于常量 / SCREAMING_SNAKE_CASE for constants;
 * 5. 中英文注释并存，支持 Doxygen 文档 / Bilingual comments with Doxygen support.
 */

#pragma once
#ifndef GLOBAL_ALLOCATOR_HPP
#define GLOBAL_ALLOCATOR_HPP

#include "memory_allocators.hpp"

#include <cstddef>
#include <type_traits>

namespace os_memory::api
{
	using InterfaceAllocator = os_memory::allocator::InterfaceAllocator;
	using PoolAllocator = os_memory::allocator::PoolAllocator;

	/// @brief 全局分配器管理类 / Manager for global allocator singleton
	class GlobalAllocator
	{
	public:
		/// @brief 获取全局分配器实例 / Get the global allocator instance
		static InterfaceAllocator* get()
		{
			if ( instance_ == nullptr )
			{
				static PoolAllocator default_pool_allocator;
				instance_ = &default_pool_allocator;
			}
			return instance_;
		}

		/**
		 * @brief 设置全局分配器 / Set a custom global allocator
		 * @param allocator_instance 目标分配器实例（不能为空）/ allocator instance (must not be nullptr)
		 */
		static void set( InterfaceAllocator* allocator_instance )
		{
			if ( allocator_instance == nullptr )
			{
				return;
			}
			instance_ = allocator_instance;
		}

		/**
		 * @brief 启用或禁用内存泄露检测 / Enable or disable leak detection
		 * @param detailed          是否开启详细追踪 / detailed tracking if true
		 */
		static void enable_leak_detection( bool detailed = false )
		{
			get()->enable_leak_detection( detailed );
		}

		/// @brief 报告检测到的内存泄露 / Report detected memory leaks
		static void report_leaks()
		{
			get()->report_leaks();
		}

		/**
		 * @brief 获取当前内存使用量 / Get current memory usage
		 * @return 已分配总字节数 / total bytes currently allocated
		 */
		static size_t current_memory_usage()
		{
			return get()->current_memory_usage();
		}

	private:
		static InterfaceAllocator* instance_;  //!< 全局分配器实例指针 / pointer to allocator instance
	};

	InterfaceAllocator* GlobalAllocator::instance_ = nullptr;

	/// @brief 全局分配接口函数：分配内存 / Global allocation function
	/// @see InterfaceAllocator::allocate
	inline void* my_allocate( size_t size, size_t alignment = sizeof( void* ), const char* file = nullptr, int line = 0, bool nothrow = false )
	{
		return GlobalAllocator::get()->allocate( size, alignment, file, line, nothrow );
	}

	/// @brief 全局释放接口函数：释放内存 / Global deallocation function
	/// @see InterfaceAllocator::deallocate
	inline void my_deallocate( void* pointer )
	{
		GlobalAllocator::get()->deallocate( pointer );
	}

	/// @brief 启用内存泄漏跟踪 / Enable memory leak tracking
	inline void enable_memory_tracking( bool detailed = false )
	{
		GlobalAllocator::get()->enable_leak_detection( detailed );
	}

	/// @brief 禁用内存泄漏跟踪 / Disable memory leak tracking
	inline void disable_memory_tracking()
	{
		// 注意：暂不支持禁用后恢复为未跟踪状态 / Note: disable without restoring default allocator tracking
		MemoryTracker::instance().disable();
	}

	/// @brief 报告内存泄露 / Report memory leaks
	inline void report_memory_leaks()
	{
		MemoryTracker::instance().report_leaks();
	}

	/// @brief 查询当前内存使用量 / Query current memory usage
	inline size_t get_current_memory_usage()
	{
		return MemoryTracker::instance().current_memory_usage();
	}
}

// 带调试信息的分配宏
#ifdef _DEBUG
	#define ALLOCATE(size) os_memory::api::my_allocate(size, 0, __FILE__, __LINE__, false)
	#define ALLOCATE_NOTHROW(size) os_memory::api::my_allocate(size, 0, __FILE__, __LINE__, true)
	#define ALLOCATE_ALIGNED(size, alignment) os_memory::api::my_allocate(size, alignment, __FILE__, __LINE__, false)
	#define ALLOCATE_ALIGNED_NOTHROW(size, alignment) os_memory::api::my_allocate(size, alignment, __FILE__, __LINE__, true)
#else
	#define ALLOCATE(size) os_memory::api::my_allocate(size)
	#define ALLOCATE_NOTHROW(size) os_memory::api::my_allocate(size, 0, nullptr, 0, true)
	#define ALLOCATE_ALIGNED(size, alignment) os_memory::api::my_allocate(size, alignment)
	#define ALLOCATE_ALIGNED_NOTHROW(size, alignment) os_memory::api::my_allocate(size, alignment, nullptr, 0, true)
#endif

#ifdef _DEBUG
	#define DEALLOCATE( this_pointer )																									\
	do																																	\
	{																																	\
		static_assert( std::is_pointer<std::remove_reference_t<decltype(this_pointer)>>::value, "DEALLOCATE requires a pointer type" );	\
		os_memory::api::my_deallocate( this_pointer );																					\
	} while ( 0 )																														
#else																																	
	#define DEALLOCATE( this_pointer )																									\
	do																																	\
	{																																	\
		os_memory::api::my_deallocate( this_pointer );																					\
	} while ( 0 )
#endif

#endif	// GLOBAL_ALLOCATOR_HPP
