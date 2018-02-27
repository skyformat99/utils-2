// #define BSP_OBJECT_POOL_LOG_ERROR(message) to log errors
// To customise the behaviour of the object pool
// supply an object pool policy as a template parameter.
// See bsp::detail::default_object_pool_policy for the format.

#ifndef BSP_OBJECT_POOL_H
#define BSP_OBJECT_POOL_H

#include <array>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <ostream>
#include <stdexcept>

#include "storage_pool.h"

namespace bsp {

namespace detail {

template <class object_pool>
class object_pool_iterator: public std::iterator<std::input_iterator_tag, typename object_pool::value_type> {
public:
	using value_type 	  = typename object_pool::value_type;
	using reference 	  = typename object_pool::reference;
	using const_reference = typename object_pool::const_reference;
	using size_type		  = typename object_pool::size_type;

public:
	object_pool_iterator(object_pool& array, size_type index);
	object_pool_iterator& operator++();
	object_pool_iterator operator++(int){ object_pool_iterator tmp(*this); ++(*this); return tmp; }
	bool operator==(const object_pool_iterator& rhs) const;
	bool operator!=(const object_pool_iterator& rhs) const;
	reference operator*();
	const_reference operator*() const;
private:
	object_pool& object_pool_;
	storage_pool<value_type>& storage_pool_;
	size_type i_  = 0;
	size_type di_ = 0;
	const typename storage_pool<value_type>::storage_type* db_ = nullptr;
	
