#include "Pool.hh"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <phosg/Strings.hh>

// this mmap flag is required on OSX but doesn't exist on Linux
#ifndef MACOSX
#define MAP_HASSEMAPHORE 0
#endif

// TODO: this assumption might be wrong on some less-common architectures
#define PAGE_SIZE 4096

using namespace std;

namespace sharedstructures {


static int open_segment(const char* name, int type, mode_t mode, bool file) {
  if (file) {
    return open(name, type, mode);
  } else {
    return shm_open(name, type, mode);
  }
}

static int unlink_segment(const char* name, bool file) {
  if (file) {
    return unlink(name);
  } else {
    return shm_unlink(name);
  }
}


Pool::Pool(const string& name, size_t max_size, bool file) : name(name),
    max_size(max_size) {

  // on Linux, shared memory objects can be resized at any time just by calling
  // ftruncate again. but on OSX, ftruncate can be called only once for each
  // shared memory object, so we instead have to memory-map a file on disk.
  if (MAP_HASSEMAPHORE) {
    file = true;
  }

  this->fd = open_segment(this->name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666,
      file);
  if (this->fd == -1 && errno == EEXIST) {
    this->fd = open_segment(this->name.c_str(), O_RDWR, 0666, file);
    if (this->fd == -1) {
      throw cannot_open_file(this->name);
    }

    // we did not create the shared memory object; map it all into memory
    this->pool_size = fstat(this->fd).st_size;
    this->data = (Data*)mmap(NULL, this->pool_size, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_HASSEMAPHORE, this->fd, 0);
    if (!this->data) {
      throw runtime_error("mmap failed: " + string_for_error(errno));
    }

  } else {

    // we created the shared memory object, so its size is zero. resize it to
    // the minimum size and initialize the basic data structures. note that this
    // procedure is safe from a concurrency perspective because we use 0 as the
    // locked state for our mutexes.
    this->pool_size = PAGE_SIZE;
    if (ftruncate(this->fd, this->pool_size)) {
      unlink_segment(this->name.c_str(), file);
      throw runtime_error("can\'t resize memory map: " +
          string_for_error(errno));
    }

    this->data = (Data*)mmap(NULL, this->pool_size, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_HASSEMAPHORE, this->fd, 0);
    if (!this->data) {
      unlink_segment(this->name.c_str(), file);
      throw runtime_error("mmap failed: " + string_for_error(errno));
    }

    this->data->size = this->pool_size;
  }
}

Pool::~Pool() {
  if (this->data) {
    munmap(this->data, this->pool_size);
  }
  // this->fd is closed automatically because it's a scoped_fd
}


const string& Pool::get_name() const {
  return this->name;
}


void Pool::expand(size_t new_size) {
  if (new_size < this->pool_size) {
    return;
  }

  // the new size must be a multiple of the page size, so round it up.
  new_size = (new_size + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1));
  if (this->max_size && (new_size > this->max_size)) {
    throw runtime_error("can\'t expand pool beyond maximum size");
  }

  if (ftruncate(this->fd, new_size)) {
    throw runtime_error("can\'t resize memory map: " + string_for_error(errno));
  }
  this->data->size = new_size;

  // now the underlying shared memory object is larger; we need to recreate our
  // view of it
  this->check_size_and_remap(); // sets this->pool_size
}

void Pool::check_size_and_remap() const {
  uint64_t new_pool_size = this->pool_size ? this->data->size.load() :
      fstat(this->fd).st_size;
  if (new_pool_size != this->pool_size) {
    munmap(this->data, this->pool_size);

    // remap the pool with the new size
    this->pool_size = new_pool_size;
    this->data = (Data*)mmap(NULL, this->pool_size, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_HASSEMAPHORE, this->fd, 0);
    if (!this->data) {
      throw runtime_error("mmap failed: " + string_for_error(errno));
    }
  }
}

size_t Pool::size() const {
  return this->data->size;
}


bool Pool::delete_pool(const std::string& name, bool file) {
  int ret = unlink_segment(name.c_str(), file || MAP_HASSEMAPHORE);
  if (ret == 0) {
    return true;
  }
  if (errno == ENOENT) {
    return false;
  }
  throw runtime_error("can\'t delete pool: " + string_for_error(errno));
}

} // namespace sharedstructures
