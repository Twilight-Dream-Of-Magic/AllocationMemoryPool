/**
 * @file stl_allocator.hpp
 * @brief STL 兼容分配器实现 / STL-compatible allocator implementation
 *
 * @details
 * 1. 基于线程本地的 PoolAllocator 实现 / Implemented using thread-local PoolAllocator
 * 2. 提供可选的不抛出异常接口 / Optional nothrow allocation interface
 * 3. 满足 C++11 及以上 std::allocator 要求 / Compliant with C++11 and above std::allocator requirements
 *
 * 代码风格 / Style Notes:
 * 1. 完全避免缩写：All identifiers use full words.
 * 2. PascalCase 用于类型 / PascalCase for types.
 * 3. snake_case 用于函数和变量 / snake_case for functions and variables.
 * 4. SCREAMING_SNAKE_CASE 用于常量 / SCREAMING_SNAKE_CASE for constants.
 * 5. 中英文注释并存，支持 Doxygen 文档 / Bilingual comments with Doxygen support.
 */

#pragma once
#ifndef STL_ALLOCATOR_HPP
#define STL_ALLOCATOR_HPP

#include "memory_allocators.hpp"  // 包含 PoolAllocator 定义 / Include PoolAllocator definition
#include <cstddef>                  // std::size_t, std::ptrdiff_t
#include <limits>                   // std::numeric_limits
#include <new>                      // std::bad_alloc, std::nothrow
#include <type_traits>              // std::true_type

namespace os_memory::allocator
{

	/// @brief STL兼容分配器模板 / STL-compatible allocator template
	/// @tparam Type 要分配的元素类型 / Type of elements to allocate
	template <typename Type>
	class STL_Allocator
	{
	public:
		/**
		 * @brief 标准模板库约定的类型别名 / Standard Template Library allocator type alias conventions
		 */
		/// @name 标准模板库约定类型变量名 / STL‑convention type alias names
		/// {@
		/// @brief 值类型 / Value type
		using value_type          = Type;
		/// @brief 指针类型 / Pointer type
		using pointer             = Type*;
		/// @brief 常量指针类型 / Const pointer type
		using const_pointer       = const Type*;
		/// @brief void 指针类型 / Void pointer type
		using void_pointer        = void*;
		/// @brief 常量 void 指针类型 / Const void pointer type
		using const_void_pointer  = const void*;
		/// @brief 大小类型 / Size type
		using size_type           = std::size_t;
		/// @brief 差值类型 / Difference type
		using difference_type     = std::ptrdiff_t;
		/// @}

		//c++ 2011 auto generate this
		/// @brief 用于 rebind 的嵌套模板 / Nested template for rebind
		template <typename U>
		struct rebind { using other = STL_Allocator<U>; };

		/// @brief 在移动赋值时传播分配器 / Propagate on container move assignment
		using propagate_on_container_move_assignment = std::true_type;
		/// @brief 所有实例总是相等 / Always equal
		using is_always_equal                     = std::true_type;

		/// @brief 默认构造函数 / Default constructor
		STL_Allocator() noexcept = default;
		/// @brief 拷贝构造函数 / Copy constructor
		template <typename U>
		STL_Allocator(const STL_Allocator<U>&) noexcept {}

		/// @brief 分配内存 / Allocate memory
		/// @param count 要分配的元素个数 / Number of elements to allocate
		/// @return 指向已分配内存的指针 / Pointer to allocated memory
		pointer allocate(size_type count)
		{
			if (count == 0)
				return nullptr;

			if (this->requested_alignment == 0 || (this->requested_alignment & 1) == 1 || !os_memory::memory_pool::is_power_of_two(this->requested_alignment))
			{
				this->requested_alignment = alignof(Type) * alignof(void*);
			}

			void* raw_pointer = get_pool().allocate(count * sizeof(Type), this->requested_alignment, __FILE__, __LINE__, is_nothrow);
			return static_cast<pointer>(raw_pointer);
		}

		/// @brief 释放内存 / Deallocate memory
		/// @param allocated_pointer 已分配的指针 / Pointer to deallocate
		/// @param count 原始分配的元素个数 / Original number of elements allocated
		void deallocate(pointer allocated_pointer, size_type /*count*/) noexcept
		{
			if (!allocated_pointer)
				return;
			get_pool().deallocate(static_cast<void*>(allocated_pointer));
		}

		/// @brief 返回可分配的最大元素数 / Maximum number of elements that can be allocated
		/// @return 可分配的最大元素数 / Max size
		size_type max_size() const noexcept
		{
			return std::numeric_limits<size_type>::max() / sizeof(Type);
		}

		/// @brief 判断是否相等 / Equality comparison
		bool operator==(const STL_Allocator&) const noexcept { return true; }
		/// @brief 判断是否不等 / Inequality comparison
		bool operator!=(const STL_Allocator& other) const noexcept { return !(*this == other); }

		/**
		 * @brief 设置是否为不抛出异常模式 / Set nothrow allocation mode
		 * @param value true 时分配失败返回 nullptr，不抛出异常 / if true, return nullptr on failure instead of throwing
		 */
		void set_nothrow(bool value) noexcept
		{
			this->is_nothrow = value;
		}

		/**
		 * @brief 设置自定义对齐 / Set custom alignment
		 * @param alignment 对齐值（字节），必须为 2 的幂且 >= sizeof(void*) / alignment in bytes, power of two and >= pointer size
		 */
		void set_alignment(size_type alignment) noexcept
		{
			this->requested_alignment = alignment;
		}

	private:
		/// @brief 标记分配模式：是否不抛出异常 / Allocation mode flag: nothrow if true
		bool is_nothrow = false;
		/// @brief 用户请求的对齐 / User requested alignment; 0 表示使用默认对齐 / 0 means use default alignment
		size_type requested_alignment = 0;

		/// @brief 获取线程本地 PoolAllocator 实例 / Get thread-local PoolAllocator instance
		/// @return PoolAllocator 实例 / PoolAllocator instance
		static PoolAllocator& get_pool()
		{
			static thread_local PoolAllocator instance;
			return instance;
		}
	};

}  // namespace os_memory::allocator

#endif  // STL_ALLOCATOR_HPP