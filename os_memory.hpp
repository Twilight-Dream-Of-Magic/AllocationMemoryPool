#pragma once
#ifndef OS_MEMORY_HPP
#define OS_MEMORY_HPP

/*--------------------------------- 系统头文件 / System Headers -------------*/
#if defined( _WIN32 )
#include <windows.h>
#include <winternl.h>
#define NOMINMAX

#elif defined( __linux__ )
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <linux/mman.h>	 // MAP_HUGETLB
#endif

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cassert>
#include <iostream>

namespace os_memory
{

	/**  
    * @brief 自定义异常类：bad_dealloc  
    * @details 用于表示内存释放失败的异常  
    */
	class bad_dealloc : public std::exception
	{
	public:
		bad_dealloc() 
		: 
		exception("bad deallocation", 1) 
		{
		}

		explicit bad_dealloc(const std::string& message)
		:
		exception(message.c_str(), 1)
		{
		}

		explicit bad_dealloc(const char* message)
		:
		exception(message, 1)
		{
		}
	};


	/*--------------------------------- Linux实现 / Linux Implementation -------*/
#if defined( __linux__ )

	/**
	 * @brief 分配虚拟内存 / Allocate virtual memory
	 * @param size 请求字节数 / requested byte count
	 * @param alignment 内存对齐要求 / memory alignment requirement
	 * @return 分配的内存地址 / allocated memory address
	 * 
	 * @details
	 * 1. 使用直接系统调用绕过libc / Uses direct syscall bypassing libc
	 * 2. 大页支持：对齐>4KB时启用巨页 / Hugepage support for alignments >4KB
	 * 3. 返回原始内存指针 / Returns raw memory pointer
	 */
	inline void* allocate_memory( size_t size, size_t alignment = alignof(std::max_align_t) )
	{
		// 配置内存映射标志 / Configure mmap flags
		const long flags = MAP_PRIVATE | MAP_ANONYMOUS | ( ( alignment > 0x1000 ) ? MAP_HUGETLB : 0 );

		// 直接系统调用：避免libc开销 / Direct syscall: avoids libc overhead
		const long result = syscall
		(
			SYS_mmap,
			nullptr,				// 地址提示 / address hint
			size,					// 分配大小 / allocation size
			PROT_READ | PROT_WRITE, // 读写权限 / read-write protection
			flags,					// 映射标志 / mapping flags
			-1,						// 文件描述符 / file descriptor
			0						// 偏移量 / offset
		);

		// 错误处理：直接输出原始错误 / Error handling: outputs raw errno
		if ( result < 0 )
		{
			std::cerr << "[Geek] mmap failure: errno=" << errno << " (" << strerr( errno ) << ")\n";
			return nullptr;
		}

		return reinterpret_cast<void*>( result );
	}

	/**
	 * @brief 释放虚拟内存 / Deallocate virtual memory
	 * @param raw_pointer 原始内存指针 / raw memory pointer
	 * @param size 释放字节数 / deallocation size
	 * @return 操作是否成功 / operation success status
	 */
	inline bool deallocate_memory( void* raw_pointer, size_t size )
	{
		// 直接系统调用解除映射 / Direct syscall unmapping
		const long result = syscall
		(
			SYS_munmap,
			raw_pointer, // 目标地址 / target address
			size		 // 释放大小 / deallocation size
		);

		// 错误处理 / Error handling
		if ( result < 0 )
		{
			std::cerr << "[Geek] munmap failure: errno=" << errno << " (" << strerr( errno ) << ")\n";
			return false;
		}

		return true;
	}

	/*--------------------------------- Windows实现 / Windows Implementation ---*/
#elif defined( _WIN32 )

	// NT API函数原型 / NT API function prototypes
	using NTAllocateVirtualMemoryType = NTSTATUS( NTAPI* )( HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG );
	using NTFreeVirtualMemoryType = NTSTATUS( NTAPI* )( HANDLE, PVOID*, PSIZE_T, ULONG );

