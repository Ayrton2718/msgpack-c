//
// MessagePack for C++ memory pool
//
// Copyright (C) 2008-2013 FURUHASHI Sadayuki and KONDO Takatoshi
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.
//
#ifndef MSGPACK_CPP11_ZONE_HPP
#define MSGPACK_CPP11_ZONE_HPP

#include <cstdlib>
#include <memory>
#include <vector>

#include "msgpack/cpp_config.hpp"

#ifndef MSGPACK_ZONE_CHUNK_SIZE
#define MSGPACK_ZONE_CHUNK_SIZE 8192
#endif

#ifndef MSGPACK_ZONE_ALIGN
#define MSGPACK_ZONE_ALIGN sizeof(int)
#endif

namespace msgpack {

class zone {
private:
	struct finalizer {
		finalizer(void (*func)(void*), void* data):m_func(func), m_data(data) {}
		void operator()() { m_func(m_data); }
		void (*m_func)(void*);
		void* m_data;
	};
	struct finalizer_array {
		finalizer_array():m_tail(nullptr), m_end(nullptr), m_array(nullptr) {}
		void call() {
			finalizer* fin = m_tail;
			for(; fin != m_array; --fin) (*(fin-1))();
		}
		~finalizer_array() {
			call();
			::free(m_array);
		}
		void clear() {
			call();
			m_tail = m_array;
		}
		void push(void (*func)(void* data), void* data)
		{
			finalizer* fin = m_tail;

			if(fin == m_end) {
				push_expand(func, data);
				return;
			}

			fin->m_func = func;
			fin->m_data = data;

			++m_tail;
		}
		void push_expand(void (*func)(void*), void* data) {
			const size_t nused = m_end - m_array;
			size_t nnext;
			if(nused == 0) {
				nnext = (sizeof(finalizer) < 72/2) ?
					72 / sizeof(finalizer) : 8;
			} else {
				nnext = nused * 2;
			}
			finalizer* tmp =
				static_cast<finalizer*>(::realloc(m_array, sizeof(finalizer) * nnext));
			if(!tmp) {
				throw std::bad_alloc();
			}
			m_array	= tmp;
			m_end	= tmp + nnext;
			m_tail	= tmp + nused;
			new (m_tail) finalizer(func, data);

			++m_tail;
		}
		finalizer* m_tail;
		finalizer* m_end;
		finalizer* m_array;
	};
	struct chunk {
		chunk* m_next;
	};
	struct chunk_list {
		chunk_list(size_t chunk_size)
		{
			chunk* c = static_cast<chunk*>(::malloc(sizeof(chunk) + chunk_size));
			if(!c) {
				throw std::bad_alloc();
			}

			m_head = c;
			m_free = chunk_size;
			m_ptr  = reinterpret_cast<char*>(c) + sizeof(chunk);
			c->m_next = nullptr;
		}
		~chunk_list()
		{
			chunk* c = m_head;
			while(true) {
				chunk* n = c->m_next;
				::free(c);
				if(n) {
					c = n;
				} else {
					break;
				}
			}
		}
		void clear(size_t chunk_size)
		{
			chunk* c = m_head;
			while(true) {
				chunk* n = c->m_next;
				if(n) {
					::free(c);
					c = n;
				} else {
					m_head = c;
					break;
				}
			}
			m_head->m_next = nullptr;
			m_free = chunk_size;
			m_ptr  = reinterpret_cast<char*>(m_head) + sizeof(chunk);
		}
		size_t m_free;
		char* m_ptr;
		chunk* m_head;
	};
	size_t m_chunk_size;
	chunk_list m_chunk_list;
	finalizer_array m_finalizer_array;

public:
	zone(size_t chunk_size = MSGPACK_ZONE_CHUNK_SIZE) noexcept;

public:
	static zone* create(size_t chunk_size);
	static void destroy(zone* zone);
	void* allocate_align(size_t size);
	void* allocate_no_align(size_t size);

	void push_finalizer(void (*func)(void*), void* data);

	template <typename T>
	void push_finalizer(msgpack::unique_ptr<T> obj);

	void clear();

	void swap(zone& o);


	static void* operator new(std::size_t size) throw(std::bad_alloc)
	{
		void* p = ::malloc(size);
		if (!p) throw std::bad_alloc();
		return p;
	}
	static void operator delete(void *p) throw()
	{
		::free(p);
	}
	static void* operator new(std::size_t size, void* mem) throw()
	{
		return mem;
	}
	static void operator delete(void *p, void* mem) throw()
	{
	}

	template <typename T, typename... Args>
	T* allocate(Args... args);

private:
	void undo_allocate(size_t size);

	template <typename T>
	static void object_destructor(void* obj);

	void* allocate_expand(size_t size);
};

inline zone* zone::create(size_t chunk_size)
{
	zone* z = static_cast<zone*>(::malloc(sizeof(zone) + chunk_size));
	if (!z) {
		return nullptr;
	}
	new (z) zone(chunk_size);
	return z;
}

inline void zone::destroy(zone* z)
{
	z->~zone();
	::free(z);
}

inline zone::zone(size_t chunk_size) noexcept:m_chunk_size(chunk_size), m_chunk_list(m_chunk_size)
{
}

inline void* zone::allocate_align(size_t size)
{
	return allocate_no_align(
		((size)+((MSGPACK_ZONE_ALIGN)-1)) & ~((MSGPACK_ZONE_ALIGN)-1));
}

inline void* zone::allocate_no_align(size_t size)
{
	if(m_chunk_list.m_free < size) {
		return allocate_expand(size);
	}

	char* ptr = m_chunk_list.m_ptr;
	m_chunk_list.m_free -= size;
	m_chunk_list.m_ptr  += size;

	return ptr;
}

inline void* zone::allocate_expand(size_t size)
{
	chunk_list* const cl = &m_chunk_list;

	size_t sz = m_chunk_size;

	while(sz < size) {
		sz *= 2;
	}

	chunk* c = static_cast<chunk*>(::malloc(sizeof(chunk) + sz));
	if (!c) return nullptr;

	char* ptr = reinterpret_cast<char*>(c) + sizeof(chunk);

	c->m_next  = cl->m_head;
	cl->m_head = c;
	cl->m_free = sz - size;
	cl->m_ptr  = ptr + size;

	return ptr;
}

inline void zone::push_finalizer(void (*func)(void*), void* data)
{
	m_finalizer_array.push(func, data);
}

template <typename T>
inline void zone::push_finalizer(msgpack::unique_ptr<T> obj)
{
	m_finalizer_array.push(&zone::object_destructor<T>, obj.get());
	obj.release();
}

inline void zone::clear()
{
	m_finalizer_array.clear();
	m_chunk_list.clear(m_chunk_size);
}

inline void zone::swap(zone& o)
{
	std::swap(*this, o);
}

template <typename T>
void zone::object_destructor(void* obj)
{
	reinterpret_cast<T*>(obj)->~T();
}

inline void zone::undo_allocate(size_t size)
{
	m_chunk_list.m_ptr  -= size;
	m_chunk_list.m_free += size;
}


template <typename T, typename... Args>
T* zone::allocate(Args... args)
{
	void* x = allocate_align(sizeof(T));
	try {
		m_finalizer_array.push(&zone::object_destructor<T>, x);
	} catch (...) {
		undo_allocate(sizeof(T));
		throw;
	}
	try {
		return new (x) T(args...);
	} catch (...) {
		--m_finalizer_array.m_tail;
		undo_allocate(sizeof(T));
		throw;
	}
}

}  // namespace msgpack

#endif // MSGPACK_CPP11_ZONE_HPP