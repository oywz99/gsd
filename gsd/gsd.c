// Copyright (c) 2016-2019 The Regents of the University of Michigan
// This file is part of the General Simulation Data (GSD) project, released under the BSD 2-Clause License.

#include <sys/stat.h>
#ifdef _WIN32

#define GSD_USE_MMAP 0
#include <io.h>

#else // linux / mac

#define _XOPEN_SOURCE 500
#include <unistd.h>
#include <sys/mman.h>
#define GSD_USE_MMAP 1

#endif

#ifdef __APPLE__
#include <limits.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <stddef.h>

#include "gsd.h"

/** @file gsd.c
    @brief Implements the GSD C API
*/

/// Magic value identifying a GSD file
const uint64_t GSD_MAGIC_ID = 0x65DF65DF65DF65DF;

/// Initial index size
const size_t GSD_INITIAL_INDEX_SIZE = 128;

/// Initial namelist size
const size_t GSD_INITIAL_NAMELIST_SIZE = 128;

/// Size of the temporary copy buffer
enum { GSD_COPY_BUFFER_SIZE = 1024*16 };

// define windows wrapper functions
#ifdef _WIN32
#define lseek _lseeki64
#define open _open
#define ftruncate _chsize
#define fsync _commit
typedef int64_t ssize_t;

int S_IRUSR = _S_IREAD;
int S_IWUSR = _S_IWRITE;
int S_IRGRP = _S_IREAD;
int S_IWGRP = _S_IWRITE;

ssize_t pread(int fd, void *buf, size_t count, int64_t offset)
    {
    // Note: _read only accepts unsigned int values
    if (count > UINT_MAX)
        return -1;

    int64_t oldpos = _telli64(fd);
    _lseeki64(fd, offset, SEEK_SET);
    ssize_t result = _read(fd, buf, (unsigned int)count);
    _lseeki64(fd, oldpos, SEEK_SET);
    return result;
    }

ssize_t pwrite(int fd, const void *buf, size_t count, int64_t offset)
    {
    // Note: _write only accepts unsigned int values
    if (count > UINT_MAX)
        return -1;

    int64_t oldpos = _telli64(fd);
    _lseeki64(fd, offset, SEEK_SET);
    ssize_t result = _write(fd, buf, (unsigned int)count);
    _lseeki64(fd, oldpos, SEEK_SET);
    return result;
    }

#endif

/** Zero memory

    @param d pointer to memory region
    @param size_data size of the memory region in bytes
    @param size_to_zero size of the area to zero in bytes
*/
void gsd_zero_memory(void *d, size_t size_data, size_t size_to_zero)
    {
    if(size_to_zero > size_data)
        {
        size_to_zero = size_data;
        }
    volatile unsigned char *p = d;
    while (size_to_zero--)
        {
        *p++ = 0;
        }
    }

static ssize_t __pwrite_retry(int fd, const void *buf, size_t count, int64_t offset)
    {
    size_t total_bytes_written = 0;
    const char *ptr = (char *)buf;

    // perform multiple pwrite calls to complete a large write successfully
    while (total_bytes_written < count)
        {
        size_t to_write = count - total_bytes_written;
        #if defined(_WIN32) || defined(__APPLE__)
        // win32 and apple raise an error for writes greater than INT_MAX
        if (to_write > INT_MAX/2) to_write = INT_MAX/2;
        #endif

        errno = 0;
        ssize_t bytes_written = pwrite(fd, ptr + total_bytes_written, to_write, offset + total_bytes_written);
        if (bytes_written == -1 || (bytes_written == 0 && errno != 0))
            {
            return -1;
            }

        total_bytes_written += bytes_written;
        }

    return total_bytes_written;
    }

static ssize_t __pread_retry(int fd, void *buf, size_t count, int64_t offset)
    {
    size_t total_bytes_read = 0;
    char *ptr = (char *)buf;

    // perform multiple pread calls to complete a large write successfully
    while (total_bytes_read < count)
        {
        size_t to_read = count - total_bytes_read;
        #if defined(_WIN32) || defined(__APPLE__)
        // win32 and apple raise errors for reads greater than INT_MAX
        if (to_read > INT_MAX/2) to_read = INT_MAX/2;
        #endif

        errno = 0;
        ssize_t bytes_read = pread(fd, ptr + total_bytes_read, to_read, offset + total_bytes_read);
        if (bytes_read == -1 || (bytes_read == 0 && errno != 0))
            {
            return -1;
            }

        total_bytes_read += bytes_read;

        // handle end of file
        if (bytes_read == 0)
            {
            return total_bytes_read;
            }
        }

    return total_bytes_read;
    }

