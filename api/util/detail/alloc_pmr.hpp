// This file is a part of the IncludeOS unikernel - www.includeos.org
//
// Copyright 2018 IncludeOS AS, Oslo, Norway
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef UTIL_DETAIL_ALLOC_PMR
#define UTIL_DETAIL_ALLOC_PMR

#include <common>
#include <deque>
#include <delegate>
#include <util/units.hpp>

namespace os::mem::detail {

  class Pmr_pool : public std::enable_shared_from_this<Pmr_pool>, public std::pmr::memory_resource {
  public:
    using Resource     = os::mem::Pmr_pool::Resource;
    using Resource_ptr = os::mem::Pmr_pool::Resource_ptr;
    Pmr_pool(std::size_t total_max, std::size_t suballoc_max, std::size_t max_allocs)
      : cap_total_{total_max}, cap_suballoc_{suballoc_max}, max_resources_{max_allocs} {}

    void* do_allocate(size_t size, size_t align) override {
      if (UNLIKELY(size + allocated_ > cap_total_)) {
        throw std::bad_alloc();
      }

      // Adapt to memalign's minimum size- and alignemnt requiremnets
      if (align < sizeof(void*))
        align = sizeof(void*);

      if (size < sizeof(void*))
        size = sizeof(void*);

      void* buf = memalign(align, size);

      if (buf == nullptr) {
        throw std::bad_alloc();
      }

      allocated_ += size;
      return buf;
    }

    void do_deallocate (void* ptr, size_t size, size_t) override {

      // Adapt to memalign
      if (size < sizeof(void*))
        size = sizeof(void*);

      free(ptr);
      allocated_ -= size;
    }

    bool do_is_equal(const std::pmr::memory_resource&) const noexcept override {
      return true;
    }

    Resource_ptr resource_from_raw(Resource* raw) {
      Resource_ptr res_ptr(raw, [this](auto* res) {
          Expects(res->pool().get() == this);
          this->return_resource(res);
        });

      return res_ptr;
    }

    Resource_ptr new_resource() {
      Expects(used_resources_ < max_resources_);

      auto res = resource_from_raw(new Pmr_resource(shared_ptr()));
      used_resources_++;

      Ensures(res != nullptr);
      Ensures(used_resources_ <= max_resources_);
      return res;
    }

    Resource_ptr get_resource() {

      if (! free_resources_.empty()) {
        auto res = std::move(free_resources_.front());
        free_resources_.pop_front();

        if (UNLIKELY(! res->empty() and used_resources_ < max_resources_)) {
          free_resources_.emplace_back(std::move(res));
          return new_resource();
        }

        return res;
      }

      if (used_resources_ >= max_resources_)
        return nullptr;

      return new_resource();
    }

    void return_resource(Resource* raw) {
      Expects(used_resources_ > 0);
      auto res_ptr = resource_from_raw(raw);
      used_resources_--;
      free_resources_.emplace_back(std::move(res_ptr));
    }

    std::shared_ptr<Pmr_pool> shared_ptr() {
      return shared_from_this();
    }

    std::size_t resource_count() {
      return used_resources_ + free_resources_.size();
    }

    std::size_t free_resources() {
      return free_resources_.size();
    }

    std::size_t used_resources() {
      return used_resources_;
    }

    std::size_t total_capacity() {
      return cap_total_;
    }

    void set_resource_capacity(std::size_t sz) {
      cap_suballoc_ = sz;
    }

    void set_total_capacity(std::size_t sz) {
      cap_total_ = sz;
    }

    std::size_t resource_capacity() {
      if (cap_suballoc_ == 0) {
        if (used_resources_ == 0)
          return cap_total_;
        return cap_total_ / used_resources_;
      }
      return cap_suballoc_;
    }

    std::size_t bytes_booked() {
      return cap_suballoc_ * used_resources_;
    }

    std::size_t bytes_bookable() {
      auto booked = bytes_booked();
      if (booked > cap_total_)
        return 0;

      return cap_total_ - booked;
    }


    std::size_t allocated() {
      return allocated_;
    }

    bool full() {
      return allocated_ >= cap_total_;
    }

    bool empty() {
      return allocated_ == 0;
    }

    std::size_t allocatable() {
      auto allocd = allocated();
      if (allocd > cap_total_)
        return 0;
      return cap_total_ - allocd;
    }

    // NOTE: This can cause leaks or other chaos if you're not sure what you're doing
    void clear_free_resources() {
      for (auto& res : free_resources_) {
        *res = Resource(shared_ptr());
      }
    }

  private:
    std::size_t allocated_      = 0;
    std::size_t cap_total_      = 0;
    std::size_t cap_suballoc_   = 0;
    std::size_t max_resources_  = 0;
    std::size_t used_resources_ = 0;
    std::deque<Resource_ptr> free_resources_{};
  };

} // os::mem::detail


namespace os::mem {

  //
  // Pmr_pool implementatino (PIMPL wrapper)
  //
  std::size_t Pmr_pool::total_capacity() { return impl->total_capacity(); }
  std::size_t Pmr_pool::resource_capacity() { return impl->resource_capacity(); }
  std::size_t Pmr_pool::allocatable() { return impl->allocatable(); }
  std::size_t Pmr_pool::allocated() { return impl->allocated(); }

  void Pmr_pool::set_resource_capacity(std::size_t s) { impl->set_resource_capacity(s); }
  void Pmr_pool::set_total_capacity(std::size_t s) { impl->set_total_capacity(s); };

  Pmr_pool::Pmr_pool(size_t sz, size_t sz_sub, size_t max_allocs)
    : impl{std::make_shared<detail::Pmr_pool>(sz, sz_sub, max_allocs)}{}
  Pmr_pool::Resource_ptr Pmr_pool::get_resource() { return impl->get_resource(); }
  std::size_t Pmr_pool::resource_count() { return impl->resource_count(); }
  void Pmr_pool::return_resource(Resource* res) { impl->return_resource(res); }
  bool Pmr_pool::full() { return impl->full(); }
  bool Pmr_pool::empty() { return impl->empty(); }

  //
  // Pmr_resource implementation
  //
  Pmr_resource::Pmr_resource(Pool_ptr p) : pool_{p} {}
  std::size_t Pmr_resource::capacity() { return pool_->resource_capacity(); }
  std::size_t Pmr_resource::allocatable() {
    auto cap = capacity();
    if (used > cap)
      return 0;
    return cap - used;
  }

  std::size_t Pmr_resource::allocated() {
    return used;
  }

  Pmr_resource::Pool_ptr Pmr_resource::pool() {
    return pool_;
  }

  void* Pmr_resource::do_allocate(std::size_t size, std::size_t align) {
    auto cap = capacity();
    if (UNLIKELY(size + used > cap)) {
      throw std::bad_alloc();
    }

    void* buf = pool_->allocate(size, align);

    used += size;
    allocs++;

    return buf;
  }

  void Pmr_resource::do_deallocate(void* ptr, std::size_t s, std::size_t a) {
    Expects(s != 0); // POSIX malloc will allow size 0, but return nullptr.
    pool_->deallocate(ptr,s,a);
    deallocs++;
    used -= s;
  }

  bool Pmr_resource::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    if (const auto* other_ptr = dynamic_cast<const Pmr_resource*>(&other)) {
      return pool_ == other_ptr->pool_;
    }
    return false;
  }

  bool Pmr_resource::full() {
    return used >= capacity();
  }

  bool Pmr_resource::empty() {
    return used == 0;
  }
}

#endif
