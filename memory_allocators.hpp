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
		SystemAllocator() = default;
		~SystemAllocator()
		{
			const uint64_t leaked = os_memory::used_memory_bytes_counter.load( std::memory_order_seq_cst );
			if ( leaked != 0 )
			{
				std::cerr << "[SystemAllocator] Memory leak detected: " << leaked << " bytes still allocated." << std::endl;
			}

			const uint64_t original_point = os_memory::user_operation_counter.load( std::memory_order_seq_cst );
			if ( original_point != 0 )
			{
				std::cerr << "[SystemAllocator] Operation imbalance detected: " << original_point << " net operations (allocs minus frees)." << std::endl;
			}
		}

		void* allocate( size_t size, size_t alignment = alignof( std::max_align_t ), const char* file = nullptr, size_t line = 0, bool nothrow = false ) override
		{
			if ( size == 0 )
			{
				return nullptr;
			}

			// 确保 alignment 为 2 的幂 / alignment must be power of two
			if ( alignment == 0 )
			{
				alignment = sizeof( void* );
			}
			assert( ( alignment & ( alignment - 1 ) ) == 0 && "alignment must be power of two" );

			// 从操作系统请求内存 / request memory from OS
			void* raw_pointer = os_memory::allocate_tracked( size, alignment );
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

			os_memory::deallocate_tracked( pointer, allocated_size );
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
		PoolAllocator() = default;

		//────────────────────────────────────────────────────────────
		// 分配 / allocate
		//────────────────────────────────────────────────────────────
		void* allocate( size_t size, size_t alignment = alignof( void* ), const char* file = nullptr, size_t line = 0, bool nothrow = false ) override
		{
			if ( size == 0 )
				return nullptr;

			// 交给 MemoryPool 进行真实分配与对齐
			void* user_pointer = memory_pool_.allocate( size, alignment, file, line, nothrow );
			if ( !user_pointer )
			{
				if ( !nothrow )
					throw std::bad_alloc();
				return nullptr;
			}

			if ( leak_detection_enabled_ )
			{
				MemoryTracker::instance().track_allocation( user_pointer, size, file, line );
			}
			else
			{
				insert_mapping( user_pointer, user_pointer );
			}

			return user_pointer;
		}

		//────────────────────────────────────────────────────────────
		// 释放 / deallocate
		//────────────────────────────────────────────────────────────
		void deallocate( void* user_pointer ) override
		{
			if ( !user_pointer )
				return;

			if ( leak_detection_enabled_ )
			{
				MemoryTracker::instance().track_deallocation( user_pointer );
			}
			else
			{
				remove_mapping( user_pointer );
			}

			memory_pool_.deallocate( user_pointer );
		}

		//────────────────────────────────────────────────────────────
		// 泄漏检测接口
		//────────────────────────────────────────────────────────────
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

		//────────────────────────────────────────────────────────────
		// 析构：提示未释放
		//────────────────────────────────────────────────────────────
		~PoolAllocator()
		{
			if ( !leak_detection_enabled_ && mapping_root_ != nullptr )
			{
				size_t count = count_mappings( mapping_root_ );
				if ( count != 0 )
				{
					std::cerr << "[PoolAllocator] WARNING: " << count << " allocations not freed\n";
				}
			}
		}

	private:
		//────────────────────────────────────────────────────────────
		// 成员
		//────────────────────────────────────────────────────────────
		MemoryPool memory_pool_;  //!< 线程本地 MemoryPool

		bool leak_detection_enabled_ = false;
		bool detailed_tracking_enabled_ = false;

		struct TreeNode
		{
			std::uintptr_t address_key;		 //!< 用地址整数做排序键
			void*		   aligned_pointer;	 //!< 对齐指针（也是 key 原值）
			void*		   raw_pointer;		 //!< 原始指针（当前与 aligned 相同）
			TreeNode*	   left_child;
			TreeNode*	   right_child;

			~TreeNode() = default;
		};

		TreeNode*				  mapping_root_ = nullptr;
		mutable std::shared_mutex mapping_mutex_;  //!< 保护整棵树的互斥锁

		//────────────────────────────────────────────────────────────
		// 插入映射
		//────────────────────────────────────────────────────────────
		void insert_mapping( void* aligned_pointer, void* raw_pointer )
		{
			std::uintptr_t						key = reinterpret_cast<std::uintptr_t>( aligned_pointer );
			std::unique_lock<std::shared_mutex> lock( mapping_mutex_ );
			mapping_root_ = insert_node( mapping_root_, key, aligned_pointer, raw_pointer );
		}

		TreeNode* insert_node( TreeNode* node, std::uintptr_t key, void* aligned_pointer, void* raw_pointer )
		{
			if ( node == nullptr )
			{
				void* storage = ::operator new( sizeof( TreeNode ) );
				auto* new_node = new ( storage ) TreeNode { key, aligned_pointer, raw_pointer, nullptr, nullptr };
				return new_node;
			}

			if ( key < node->address_key )
				node->left_child = insert_node( node->left_child, key, aligned_pointer, raw_pointer );
			else if ( key > node->address_key )
				node->right_child = insert_node( node->right_child, key, aligned_pointer, raw_pointer );
			// key 相等：已存在，不重复插入
			return node;
		}

		//────────────────────────────────────────────────────────────
		// 移除映射
		//────────────────────────────────────────────────────────────
		void remove_mapping( void* aligned_pointer )
		{
			std::uintptr_t						key = reinterpret_cast<std::uintptr_t>( aligned_pointer );
			std::unique_lock<std::shared_mutex> lock( mapping_mutex_ );
			mapping_root_ = remove_node( mapping_root_, key );
		}

		TreeNode* remove_node( TreeNode* node, std::uintptr_t key )
		{
			if ( node == nullptr )
			{
				std::cerr << "Warning: deallocating untracked pointer " << reinterpret_cast<void*>( key ) << '\n';
				return nullptr;
			}

			if ( key < node->address_key )
			{
				node->left_child = remove_node( node->left_child, key );
			}
			else if ( key > node->address_key )
			{
				node->right_child = remove_node( node->right_child, key );
			}
			else
			{
				// 找到节点，删除
				if ( node->left_child == nullptr || node->right_child == nullptr )
				{
					TreeNode* child = node->left_child ? node->left_child : node->right_child;
					node->~TreeNode();
					::operator delete( node );
					return child;
				}
				else  // 两个子节点都存在：找右子树最小节点替换
				{
					TreeNode* successor = node->right_child;
					while ( successor->left_child )
						successor = successor->left_child;

					node->address_key = successor->address_key;
					node->aligned_pointer = successor->aligned_pointer;
					node->raw_pointer = successor->raw_pointer;
					node->right_child = remove_node( node->right_child, successor->address_key );
				}
			}
			return node;
		}

		//────────────────────────────────────────────────────────────
		// 统计映射数量
		//────────────────────────────────────────────────────────────
		size_t count_mappings( TreeNode* node ) const
		{
			if ( node == nullptr )
				return 0;
			return 1 + count_mappings( node->left_child ) + count_mappings( node->right_child );
		}
	};


}  // namespace os_memory::allocator

#endif	// MEMORY_ALLOCATORS_HPP