/*! \internal
    \brief Utility function to expand the memory space for the index block
    \param handle handle to the open gsd file
*/
static int __gsd_expand_index(struct gsd_handle *handle)
    {
    // multiply the index size each time it grows
    // this allows the index to grow rapidly to accommodate new frames
    const int multiplication_factor = 2;

    // save the old size and update the new size
    size_t old_size = handle->header.index_allocated_entries;
    size_t new_size = old_size * multiplication_factor;
    handle->header.index_allocated_entries = new_size;

    if (handle->open_flags == GSD_OPEN_READWRITE)
        {
        // allocate the new larger index block
        handle->index = (struct gsd_index_entry *)
                        realloc(handle->index, sizeof(struct gsd_index_entry) * new_size);
        if (handle->index == NULL)
            {
            return -1;
            }

        // zero the new memory
        gsd_zero_memory(handle->index + old_size,
                        sizeof(struct gsd_index_entry) * (new_size - old_size),
                        sizeof(struct gsd_index_entry) * (new_size - old_size));

        // now, put the new larger index at the end of the file
        handle->header.index_location = lseek(handle->fd, 0, SEEK_END);
        ssize_t bytes_written = __pwrite_retry(handle->fd,
                                               handle->index,
                                               sizeof(struct gsd_index_entry) * new_size,
                                               handle->header.index_location);

        if (bytes_written == -1 || bytes_written != sizeof(struct gsd_index_entry) * new_size)
            {
            return -1;
            }

        // set the new file size
        handle->file_size = handle->header.index_location + bytes_written;
        }
    else if (handle->open_flags == GSD_OPEN_APPEND)
        {
        // in append mode, we don't have the whole index stored in memory. Instead, we need to copy it in chunks
        // from the file's old position to the new position
        char buf[GSD_COPY_BUFFER_SIZE];

        int64_t new_index_location = lseek(handle->fd, 0, SEEK_END);
        int64_t old_index_location = handle->header.index_location;
        size_t total_bytes_written = 0;
        size_t old_index_bytes = old_size * sizeof(struct gsd_index_entry);
        while (total_bytes_written < old_index_bytes)
            {
            size_t bytes_to_copy = GSD_COPY_BUFFER_SIZE;
            if (old_index_bytes - total_bytes_written < GSD_COPY_BUFFER_SIZE)
                {
                bytes_to_copy = old_index_bytes - total_bytes_written;
                }

            ssize_t bytes_read = __pread_retry(handle->fd,
                                               buf,
                                               bytes_to_copy,
                                               old_index_location + total_bytes_written);

            if (bytes_read == -1 || bytes_read != bytes_to_copy)
                {
                return -1;
                }

            ssize_t bytes_written = __pwrite_retry(handle->fd,
                                                   buf,
                                                   bytes_to_copy,
                                                   new_index_location + total_bytes_written);

            if (bytes_written == -1 || bytes_written != bytes_to_copy)
                {
                return -1;
                }

            total_bytes_written += bytes_written;
            }

        // fill the new index space with 0s
        gsd_zero_memory(buf, GSD_COPY_BUFFER_SIZE, GSD_COPY_BUFFER_SIZE);

        size_t new_index_bytes = new_size * sizeof(struct gsd_index_entry);
        while (total_bytes_written < new_index_bytes)
            {
            size_t bytes_to_copy = GSD_COPY_BUFFER_SIZE;
            if (new_index_bytes - total_bytes_written < GSD_COPY_BUFFER_SIZE)
                {
                bytes_to_copy = new_index_bytes - total_bytes_written;
                }

            ssize_t bytes_written = __pwrite_retry(handle->fd,
                                                   buf,
                                                   bytes_to_copy,
                                                   new_index_location + total_bytes_written);

            if (bytes_written == -1 || bytes_written != bytes_to_copy)
                {
                return -1;
                }

            total_bytes_written += bytes_written;
            }

        // update to the new index location in the header
        handle->header.index_location = new_index_location;
        handle->file_size = handle->header.index_location + total_bytes_written;
        }

    // sync the expanded index
    int retval = fsync(handle->fd);
    if (retval != 0)
        {
        return -1;
        }

    // write the new header out
    ssize_t bytes_written = __pwrite_retry(handle->fd, &(handle->header), sizeof(struct gsd_header), 0);
    if (bytes_written != sizeof(struct gsd_header))
        {
        return -1;
        }

    // sync the updated header
    retval = fsync(handle->fd);
    if (retval != 0)
        {
        return -1;
        }

    return 0;
    }

