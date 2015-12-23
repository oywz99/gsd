/* Copyright (c) The Regents of the University of Michigan
This file is part of the General Simulation Data (GSD) project, released under the BSD 2-Clause License:
(http://opensource.org/licenses/BSD-2-Clause).
*/

#ifndef __GSD_H__
#define __GSD_H__

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! \file gsd.h
    \brief Declare GSD data types and C API
*/

//! GSD file header
/*! The GSD file header.

    \warning All members are **read-only** to the caller.
*/
struct gsd_header
    {
    uint64_t magic;
    uint32_t gsd_version;               //!< File format version: 0xaaaabbbb => aaaa.bbbb
    char application[64];               //!< Name of generating application
    char schema[64];                    //!< Name of data schema
    uint32_t schema_version;            //!< Schema version: 0xaaaabbbb => aaaa.bbbb
    uint64_t index_location;
    uint64_t index_allocated_entries;
    uint64_t namelist_location;
    uint64_t namelist_allocated_entries;
    char reserved[80];
    };

//! Index entry
/*! An index entry for a single chunk of data.

    \warning All members are **read-only** to the caller.
*/
struct gsd_index_entry
    {
    uint64_t frame;     //!< Frame index of the chunk
    uint64_t N;         //!< Number of rows in the chunk
    int64_t location;
    uint16_t id;
    uint8_t M;          //!< Number of columns in the chunk
    uint8_t type;       //!< Data type of the chunk
    };

//! Namelist entry
/*! An entry in the list of data chunk names

    \warning All members are **read-only** to the caller.
*/
struct gsd_namelist_entry
    {
    char name[128];     //!< Entry name
    };

//! File handle
/*! A handle to an open GSD file.

    This handle is obtained when opening a GSD file and is passed into every method that operates on the file.

    \warning All members are **read-only** to the caller.
*/
struct gsd_handle
    {
    int fd;
    gsd_header_t header;                //!< GSD file header
    gsd_index_entry_t *index;
    gsd_namelist_entry_t *namelist;
    uint64_t namelist_written_entries;
    uint64_t namelist_num_entries;
    uint64_t index_written_entries;
    uint64_t index_num_entries;
    uint64_t cur_frame;
    int64_t file_size;                  //!< File size (in bytes)
    uint8_t open_flags;                 //!< Flags passed to gsd_open()
    };

//! Identifiers for the gsd data chunk element types
enum gsd_type
    {
    GSD_TYPE_UINT8=1;
    GSD_TYPE_UINT16;
    GSD_TYPE_UINT32;
    GSD_TYPE_UINT64;
    GSD_TYPE_INT8;
    GSD_TYPE_INT16;
    GSD_TYPE_INT32;
    GSD_TYPE_INT64;
    GSD_TYPE_FLOAT;
    GSD_TYPE_DOUBLE;
    };

//! Flag for GSD file open options
enum gsd_open_flag
    {
    GSD_OPEN_READWRITE=1;
    GSD_OPEN_READONLY;
    };

//! Create a GSD file
int gsd_create(const char *fname, const char *application, const char *schema, uint32_t schema_version);

//! Open a GSD file for read/write
gsd_handle_t* gsd_open(const char *fname, const uint8_t flags);

//! Close a GSD file
int gsd_close(gsd_handle_t* handle);

//! Move on to the next frame
int gsd_end_frame(gsd_handle_t* handle);

//! Write a data chunk to the current frame
int gsd_write_chunk(gsd_handle_t* handle,
                     const char *name,
                     uint8_t type,
                     uint64_t N,
                     uint64_t M,
                     uint64_t step,
                     const void *data);

//! Find a chunk in the GSD file
gsd_index_entry_t* gsd_find_chunk(gsd_handle_t* handle, uint64_t frame, const char *name);

//! Read a chunk from the GSD file
int gsd_read_chunk(gsd_handle_t* handle, void* data, const gsd_index_entry_t* chunk);

//! Query the last time step in the GSD file
uint64_t gsd_get_last_step(gsd_handle_t* handle);

//! Get the number of frames in the GSD file
uint64_t gsd_get_nframes(gsd_handle_t* handle);

//! Query size of a GSD type ID
size_t gsd_sizeof_type(uint8_t type);

#ifdef __cplusplus
}
#endif

#endif  // #ifndef __GSD_H__