	/**
	 * @brief 获取NT内存分配函数 / Retrieve NT memory allocation function
	 * @return 函数指针 / function pointer
	 * 
	 * @note 延迟加载NTDLL / Lazy-loads ntdll.dll
	 */
	inline NTAllocateVirtualMemoryType get_nt_allocate_function()
	{
		static auto resolver = []() -> NTAllocateVirtualMemoryType {
			const HMODULE ntdll_module = GetModuleHandleW( L"ntdll.dll" );
			if ( !ntdll_module )
			{
				std::cerr << "[Geek] Failed to locate ntdll.dll\n";
				return nullptr;
			}
			return reinterpret_cast<NTAllocateVirtualMemoryType>( GetProcAddress( ntdll_module, "NtAllocateVirtualMemory" ) );
		}();
		return resolver;
	}

	/**
	 * @brief 获取NT内存释放函数 / Retrieve NT memory deallocation function
	 * @return 函数指针 / function pointer
	 */
	inline NTFreeVirtualMemoryType get_nt_free_function()
	{
		static auto resolver = []() -> NTFreeVirtualMemoryType {
			const HMODULE ntdll_module = GetModuleHandleW( L"ntdll.dll" );
			if ( !ntdll_module )
			{
				std::cerr << "[Geek] Failed to locate ntdll.dll\n";
				return nullptr;
			}
			return reinterpret_cast<NTFreeVirtualMemoryType>( GetProcAddress( ntdll_module, "NtFreeVirtualMemory" ) );
		}();
		return resolver;
	}

	/**
	 * @brief 分配虚拟内存 / Allocate virtual memory
	 * @param size 请求字节数 / requested byte count
	 * @param alignment 内存对齐要求 / memory alignment requirement
	 * @return 分配的内存地址 / allocated memory address
	 * 
	 * @details
	 * 1. 使用NT系统调用绕过Win32 API / Uses NT syscall bypassing Win32 API
	 * 2. 大页支持：对齐>4KB时启用大页 / Large page support for alignments >4KB
	 */
	inline void* allocate_memory( size_t size, size_t alignment = alignof(std::max_align_t) )
	{
		void*  base_address = nullptr;
		SIZE_T allocation_size = size;

		// 配置内存分配标志 / Configure allocation flags
		ULONG allocation_type = MEM_RESERVE | MEM_COMMIT;
		if ( alignment > 0x1000 )
		{
			allocation_type |= MEM_LARGE_PAGES;	 // 大页分配 / huge page allocation
		}

		// 调用NT内存分配函数 / Invoke NT memory allocation
		const NTSTATUS status = get_nt_allocate_function()
		(
			GetCurrentProcess(),  // 当前进程 / current process
			&base_address,		  // 返回地址 / return address
			0,					  // 零位掩码 / zero bits mask
			&allocation_size,	  // 分配大小 / allocation size
			allocation_type,	  // 分配类型 / allocation type
			PAGE_READWRITE		  // 内存保护 / memory protection
		);

		return NT_SUCCESS( status ) ? base_address : nullptr;
	}

	/**
	 * @brief 释放虚拟内存 / Deallocate virtual memory
	 * @param raw_pointer 原始内存指针 / raw memory pointer
	 * @param size 释放字节数 / deallocation size
	 * @return 操作是否成功 / operation success status
	 */
	inline bool deallocate_memory( void* raw_pointer, size_t size )
	{
		SIZE_T		   deallocation_size = size;
		const NTSTATUS status = get_nt_free_function()
		(
			GetCurrentProcess(),  // 当前进程 / current process
			&raw_pointer,		  // 目标地址 / target address
			&deallocation_size,	  // 释放大小 / deallocation size
			MEM_RELEASE			  // 释放类型 / release type
		);

		return NT_SUCCESS( status );
	}

#endif	// 平台选择结束 / End of platform selection

}  // namespace os_memory



#endif	// OS_MEMORY_HPP