/*! \internal
    \brief utility function to search the namelist and return the id assigned to the name
    \param handle handle to the open gsd file
    \param name string name
    \param append Set to true to allow appending new names into the index, false to disallow

    \return the id assigned to the name, or UINT16_MAX if not found and append is false
*/
uint16_t __gsd_get_id(struct gsd_handle *handle, const char *name, uint8_t append)
    {
    // search for the name in the namelist
    size_t i;
    for (i = 0; i < handle->namelist_num_entries; i++)
        {
        if (0 == strncmp(name, handle->namelist[i].name, sizeof(handle->namelist[i].name)))
            {
            return i;
            }
        }

    // append the name if allowed
    if (append &&
        (handle->open_flags == GSD_OPEN_READWRITE || handle->open_flags == GSD_OPEN_APPEND) &&
        handle->namelist_num_entries < handle->header.namelist_allocated_entries)
        {
        strncpy(handle->namelist[handle->namelist_num_entries].name, name, sizeof(struct gsd_namelist_entry)-1);
        handle->namelist[handle->namelist_num_entries].name[sizeof(struct gsd_namelist_entry)-1] = 0;

        // update the namelist on disk
        ssize_t bytes_written = __pwrite_retry(handle->fd,
                                               &(handle->namelist[handle->namelist_num_entries]),
                                               sizeof(struct gsd_namelist_entry),
                                               handle->header.namelist_location
                                                   + sizeof(struct gsd_namelist_entry)*handle->namelist_num_entries);

        if (bytes_written != sizeof(struct gsd_namelist_entry))
            {
            return UINT16_MAX;
            }

        handle->namelist_num_entries++;

        // mark that synchronization is needed
        handle->needs_sync = true;

        return handle->namelist_num_entries-1;
        }

    // otherwise, return not found
    return UINT16_MAX;
    }

/** @internal
    @brief Truncate the file and write a new gsd header.

    @param fd file descriptor to initialize
    @param application Generating application name (truncated to 63 chars)
    @param schema Schema name for data to be written in this GSD file (truncated to 63 chars)
    @param schema_version Version of the scheme data to be written (make with gsd_make_version())
*/
int __gsd_initialize_file(int fd, const char *application, const char *schema, uint32_t schema_version)
    {
    // check if the file was created
    if (fd == -1)
        {
        return -1;
        }

    int retval = ftruncate(fd, 0);
    if (retval != 0)
        {
        return retval;
        }

    // populate header fields
    struct gsd_header header;
    gsd_zero_memory(&header, sizeof(header), sizeof(header));

    header.magic = GSD_MAGIC_ID;
    header.gsd_version = gsd_make_version(1,0);
    strncpy(header.application, application, sizeof(header.application)-1);
    header.application[sizeof(header.application)-1] = 0;
    strncpy(header.schema, schema, sizeof(header.schema)-1);
    header.schema[sizeof(header.schema)-1] = 0;
    header.schema_version = schema_version;
    header.index_location = sizeof(header);
    header.index_allocated_entries = GSD_INITIAL_INDEX_SIZE;
    header.namelist_location = header.index_location + sizeof(struct gsd_index_entry)*header.index_allocated_entries;
    header.namelist_allocated_entries = GSD_INITIAL_NAMELIST_SIZE;
    gsd_zero_memory(header.reserved, sizeof(header.reserved), sizeof(header.reserved));

    // write the header out
    ssize_t bytes_written = __pwrite_retry(fd, &header, sizeof(header), 0);
    if (bytes_written != sizeof(header))
        {
        return -1;
        }

    // allocate and zero default index memory
    struct gsd_index_entry index[GSD_INITIAL_INDEX_SIZE];
    gsd_zero_memory(index, sizeof(index), sizeof(index));

    // write the empty index out
    bytes_written = __pwrite_retry(fd, index, sizeof(index), sizeof(header));
    if (bytes_written != sizeof(index))
        {
        return -1;
        }

    // allocate and zero the namelist memory
    struct gsd_namelist_entry namelist[GSD_INITIAL_NAMELIST_SIZE];
    gsd_zero_memory(namelist, sizeof(namelist), sizeof(namelist));

    // write the namelist out
    bytes_written = __pwrite_retry(fd, namelist, sizeof(namelist), sizeof(header)+sizeof(index));
    if (bytes_written != sizeof(namelist))
        {
        return -1;
        }

    // sync file
    retval = fsync(fd);
    if (retval != 0)
        {
        return -1;
        }

    return 0;
    }