	template <typename T> friend class object_pool_const_iterator;	
};

template <class object_pool>
class object_pool_const_iterator: public std::iterator<std::input_iterator_tag, typename object_pool::value_type> {
public:
	using value_type = typename object_pool::value_type;
	using reference  = typename object_pool::const_reference;
	using size_type  = typename object_pool::size_type;

public:
	object_pool_const_iterator(const object_pool& array, size_type index);
	object_pool_const_iterator(const object_pool_const_iterator&) = default;
	object_pool_const_iterator(const object_pool_iterator<object_pool>& it):object_pool_(it.object_pool_), storage_pool_(it.storage_pool_), i_(it.i_), di_(it.di_), db_(it.db_){}
	object_pool_const_iterator& operator++();
	object_pool_const_iterator operator++(int){ object_pool_const_iterator tmp(*this); ++(*this); return tmp; }
	bool operator==(const object_pool_const_iterator& rhs) const;
	bool operator!=(const object_pool_const_iterator& rhs) const;
	reference operator*() const;
private:
	const object_pool& object_pool_;
	const storage_pool<value_type>& storage_pool_;
	size_type i_  = 0;
	size_type di_ = 0;
	const typename storage_pool<value_type>::storage_type* db_ = nullptr;
};
} // namespace detail

// Personal note: my object_is_valid() will be something like:
// value.id!=InvalidId && value.enabled() && !value.toBeRemoved()

namespace detail {
	template <typename T, typename ID>
	struct default_object_pool_policy {
		static const bool store_id_in_object = false;
		static const bool shrink_after_clear = false;
		static bool is_object_iterable(const T&){ return true; }
		static void set_object_id(T&, const ID&){}
		static ID get_object_id(const T&){return 0;}
	};
}

class object_pool_base {
public:
	virtual ~object_pool_base() = default;
	virtual void clear() = 0;
};

// A pool that stores objects in contiguous arrays.
// Requirements: 
// - ID must be castable to and from a uint32_t
// Customisation:
// - TODO
// Reference: Code is heavily inspired by Bitsquid.
template<typename T, typename ID = uint32_t, class ObjectPolicy = detail::default_object_pool_policy<T, ID>> class object_pool : public object_pool_base {
public:
	using id_type = ID;
	using value_type = T;
	using reference = T&;
	using const_reference = const T&;
	using pointer = T*;
	using size_type = int;
	using iterator = detail::object_pool_iterator<object_pool>;
	using const_iterator = detail::object_pool_const_iterator<object_pool>;
	using object_policy = ObjectPolicy;

public:
	// Construct an object pool (requires size <= max_size())
	explicit object_pool(size_type size)
	:initial_capacity_{size}, capacity_{size}, objects_{size}{
		if (size > max_size()) throw std::length_error("object_pool: constructor size too large");
		clear();
	}

	~object_pool() final override {
		for (size_type i = 0; i < num_objects_; i++) {
			destroy(objects_[i]);
		}
	}

	object_pool(const object_pool&) = delete;
	object_pool& operator=(const object_pool&) = delete;
	
	std::pair<id_type, pointer> construct(const T& value = T()) {
		index_type& in = new_index();
		T* nv = new (&objects_[in.index]) T(value);
		if (object_policy::store_id_in_object){
			object_policy::set_object_id(*nv, in.id);
		}
		return { in.id, nv };
	}

	template <typename U>
	std::pair<id_type, pointer> construct(U&& value) {
		index_type& in = new_index();
		T* nv = new (&objects_[in.index]) T(std::forward<U>(value));
		if (object_policy::store_id_in_object){
			object_policy::set_object_id(*nv, in.id);
		}
		return { in.id, nv };
	}

	template<class... Args>
	std::pair<id_type, pointer> construct(Args&&... args) {
		index_type& in = new_index();
		T* nv = new (&objects_[in.index]) T(std::forward<Args>(args)...);
		if (object_policy::store_id_in_object){
			object_policy::set_object_id(*nv, in.id);
		}
		return { in.id, nv };
	}

	size_type count(id_type id) const {
		const index_type& in = index(id);
		return (in.id == id && in.index != USHRT_MAX) ? 1 : 0;
	}

	reference operator[](id_type id) {
		return objects_[index(id).index];
	}

	const_reference operator[](id_type id) const {
		return objects_[index(id).index];
	}

	reference front() { return objects_[0]; }

	const_reference front() const { return objects_[0]; }

	const storage_pool<T>& objects() const { return objects_; }

	inline bool empty() const { return size() == 0; }

	// Returns the number of objects (invalid or valid)
	inline size_type size() const { return num_objects_; }
	
	inline size_type capacity() const { return capacity_; }

	static constexpr size_type max_size() { return max_size_; }

	void remove(id_type id) {
		index_type& in = index(id);
		assert(in.id == id);

		// increment id to avoid conflicts
		const uint32_t id_increment = 0x10000;
		in.id = id_type { static_cast<uint32_t>(id) + id_increment };

		T& target = objects_[in.index];
		if (object_policy::store_id_in_object){
			ID target_id = object_policy::get_object_id(target);
			assert(target_id == id);
		}
		destroy(target);
		if (in.index != num_objects_ - 1) {
			move_back_into(target, in);
		}
		num_objects_--;

		// Update enqueue
		in.index = std::numeric_limits<uint16_t>::max();
		indices_[freelist_enque_].next = mask_index(id);
		freelist_enque_ = mask_index(id);
	}

	// TODO:
	void pop_back() {

	}

	void clear() final override {
		for (size_type i = 0; i < num_objects_; i++) {
			destroy(objects_[i]);
		}
		num_objects_ = 0;
		for (size_type i = 0; i < max_size(); ++i) {
			auto& index = indices_[i];
			index.id = static_cast<id_type>(i);
			index.next = static_cast<uint16_t>(i + 1);
			index.index = std::numeric_limits<uint16_t>::max();
		}
		freelist_deque_ = 0;
		freelist_enque_ = static_cast<uint16_t>(capacity_ - 1);

		if (object_policy::shrink_after_clear){
			while (objects_.storage_count() > 1){
				const auto& storage = objects_.storage(objects_.storage_count() - 1);
				auto count = storage.count;
				objects_.deallocate();
				capacity_ -= count;
			}
			assert(capacity_ == initial_capacity_);
		}
	}

	iterator begin() { 
		auto it = iterator(*this, 0);
		auto end_ = end();
		while (!object_policy::is_object_iterable(*it) && it != end_){
			++it;
		}
		return it;
	}

	iterator end() { return iterator(*this, size()); }

	const_iterator begin() const { 
		auto it = const_iterator(*this, 0);
		auto end_ = end();
		while (!object_policy::is_object_iterable(*it) && it != end_){
			++it;
		}
		return it;	
	}

	const_iterator end() const { return const_iterator(*this, size()); }

	const_iterator cbegin() const { return begin(); }

	const_iterator cend() const { return end(); }
	
	// Debugging
	void check_internal_consistency() const {
		// trace freelist
		int ni = freelist_deque_;
		if (freelist_deque_ == capacity_) {
			if (freelist_deque_ != freelist_enque_){
				error("object_pool: freelist_deque_ != freelist_enque_");
			}
		}
		else {
			int count = 1;
			while (true) {
				if (ni == freelist_enque_)
					break;
				ni = indices_[ni].next;
				count++;
			}
			if (count != capacity_ - num_objects_){
				error("object_pool: count != capacity_ - num_objects_");
			}
		}
	}

protected:
	static const size_type max_size_ = 0xffff;
	size_type initial_capacity_ = 0;
	size_type capacity_ = 0;
	size_type num_objects_ = 0;
	uint16_t freelist_enque_ = 0;
	uint16_t freelist_deque_ = 0;

	struct index_type {
		id_type id = static_cast<id_type>(0);
		uint16_t index = 0;
		uint16_t next  = 0;
	};

	std::array<index_type, max_size_> indices_;
	storage_pool<T> objects_;

protected:

	inline uint32_t mask_index(id_type id){
		const uint32_t index_mask = 0xffff;
		return static_cast<uint32_t>(id) & index_mask;
	}

	inline index_type& index(id_type id) {
		return indices_[mask_index(id)];
	}

	inline const index_type& index(id_type id) const {
		return indices_[mask_index(id)];
	}

	index_type& new_index() {
		if (num_objects_ >= max_size()) {
			throw std::length_error("object_pool: maximum capacity exceeded");
		}

		if (num_objects_ >= capacity_) {
			size_type new_size = std::min(capacity_ + initial_capacity_, max_size());
			if (new_size <= max_size()) {
				int num_new_objects = new_size - capacity_;
				bool resize_succeeded = false;
				const int resize_attempts = 8;
				for (int i = 0; i < resize_attempts && !resize_succeeded; i++) {
					resize_succeeded = objects_.allocate(num_new_objects);
					if (!resize_succeeded) num_new_objects /= 2;
				}
				if (!resize_succeeded){
					throw std::length_error("object_pool: cannot append more storage");
				}
				capacity_ = objects_.size();
				indices_[freelist_enque_].next = static_cast<uint16_t>(num_objects_ + 1);
				freelist_enque_ = static_cast<uint16_t>(capacity_ - 1);
			}
			else {
				throw std::length_error("object_pool: maximum capacity exceeded");
			}
		}

		index_type& in = indices_[freelist_deque_];
		freelist_deque_ = in.next;
		in.index = static_cast<uint16_t>(num_objects_);
		num_objects_++;
		return in;
	}

	void destroy(T& object){
		object.~T();
	}

	void move_back_into(T& target, index_type& index_){
		new (&target) T(std::move(objects_[num_objects_ - 1]));
		// TODO: double check we need to destroy here (valgrind?)
		destroy(objects_[num_objects_ - 1]);

		// Adjust index
		if (object_policy::store_id_in_object){
			index(object_policy::get_object_id(target)).index = index_.index;
		}
		else {
			for (size_type i = 0; i < max_size(); ++i){
				if (indices_[i].index == num_objects_ - 1){
					indices_[i].index = index_.index;
					break;
				}
			}
		}
	}

	void error(const char* message) const {
#ifdef BSP_OBJECT_POOL_LOG_ERROR
		BSP_OBJECT_POOL_LOG_ERROR(message);
#endif
	}

	template<typename OP> friend class detail::object_pool_iterator;
	template<typename OP> friend class detail::object_pool_const_iterator;

	template<typename T_, typename ID_, typename Policy_>
	friend std::ostream& operator<<(std::ostream&, const object_pool<T_, ID_, Policy_>&);
};
  
template <typename T, typename ID, typename Policy> const typename object_pool<T, ID, Policy>::size_type object_pool<T, ID, Policy>::max_size_;

template<typename T, typename ID, typename Policy>
std::ostream& operator<<(std::ostream& out, const object_pool<T, ID, Policy>& pool){
	out << "object_pool [";
	auto it = pool.begin();
	auto end = pool.end();
	if (it == end){
		out << "]";
	}
	else {
		for (; it != end; ++it) {
			if (std::next(it) != end)
				out << *it << ", ";
			else
				out << *it << "]";
		}
	}
	return out;
}

}

