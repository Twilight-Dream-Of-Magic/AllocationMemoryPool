/**
 * @file memory_allocators.hpp
 * @brief 系统和池内存分配器接口与实现 / System and pool allocator interfaces and implementations
 *
 * @details
 * 1. 定义通用分配器接口 / Define general allocator interface;
 * 2. 基于 OS 内存请求的 SystemAllocator 实现 / SystemAllocator implementation using OS memory APIs;
 * 3. 基于线程本地 MemoryPool 的 PoolAllocator 实现 / PoolAllocator implementation using thread-local MemoryPool.
 *
 * 代码风格说明 / Style Notes
 * ---------------------------------------------------------------------------
 * 1. 完全避免缩写：所有标识符使用完整单词（pointer, alignment, allocator...）；
 * 2. PascalCase 用于类型 / PascalCase for types;
 * 3. snake_case 用于函数、变量 / snake_case for functions and variables;
 * 4. SCREAMING_SNAKE_CASE 用于常量 / SCREAMING_SNAKE_CASE for constants;
 * 5. 中英文注释并存，支持 Doxygen 文档 / Bilingual comments with Doxygen support.
 */

#pragma once
#ifndef MEMORY_ALLOCATORS_HPP
#define MEMORY_ALLOCATORS_HPP

#include "os_memory.hpp"
#include "memory_tracker.hpp"
#include "memory_pool.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <new>
#include <string>
#include <unordered_map>

namespace os_memory::allocator
{
	/// @brief 通用分配器接口 / General allocator interface
	class InterfaceAllocator
	{
	public:
		virtual ~InterfaceAllocator() = default;

		/**
     * @brief 分配内存 / Allocate memory
     * @param size           请求的字节数 / number of bytes requested
     * @param alignment      对齐要求（字节）/ alignment in bytes
     * @param file           源文件名（可选，用于追踪）/ source file (optional, for tracking)
     * @param line           源代码行号（可选，用于追踪）/ source line (optional, for tracking)
     * @param nothrow        分配失败时是否抛出异常 / throw on failure if false
     * @return 指向已分配内存的指针，如果失败则返回 nullptr / pointer to allocated memory or nullptr
     */
		virtual void* allocate( size_t size, size_t alignment = sizeof( void* ), const char* file = nullptr, size_t line = 0, bool nothrow = false ) = 0;

		/**
     * @brief 释放内存 / Deallocate memory
     * @param pointer        先前分配的指针 / pointer previously allocated
     */
		virtual void deallocate( void* pointer ) = 0;

		/**
     * @brief 启用或禁用内存泄露检测 / Enable or disable leak detection
     * @param detailed       是否开启详细追踪 / detailed tracking if true
     */
		virtual void enable_leak_detection( bool detailed ) = 0;

		/// @brief 报告内存泄露 / Report detected leaks
		virtual void report_leaks() = 0;

		/**
     * @brief 获取当前内存使用量 / Get current memory usage
     * @return 已分配的总字节数 / total bytes currently allocated
     */
		virtual size_t current_memory_usage() = 0;
	};

	/// @brief 基于操作系统内存请求的分配器 / Allocator using OS memory APIs
	class SystemAllocator : public InterfaceAllocator
	{
	public:
		void* allocate( size_t size, size_t alignment = alignof( std::max_align_t ), const char* file = nullptr, size_t line = 0, bool nothrow = false ) override
		{
			// 确保 alignment 为 2 的幂 / alignment must be power of two
			if ( alignment == 0 )
			{
				alignment = sizeof( void* );
			}
			assert( ( alignment & ( alignment - 1 ) ) == 0 && "alignment must be power of two" );

			// 从操作系统请求内存 / request memory from OS
			void* raw_pointer = os_memory::allocate_memory( size, alignment );
			if ( raw_pointer == nullptr )
			{
				if ( !nothrow )
				{
					throw std::bad_alloc();
				}
				std::cerr << "OS allocation failed (" << size << " bytes)" << ( file ? std::string { " @ " } + file + ":" + std::to_string( line ) : "" ) << '\n';
				return nullptr;
			}

			// 泄露检测模式 / leak detection mode
			if ( leak_detection_enabled_ )
			{
				MemoryTracker::instance().track_allocation( raw_pointer, size, file, line );
			}
			else
			{
				// 记录指针和大小 / record pointer and size
				pointer_map_[ raw_pointer ] = size;
			}

			return raw_pointer;
		}

		void deallocate( void* pointer ) override
		{
			if ( pointer == nullptr )
			{
				return;
			}

			// 泄露检测模式 / leak detection mode
			if ( leak_detection_enabled_ )
			{
				MemoryTracker::instance().track_deallocation( pointer );
			}

			auto it = pointer_map_.find( pointer );
			if ( it == pointer_map_.end() )
			{
				return;
			}

			size_t allocated_size = it->second;
			pointer_map_.erase( it );

			os_memory::deallocate_memory( pointer, allocated_size );
		}