/*! \internal
    \brief Utility function to validate index entry
    \param handle handle to the open gsd file
    \param idx index of entry to validate

    \returns 1 if the entry is valid, 0 if it is not
*/
static int __is_entry_valid(struct gsd_handle *handle, size_t idx)
    {
    const struct gsd_index_entry entry = handle->index[idx];

    // check for valid type
    if (gsd_sizeof_type((enum gsd_type)entry.type) == 0)
        {
        return 0;
        }

    // validate that we don't read past the end of the file
    size_t size = entry.N * entry.M * gsd_sizeof_type((enum gsd_type)entry.type);
    if ((entry.location + size) > handle->file_size)
        {
        return 0;
        }

    // check for valid frame (frame cannot be more than the number of index entries)
    if (entry.frame >= handle->header.index_allocated_entries)
        {
        return 0;
        }

    // check for valid id
    if (entry.id >= handle->namelist_num_entries)
        {
        return 0;
        }

    // check for valid flags
    if (entry.flags != 0)
        {
        return 0;
        }

    return 1;
    }

/*! \param handle Handle to read the header

    \pre handle->fd is an open file.
    \pre handle->open_flags is set.

    Read in the file index.
*/
int __gsd_read_header(struct gsd_handle* handle)
    {
    // check if the file was created
    if (handle->fd == -1)
        {
        return -1;
        }

    // read the header
    ssize_t bytes_read = __pread_retry(handle->fd, &handle->header, sizeof(struct gsd_header), 0);
    if (bytes_read == -1)
        {
        return -1;
        }
    if (bytes_read != sizeof(struct gsd_header))
        {
        return -2;
        }

    // validate the header
    if (handle->header.magic != GSD_MAGIC_ID)
        {
        return -2;
        }

    if (handle->header.gsd_version < gsd_make_version(1,0) && handle->header.gsd_version != gsd_make_version(0,3))
        {
        return -3;
        }

    if (handle->header.gsd_version >= gsd_make_version(2,0))
        {
        return -3;
        }

    // determine the file size
    handle->file_size = lseek(handle->fd, 0, SEEK_END);

    // map the file in read only mode
    #if GSD_USE_MMAP
    if (handle->open_flags == GSD_OPEN_READONLY)
        {
        size_t page_size = getpagesize();
        size_t index_size = sizeof(struct gsd_index_entry) * handle->header.index_allocated_entries;
        size_t offset = (handle->header.index_location / page_size) * page_size;
        handle->mapped_data = mmap(NULL, index_size+(handle->header.index_location - offset), PROT_READ, MAP_SHARED, handle->fd, offset);
        handle->index = (struct gsd_index_entry *) (((char *)handle->mapped_data) + (handle->header.index_location - offset));

        if (handle->mapped_data == MAP_FAILED)
            {
            return -1;
            }
        }
    else if (handle->open_flags == GSD_OPEN_READWRITE)
    #endif
        {
        // read the indices into our own memory
        handle->mapped_data = NULL;

        // validate that the index block exists inside the file
        if (handle->header.index_location + sizeof(struct gsd_index_entry) * handle->header.index_allocated_entries > handle->file_size)
            {
            return -4;
            }

        // read the index block
        handle->index = (struct gsd_index_entry *)malloc(sizeof(struct gsd_index_entry) * handle->header.index_allocated_entries);
        if (handle->index == NULL)
            {
            return -5;
            }

        bytes_read = __pread_retry(handle->fd,
                                   handle->index,
                                   sizeof(struct gsd_index_entry) * handle->header.index_allocated_entries,
                                   handle->header.index_location);

        if (bytes_read == -1 || bytes_read != sizeof(struct gsd_index_entry) * handle->header.index_allocated_entries)
            {
            return -1;
            }
        }
    #if GSD_USE_MMAP
    else if (handle->open_flags == GSD_OPEN_APPEND)
        {
        // in append mode, we want to avoid reading the entire index in memory, but we also don't want to bother
        // keeping the mapping up to date. Map the index for now to determine index_num_entries, but then
        // unmap it and use different logic to manage a cache of only unwritten index entries

        // mmap may fail if offset is not a multiple of the page size, so we
        // always memory map from the beginning of the file and then access the
        // index pointer by its offset.
        size_t page_size = getpagesize();
        size_t index_size = sizeof(struct gsd_index_entry) * handle->header.index_allocated_entries;
        size_t offset = (handle->header.index_location / page_size) * page_size;
        handle->mapped_data = mmap(NULL, index_size+(handle->header.index_location - offset), PROT_READ, MAP_SHARED, handle->fd, offset);
        handle->index = (struct gsd_index_entry *) (((char *)handle->mapped_data) + (handle->header.index_location - offset));

        if (handle->mapped_data == MAP_FAILED)
            {
            return -1;
            }
        }
    #endif

    // since the namelist is small, we always allocate memory for it rather than memory mapping
    // validate that the namelist block exists inside the file
    if (handle->header.namelist_location + sizeof(struct gsd_namelist_entry) * handle->header.namelist_allocated_entries > handle->file_size)
        {
        return -4;
        }

    // read the namelist block
    handle->namelist = (struct gsd_namelist_entry *)malloc(sizeof(struct gsd_namelist_entry) * handle->header.namelist_allocated_entries);
    if (handle->namelist == NULL)
        {
        return -5;
        }

    bytes_read = __pread_retry(handle->fd,
                               handle->namelist,
                               sizeof(struct gsd_namelist_entry) * handle->header.namelist_allocated_entries,
                               handle->header.namelist_location);

    if (bytes_read == -1 || bytes_read != sizeof(struct gsd_namelist_entry) * handle->header.namelist_allocated_entries)
        {
        return -1;
        }

    // determine the number of namelist entries (marked by an empty string)
    // base case: the namelist is full
    handle->namelist_num_entries = handle->header.namelist_allocated_entries;

    // general case, find the first namelist entry that is the empty string
    size_t i;
    for (i = 0; i < handle->header.namelist_allocated_entries; i++)
        {
        if (handle->namelist[i].name[0] == 0)
            {
            handle->namelist_num_entries = i;
            break;
            }
        }

    // file is corrupt if first index entry is invalid
    if (handle->index[0].location != 0 && !__is_entry_valid(handle, 0))
        {
        return -4;
        }

    if (handle->index[0].location == 0)
        {
        handle->index_num_entries = 0;
        }
    else
        {
        // determine the number of index entries (marked by location = 0)
        // binary search for the first index entry with location 0
        size_t L = 0;
        size_t R = handle->header.index_allocated_entries;

        // progressively narrow the search window by halves
        do
            {
            size_t m = (L+R)/2;

            // file is corrupt if any index entry is invalid or frame does not increase monotonically
            if (handle->index[m].location != 0 &&
                (!__is_entry_valid(handle, m) || handle->index[m].frame < handle->index[L].frame))
                {
                return -4;
                }

            if (handle->index[m].location != 0)
                {
                L = m;
                }
            else
                {
                R = m;
                }
            } while ((R-L) > 1);

        // this finds R = the first index entry with location = 0
        handle->index_num_entries = R;
        }

    // determine the current frame counter
    if (handle->index_num_entries == 0)
        {
        handle->cur_frame = 0;
        }
    else
        {
        handle->cur_frame = handle->index[handle->index_num_entries-1].frame + 1;
        }

    // at this point, all valid index entries have been written to disk
    handle->index_written_entries = handle->index_num_entries;

    if (handle->open_flags == GSD_OPEN_APPEND)
        {
        #if GSD_USE_MMAP
        // in append mode, we need to tear down the temporary mapping and allocate a temporary buffer
        // to hold indices for a single frame
        size_t page_size = getpagesize();
        size_t index_size = sizeof(struct gsd_index_entry) * handle->header.index_allocated_entries;
        size_t offset = (handle->header.index_location / page_size) * page_size;
        int retval = munmap(handle->mapped_data, index_size+(handle->header.index_location - offset));
        handle->index = NULL;

        if (retval != 0)
            {
            return -1;
            }
        #else
        free(handle->index);
        #endif

        handle->append_index_size = 1;
        handle->index = (struct gsd_index_entry *)malloc(sizeof(struct gsd_index_entry) * handle->append_index_size);
        if (handle->index == NULL)
            {
            return -5;
            }

        handle->mapped_data = NULL;
        }

    return 0;
    }