// Iterator implementations

namespace bsp {
namespace detail {

template<class object_pool>
object_pool_iterator<object_pool>::object_pool_iterator(object_pool& array, typename object_pool::size_type ri) : object_pool_(array), storage_pool_(array.objects_), i_(0), di_(0) {
	for (; di_ < storage_pool_.storage_count(); di_++) {
		auto& dbz = storage_pool_.storage(di_);
		if (ri >= dbz.offset && ri < (dbz.offset + dbz.count)) {
			i_ = ri - dbz.offset;
			db_ = &dbz;
			break;
		}
	}
}

template<class object_pool>
object_pool_iterator<object_pool>& object_pool_iterator<object_pool>::operator++() {
	while (true) {
		++i_;
		if (i_ == db_->count) {
			// Go to next datablock
			i_ = 0; 
			++di_;
			if (di_ >= storage_pool_.storage_count()) return *this;
			else db_ = &storage_pool_.storage(di_);
		}
		const auto& value = db_->data[i_];
		if (object_pool::object_policy::is_object_iterable(value)) break;
	}
	return *this;
}

template<class object_pool>
bool object_pool_iterator<object_pool>::operator==(const object_pool_iterator<object_pool>& rhs) const {
	return (i_ == rhs.i_ && di_ == rhs.di_) || (di_ == rhs.di_ && i_ >= rhs.i_) || (di_ > rhs.di_);
}

template<class object_pool>
bool object_pool_iterator<object_pool>::operator!=(const object_pool_iterator<object_pool>& rhs) const {
	return !(*this == rhs);
}

template<class object_pool> typename object_pool_iterator<object_pool>::reference object_pool_iterator<object_pool>::operator*() { return db_->data[i_]; }
template<class object_pool> typename object_pool_iterator<object_pool>::const_reference object_pool_iterator<object_pool>::operator*() const { return db_->data[i_]; }

template<class object_pool>
object_pool_const_iterator<object_pool>::object_pool_const_iterator(const object_pool& array, typename object_pool::size_type ri) : object_pool_(array), storage_pool_(array.objects_), i_(0), di_(0) {
	for (; di_ < storage_pool_.storage_count(); di_++) {
		auto& dbz = storage_pool_.storage(di_);
		if (ri >= dbz.offset && ri < (dbz.offset + dbz.count)) {
			i_ = ri - dbz.offset;
			db_ = &dbz;
			break;
		}
	}
}

template<class object_pool>
object_pool_const_iterator<object_pool>& object_pool_const_iterator<object_pool>::operator++() {
	while (true) {
		++i_;
		if (i_ == db_->count) {
			// Go to next datablock
			i_ = 0; 
			++di_;
			if (di_ >= storage_pool_.storage_count()) return *this;
			else db_ = &storage_pool_.storage(di_);
		}
		const auto& value = db_->data[i_];
		if (object_pool::object_policy::is_object_iterable(value)) break;
	}
	return *this;
}

template<class object_pool>
bool object_pool_const_iterator<object_pool>::operator==(const object_pool_const_iterator<object_pool>& rhs) const {
	return (i_ == rhs.i_ && di_ == rhs.di_) || (di_ == rhs.di_ && i_ >= rhs.i_) || (di_ > rhs.di_);
}

template<class object_pool>
bool object_pool_const_iterator<object_pool>::operator!=(const object_pool_const_iterator<object_pool>& rhs) const {
	return !(*this == rhs);
}

template<class object_pool> typename object_pool_const_iterator<object_pool>::reference object_pool_const_iterator<object_pool>::operator*() const { return db_->data[i_]; }

}
} // namespace bsp

#endif