		void enable_leak_detection( bool detailed ) override
		{
			leak_detection_enabled_ = true;
			detailed_tracking_enabled_ = detailed;
			MemoryTracker::instance().enable( detailed );
		}

		void report_leaks() override
		{
			MemoryTracker::instance().report_leaks();
		}

		size_t current_memory_usage() override
		{
			return MemoryTracker::instance().current_memory_usage();
		}

	private:
		bool							  leak_detection_enabled_ = false;	   //!< 是否启用泄露检测 / leak detection enabled
		bool							  detailed_tracking_enabled_ = false;  //!< 是否启用详细追踪 / detailed tracking enabled
		std::unordered_map<void*, size_t> pointer_map_;						   //!< 原始指针→分配大小映射 / raw pointer → size map
	};

	/// @brief 基于 MemoryPool 的分配器 / Allocator using MemoryPool
	class PoolAllocator : public InterfaceAllocator
	{
	public:
		void* allocate( size_t size, size_t alignment = sizeof( void* ), const char* file = nullptr, size_t line = 0, bool nothrow = false ) override
		{
			if ( alignment == 0 )
			{
				alignment = alignof( std::max_align_t );
			}

			/* ------- 情况 A：对齐需求 ≤ 16 ------- */
			if (alignment <= DEFAULT_ALIGNMENT)
			{
				void* p = memory_pool_.allocate(size, alignment, file, line, nothrow);
				if (!p) return nullptr;

				if (leak_detection_enabled_)
					MemoryTracker::instance().track_allocation(p, size, file, line);
				else
					aligned_to_raw_map_[p] = p;      // raw==aligned

				return p;
			}

			/* ------- 情况 B：对齐需求 > 16，需要额外 reserve ------- */
			size_t reserve_size = size + alignment - 1;

			void* raw_pointer = memory_pool_.allocate(reserve_size, alignment, file, line, nothrow);
			if (!raw_pointer) return nullptr;

			void* aligned_pointer = std::align(alignment, size, raw_pointer, reserve_size);
			if (!aligned_pointer)                                  // 理论上不会发生
			{
				memory_pool_.deallocate(raw_pointer);
				if (!nothrow) throw std::bad_alloc();
				return nullptr;
			}

			if ( leak_detection_enabled_ )
			{
				MemoryTracker::instance().track_allocation( aligned_pointer, size, file, line, raw_pointer );
			}
			else
			{
				aligned_to_raw_map_[ aligned_pointer ] = raw_pointer;
			}

			return aligned_pointer;
		}

		void deallocate( void* aligned_pointer ) override
		{
			if ( aligned_pointer == nullptr )
			{
				return;
			}

			if ( leak_detection_enabled_ )
			{
				MemoryTracker& tracker = MemoryTracker::instance();
				void*		   raw_pointer = tracker.find_tracked_pointer( aligned_pointer );
				if ( raw_pointer )
				{
					// remove the “aligned” entry
					tracker.track_deallocation(aligned_pointer);
					// also remove the underlying “reserve_size” entry
					tracker.track_deallocation(raw_pointer);
					memory_pool_.deallocate( raw_pointer );
					return;
				}
				std::cerr << "Deallocating untracked pointer (leak detection): " << aligned_pointer << '\n';
				return;
			}

			auto it = aligned_to_raw_map_.find( aligned_pointer );
			if ( it == aligned_to_raw_map_.end() )
			{
				std::cerr << "Deallocating untracked pointer: " << aligned_pointer << '\n';
				return;
			}

			void* raw_pointer = it->second;
			aligned_to_raw_map_.erase( it );
			memory_pool_.deallocate( raw_pointer );
		}

		void enable_leak_detection( bool detailed ) override
		{
			leak_detection_enabled_ = true;
			detailed_tracking_enabled_ = detailed;
			MemoryTracker::instance().enable( detailed );
		}

		void report_leaks() override
		{
			MemoryTracker::instance().report_leaks();
		}

		size_t current_memory_usage() override
		{
			return MemoryTracker::instance().current_memory_usage();
		}

		~PoolAllocator()
		{
			if ( !leak_detection_enabled_ && !aligned_to_raw_map_.empty() )
			{
				std::cerr << "WARNING: " << aligned_to_raw_map_.size() << " allocations not freed\n";
			}
		}

	private:
		MemoryPool memory_pool_;  //!< 线程本地 MemoryPool 实例 / thread-local MemoryPool instance

		bool leak_detection_enabled_ = false;	  //!< 是否启用泄露检测 / leak detection enabled
		bool detailed_tracking_enabled_ = false;  //!< 是否启用详细追踪 / detailed tracking enabled

		std::unordered_map<void*, void*> aligned_to_raw_map_;  //!< 对齐指针→原始指针映射 / aligned pointer → raw pointer map
	};
}  // namespace os_memory::allocator

#endif	// MEMORY_ALLOCATORS_HPP