uint32_t gsd_make_version(unsigned int major, unsigned int minor)
    {
    return major << 16 | minor;
    }

int gsd_create(const char *fname, const char *application, const char *schema, uint32_t schema_version)
    {
    int extra_flags = 0;
    #ifdef _WIN32
    extra_flags = _O_BINARY;
    #endif

    // create the file
    int fd = open(fname, O_RDWR | O_CREAT | O_TRUNC | extra_flags,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    int retval = __gsd_initialize_file(fd, application, schema, schema_version);
    close(fd);
    return retval;
    }

int gsd_create_and_open(struct gsd_handle* handle,
                        const char *fname,
                        const char *application,
                        const char *schema,
                        uint32_t schema_version,
                        const enum gsd_open_flag flags,
                        int exclusive_create)
    {
    int extra_flags = 0;
    #ifdef _WIN32
    extra_flags = _O_BINARY;
    #endif

    // set the open flags in the handle
    if (flags == GSD_OPEN_READWRITE)
        {
        handle->open_flags = GSD_OPEN_READWRITE;
        }
    else if (flags == GSD_OPEN_READONLY)
        {
        return -6;
        }
    else if (flags == GSD_OPEN_APPEND)
        {
        handle->open_flags = GSD_OPEN_APPEND;
        }

    // set the exclusive create bit
    if (exclusive_create)
        {
        extra_flags |= O_EXCL;
        }

    // create the file
    handle->fd = open(fname, O_RDWR | O_CREAT | O_TRUNC | extra_flags,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    int retval = __gsd_initialize_file(handle->fd, application, schema, schema_version);
    if (retval != 0)
        {
        close(handle->fd);
        return retval;
        }

    retval = __gsd_read_header(handle);
    if (retval != 0)
        {
        close(handle->fd);
        }
    return retval;
    }

int gsd_open(struct gsd_handle* handle, const char *fname, const enum gsd_open_flag flags)
    {
    // allocate the handle
    gsd_zero_memory(handle, sizeof(struct gsd_handle), sizeof(struct gsd_handle));
    handle->index = NULL;
    handle->namelist = NULL;
    handle->cur_frame = 0;

    int extra_flags = 0;
    #ifdef _WIN32
    extra_flags = _O_BINARY;
    #endif

    // open the file
    if (flags == GSD_OPEN_READWRITE)
        {
        handle->fd = open(fname, O_RDWR | extra_flags);
        handle->open_flags = GSD_OPEN_READWRITE;
        }
    else if (flags == GSD_OPEN_READONLY)
        {
        handle->fd = open(fname, O_RDONLY | extra_flags);
        handle->open_flags = GSD_OPEN_READONLY;
        }
    else if (flags == GSD_OPEN_APPEND)
        {
        handle->fd = open(fname, O_RDWR | extra_flags);
        handle->open_flags = GSD_OPEN_APPEND;
        }

    int retval = __gsd_read_header(handle);
    if (retval != 0)
        {
        close(handle->fd);
        }
    return retval;
    }

int gsd_truncate(struct gsd_handle* handle)
    {
    if (handle == NULL)
        {
        return -2;
        }
    if (handle->open_flags == GSD_OPEN_READONLY)
        {
        return -2;
        }

    // deallocate indices
    if (handle->namelist != NULL)
        {
        free(handle->namelist);
        handle->namelist = NULL;
        }

    if (handle->index != NULL)
        {
        free(handle->index);
        handle->index = NULL;
        }

    // keep a copy of the old header
    struct gsd_header old_header = handle->header;
    int retval = __gsd_initialize_file(handle->fd,
                                       old_header.application,
                                       old_header.schema,
                                       old_header.schema_version);

    if (retval != 0)
        {
        return retval;
        }

    return __gsd_read_header(handle);
    }

int gsd_close(struct gsd_handle* handle)
    {
    if (handle == NULL)
        {
        return -2;
        }

    // save the fd so we can use it after freeing the handle
    int fd = handle->fd;

    // zero and free memory allocated in the handle
    #if GSD_USE_MMAP
    if (handle->mapped_data != NULL)
        {
        size_t page_size = getpagesize();
        size_t index_size = sizeof(struct gsd_index_entry) * handle->header.index_allocated_entries;
        size_t offset = (handle->header.index_location / page_size) * page_size;
        int retval = munmap(handle->mapped_data, index_size+(handle->header.index_location - offset));
        handle->mapped_data = NULL;
        handle->index = NULL;

        if (retval != 0)
            {
            return -1;
            }

        gsd_zero_memory(handle, sizeof(struct gsd_handle), sizeof(struct gsd_handle));
        }
    else
    #endif
        {
        if (handle->index != NULL)
            {
            free(handle->index);
            handle->index = NULL;

            gsd_zero_memory(handle, sizeof(struct gsd_handle), sizeof(struct gsd_handle));
            }
        }

    if (handle->namelist != NULL)
        {
        free(handle->namelist);
        handle->namelist = NULL;
        }

    // close the file
    int retval = close(fd);
    if (retval != 0)
        {
        return -1;
        }

    return 0;
    }

int gsd_end_frame(struct gsd_handle* handle)
    {
    if (handle == NULL)
        {
        return -2;
        }
    if (handle->open_flags == GSD_OPEN_READONLY)
        {
        return -2;
        }

    // all data chunks have already been written to the file and the index updated in memory. To end a frame, all we
    // need to do is increment the frame counter
    handle->cur_frame++;

    // and write unwritten index entries to the file (if there are any to write)
    uint64_t entries_to_write = handle->index_num_entries - handle->index_written_entries;
    if (entries_to_write > 0)
        {
        // write just those unwritten entries to the end of the index block
        int64_t write_pos = handle->header.index_location + sizeof(struct gsd_index_entry)*handle->index_written_entries;

        // in append mode, the start of the write is at the start of the index in memory
        // in readwrite mode, the entire index is in memory, so start at index_written_entries
        struct gsd_index_entry* data = handle->index;
        if (handle->open_flags != GSD_OPEN_APPEND)
            {
            data += handle->index_written_entries;
            }

        ssize_t bytes_written = __pwrite_retry(handle->fd,
                                              data,
                                              sizeof(struct gsd_index_entry)*entries_to_write,
                                              write_pos);

        if (bytes_written == -1 || bytes_written != sizeof(struct gsd_index_entry)*entries_to_write)
            {
            return -1;
            }

        handle->index_written_entries += entries_to_write;
        }

    // this sync is triggered by the namelist update
    if (handle->needs_sync)
        {
        int retval = fsync(handle->fd);
        if (retval != 0)
            {
            return -1;
            }
        handle->needs_sync = false;
        }

    return 0;
    }

int gsd_write_chunk(struct gsd_handle* handle,
                    const char *name,
                    enum gsd_type type,
                    uint64_t N,
                    uint32_t M,
                    uint8_t flags,
                    const void *data)
    {
    // validate input
    if (data == NULL)
        {
        return -2;
        }
    if (M == 0)
        {
        return -2;
        }
    if (handle->open_flags == GSD_OPEN_READONLY)
        {
        return -2;
        }

    // populate fields in the index_entry data
    struct gsd_index_entry index_entry;
    gsd_zero_memory(&index_entry, sizeof(index_entry), sizeof(index_entry));
    index_entry.frame = handle->cur_frame;
    index_entry.id = __gsd_get_id(handle, name, 1);
    if (index_entry.id == UINT16_MAX)
        {
        return -3;
        }
    index_entry.type = (uint8_t)type;
    index_entry.N = N;
    index_entry.M = M;
    size_t size = N * M * gsd_sizeof_type(type);

    // find the location at the end of the file for the chunk
    index_entry.location = handle->file_size;

    // write the data
    ssize_t bytes_written = __pwrite_retry(handle->fd, data, size, index_entry.location);
    if (bytes_written == -1 || bytes_written != size)
        {
        return -1;
        }

    // update the file_size in the handle
    handle->file_size += bytes_written;

    // update the index entry in the index
    // need to expand the index if it is already full
    if (handle->index_num_entries >= handle->header.index_allocated_entries)
        {
        int retval = __gsd_expand_index(handle);
        if (retval != 0)
            {
            return -1;
            }
        }

    // once we get here, there is a free slot to add this entry to the index
    size_t slot = handle->index_num_entries;

    // in append mode, only unwritten entries are stored in memory
    if (handle->open_flags == GSD_OPEN_APPEND)
        {
        slot -= handle->index_written_entries;
        if (slot >= handle->append_index_size)
            {
            handle->append_index_size *= 2;
            handle->index = (struct gsd_index_entry *)realloc(handle->index, handle->append_index_size*sizeof(struct gsd_index_entry));
            if (handle->index == NULL)
                {
                return -1;
                }
            }
        }
    handle->index[slot] = index_entry;
    handle->index_num_entries++;

    return 0;
    }

uint64_t gsd_get_nframes(struct gsd_handle* handle)
    {
    if (handle == NULL)
        {
        return 0;
        }
    return handle->cur_frame;
    }

const struct gsd_index_entry* gsd_find_chunk(struct gsd_handle* handle, uint64_t frame, const char *name)
    {
    if (handle == NULL)
        {
        return NULL;
        }
    if (name == NULL)
        {
        return NULL;
        }
    if (frame >= gsd_get_nframes(handle))
        {
        return NULL;
        }
    if (handle->open_flags == GSD_OPEN_APPEND)
        {
        return NULL;
        }

    // find the id for the given name
    uint16_t match_id = __gsd_get_id(handle, name, 0);
    if (match_id == UINT16_MAX)
        {
        return NULL;
        }

    // binary search for the first index entry at the requested frame
    size_t L = 0;
    size_t R = handle->index_num_entries;

    // progressively narrow the search window by halves
    do
        {
        size_t m = (L+R)/2;

        if (frame < handle->index[m].frame)
            {
            R = m;
            }
        else
            {
            L = m;
            }
        } while ((R-L) > 1);

    // this finds L = the rightmost index with the desired frame
    int64_t cur_index;

    // search all index entries with the matching frame
    for (cur_index = L; (cur_index >= 0) && (handle->index[cur_index].frame == frame); cur_index--)
        {
        // if the frame matches, check the id
        if (match_id == handle->index[cur_index].id)
            {
            return &(handle->index[cur_index]);
            }
        }

    // if we got here, we didn't find the specified chunk
    return NULL;
    }

int gsd_read_chunk(struct gsd_handle* handle, void* data, const struct gsd_index_entry* chunk)
    {
    if (handle == NULL)
        {
        return -2;
        }
    if (data == NULL)
        {
        return -2;
        }
    if (chunk == NULL)
        {
        return -2;
        }
    if (handle->open_flags == GSD_OPEN_APPEND)
        {
        return -2;
        }

    size_t size = chunk->N * chunk->M * gsd_sizeof_type((enum gsd_type)chunk->type);
    if (size == 0)
        {
        return -3;
        }
    if (chunk->location == 0)
        {
        return -3;
        }

    // validate that we don't read past the end of the file
    if ((chunk->location + size) > handle->file_size)
        {
        return -3;
        }

    ssize_t bytes_read = __pread_retry(handle->fd, data, size, chunk->location);
    if (bytes_read == -1 || bytes_read != size)
        {
        return -1;
        }

    return 0;
    }

size_t gsd_sizeof_type(enum gsd_type type)
    {
    size_t val = 0;
    if (type == GSD_TYPE_UINT8)
        {
        val = sizeof(uint8_t);
        }
    else if (type == GSD_TYPE_UINT16)
        {
        val = sizeof(uint16_t);
        }
    else if (type == GSD_TYPE_UINT32)
        {
        val = sizeof(uint32_t);
        }
    else if (type == GSD_TYPE_UINT64)
        {
        val = sizeof(uint64_t);
        }
    else if (type == GSD_TYPE_INT8)
        {
        val = sizeof(int8_t);
        }
    else if (type == GSD_TYPE_INT16)
        {
        val = sizeof(int16_t);
        }
    else if (type == GSD_TYPE_INT32)
        {
        val = sizeof(int32_t);
        }
    else if (type == GSD_TYPE_INT64)
        {
        val = sizeof(int64_t);
        }
    else if (type == GSD_TYPE_FLOAT)
        {
        val = sizeof(float);
        }
    else if (type == GSD_TYPE_DOUBLE)
        {
        val = sizeof(double);
        }
    else
        {
        return 0;
        }
    return val;
    }

const char *gsd_find_matching_chunk_name(struct gsd_handle* handle, const char* match, const char *prev)
    {
    if (handle == NULL)
        {
        return NULL;
        }
    if (match == NULL)
        {
        return NULL;
        }
    if (handle->namelist_num_entries == 0)
        {
        return NULL;
        }

    // determine search start index
    size_t start;
    if (prev == NULL)
        {
        start = 0;
        }
    else
        {
        if (prev < handle->namelist[0].name)
            {
            return NULL;
            }

        ptrdiff_t d = prev - handle->namelist[0].name;
        if (d % sizeof(struct gsd_namelist_entry) != 0)
            {
            return NULL;
            }
        start = d / sizeof(struct gsd_namelist_entry) + 1;
        }

    size_t match_len = strnlen(match, sizeof(handle->namelist[0].name));
    size_t i;
    for (i = start; i < handle->namelist_num_entries; i++)
        {
        if (0 == strncmp(match, handle->namelist[i].name, match_len))
            {
            return handle->namelist[i].name;
            }
        }

    // searched past the end of the list, return NULL
    return NULL;
    }

// undefine windows wrapper macros
#ifdef _WIN32
#undef lseek
#undef write
#undef read
#undef open
#undef ftruncate

#endif
