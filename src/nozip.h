#ifndef NOZIP_H
#define NOZIP_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#if defined(NOZIP_STATIC)
#define NOZIPDEF static
#elif defined(__cplusplus)
#define NOZIPDEF extern "C"
#else
#define NOZIPDEF extern
#endif

struct zip_entry {
    uint64_t uncompressed_size;
    uint64_t compressed_size;
    uint64_t local_header_offset;
    const char *filename;
    time_t mtime;
};

NOZIPDEF size_t zip_read(struct zip_entry **ptr, FILE *stream);
NOZIPDEF int zip_seek(FILE *stream, const struct zip_entry *entry);

#endif // NOZIP_H

#ifdef NOZIP_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define PACK(x) x __attribute__((__packed__))
#elif defined(_MSC_VER)
#define PACK(x) x __pragma(pack(push, 1)) x __pragma(pack(pop))
#else
#error Unsupported compiler
#endif

PACK(struct local_file_header {
    uint32_t signature;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t compression_method;
    uint16_t last_mod_file_time;
    uint16_t last_mod_file_date;
    uint32_t crc_32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t file_name_length;
    uint16_t extra_field_length;
});

PACK(struct central_dir_header {
    uint32_t signature;
    uint16_t version;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t compression_method;
    uint16_t last_mod_file_time;
    uint16_t last_mod_file_date;
    uint32_t crc_32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t file_name_length;
    uint16_t extra_field_length;
    uint16_t file_comment_length;
    uint16_t disk_number_start;
    uint16_t internal_file_attributes;
    uint32_t external_file_attributes;
    uint32_t local_header_offset;
});

PACK(struct end_of_central_dir_record64 {
    uint32_t signature;
    uint64_t eocdr_size;
    uint16_t version;
    uint16_t version_needed;
    uint32_t disk_number;
    uint32_t cdr_disk_number;
    uint64_t disk_num_entries;
    uint64_t num_entries;
    uint64_t cdr_size;
    uint64_t cdr_offset;
});

PACK(struct end_of_central_dir_locator64 {
    uint32_t signature;
    uint32_t eocdr_disk;
    uint64_t eocdr_offset;
    uint32_t num_disks;
});

PACK(struct end_of_central_dir_record {
    uint32_t signature;
    uint16_t disk_number;
    uint16_t cdr_disk_number;
    uint16_t disk_num_entries;
    uint16_t num_entries;
    uint32_t cdr_size;
    uint32_t cdr_offset;
    uint16_t ZIP_file_comment_length;
});

size_t zip_read(struct zip_entry **ptr, FILE *stream) {
    // find the end of central directory record
    uint32_t signature;
    off_t offset;
    for (offset = sizeof(struct end_of_central_dir_record);; ++offset) {
        if (offset > UINT16_MAX || fseeko(stream, -offset, SEEK_END) || !fread(&signature, sizeof(signature), 1, stream))
            return 0;
        if (signature == 0x06054B50)
            break;
    }

    // read end of central directory record
    struct end_of_central_dir_record eocdr;
    if (!(fseeko(stream, -offset, SEEK_END) == 0 &&
          fread(&eocdr, sizeof(eocdr), 1, stream) &&
          eocdr.signature == 0x06054B50 &&
          eocdr.disk_number == 0 &&
          eocdr.cdr_disk_number == 0 &&
          eocdr.disk_num_entries == eocdr.num_entries))
        return 0;

    // check for zip64
    struct end_of_central_dir_record64 eocdr64;
    int zip64 = eocdr.num_entries == UINT16_MAX || eocdr.cdr_offset == UINT32_MAX || eocdr.cdr_size == UINT32_MAX;
    if (zip64) {
        // zip64 end of central directory locator
        struct end_of_central_dir_locator64 eocdl64;
        if (!(fseeko(stream, -offset - sizeof(eocdl64), SEEK_END) == 0 &&
              fread(&eocdl64, sizeof(eocdl64), 1, stream) &&
              eocdl64.signature == 0x07064B50 &&
              eocdl64.eocdr_disk == 0 &&
              eocdl64.num_disks == 1))
            return 0;
        // zip64 end of central directory record
        if (!(fseeko(stream, eocdl64.eocdr_offset, SEEK_SET) == 0 &&
              fread(&eocdr64, sizeof(eocdr64), 1, stream) &&
              eocdr64.signature == 0x06064B50 &&
              eocdr64.disk_number == 0 &&
              eocdr64.cdr_disk_number == 0 &&
              eocdr64.disk_num_entries == eocdr64.num_entries))
            return 0;
    }

    // seek to central directory record
    if (fseeko(stream, zip64 ? eocdr64.cdr_offset : eocdr.cdr_offset, SEEK_SET))
        return 0;

    // alloc buffer for entries array and filenames
    struct zip_entry *entries = (struct zip_entry *)malloc(zip64 ? eocdr64.cdr_size : eocdr.cdr_size);
    if (!entries)
        return 0;

    // store filenames after entries array
    char *strings = (char *)(entries + (zip64 ? eocdr64.num_entries : eocdr.num_entries));

    for (size_t i = 0, i_end = zip64 ? eocdr64.num_entries : eocdr.num_entries; i < i_end; ++i) {
        // read central directory header, filename, extra field and skip comment
        struct central_dir_header cdh;
        if (!(fread(&cdh, sizeof(cdh), 1, stream) &&
              cdh.signature == 0x02014B50 &&
              fread(strings, cdh.file_name_length + cdh.extra_field_length, 1, stream) &&
              fseeko(stream, cdh.file_comment_length, SEEK_CUR) == 0)) {
            free(entries);
            return 0;
        }

        struct zip_entry *entry = entries + i;
        entry->uncompressed_size = cdh.uncompressed_size;
        entry->compressed_size = cdh.compressed_size;
        entry->local_header_offset = cdh.local_header_offset;

        // find zip64 extended information extra field
        for (char *extra = strings + cdh.file_name_length; extra != strings + cdh.file_name_length + cdh.extra_field_length;) {
            uint16_t header_id;
            memcpy(&header_id, extra, sizeof(header_id));
            extra += sizeof(header_id);

            uint16_t data_size;
            memcpy(&data_size, extra, sizeof(data_size));
            extra += sizeof(data_size);

            switch (header_id) {
            case 0x0001:
                if (cdh.uncompressed_size == UINT32_MAX) {
                    memcpy(&entry->uncompressed_size, extra, sizeof(entry->uncompressed_size));
                    extra += sizeof(entry->uncompressed_size);
                }
                if (cdh.compressed_size == UINT32_MAX) {
                    memcpy(&entry->compressed_size, extra, sizeof(entry->compressed_size));
                    extra += sizeof(entry->compressed_size);
                }
                if (cdh.local_header_offset == UINT32_MAX) {
                    memcpy(&entry->local_header_offset, extra, sizeof(entry->local_header_offset));
                    extra += sizeof(entry->local_header_offset);
                }
                if (cdh.disk_number_start == UINT16_MAX) {
                    extra += sizeof(uint32_t);
                }
                break;
            default:
                extra += data_size;
                break;
            }
        }

        entry->filename = strings;
        strings += cdh.file_name_length;
        *strings++ = '\0';
        entry->mtime = mktime(&(struct tm){
            .tm_sec = (cdh.last_mod_file_time << 1) & 0x3F,
            .tm_min = (cdh.last_mod_file_time >> 5) & 0x3F,
            .tm_hour = (cdh.last_mod_file_time >> 11) & 0x1F,
            .tm_mday = cdh.last_mod_file_date & 0x1F,
            .tm_mon = ((cdh.last_mod_file_date >> 5) & 0xF) - 1,
            .tm_year = ((cdh.last_mod_file_date >> 9) & 0x7F) + 1980 - 1900,
            .tm_isdst = -1,
        });
    }

    *ptr = entries;
    return zip64 ? eocdr64.num_entries : eocdr.num_entries;
}

int zip_seek(FILE *stream, const struct zip_entry *entry) {
    struct local_file_header lfh;
    return !(fseeko(stream, entry->local_header_offset, SEEK_SET) == 0 &&
             fread(&lfh, sizeof(lfh), 1, stream) &&
             lfh.signature == 0x04034B50 &&
             fseeko(stream, lfh.file_name_length + lfh.extra_field_length, SEEK_CUR) == 0);
}

int zip_store(FILE *stream, const char *filename, const void *data, size_t size) {
    off_t offset = ftell(stream);
    if (offset == -1)
        return 1;

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);

    struct local_file_header lfh = {
        .signature = 0x04034B50,
        .version_needed = 10,
        .flags = 0,
        .compression_method = 0,
        .last_mod_file_time = tm->tm_hour << 11 | tm->tm_min << 5 | tm->tm_sec >> 1,
        .last_mod_file_date = (tm->tm_year - 80) << 9 | (tm->tm_mon + 1) << 5 | tm->tm_mday,
        .crc_32 = 0,
        .compressed_size = size,
        .uncompressed_size = size,
        .file_name_length = strlen(filename),
        .extra_field_length = 0,
    };

    fwrite(&lfh, sizeof(lfh), 1, stream);
    fwrite(filename, lfh.file_name_length, 1, stream);
    fwrite(data, size, 1, stream);

    return 0;
}

int zip_finalize(FILE *stream) {
    off_t offset = 0;
    struct local_file_header lfh;
    char filename[1024];

    struct end_of_central_dir_record eocdr = {
        .signature = 0x06054B50,
        .disk_number = 0,
        .cdr_disk_number = 0,
        .disk_num_entries = 0,
        .num_entries = 0,
        .cdr_size = 0,
        .cdr_offset = ftello(stream),
        .ZIP_file_comment_length = 0,
    };

    while (fseeko(stream, offset, SEEK_SET) == 0 &&
           fread(&lfh, sizeof(lfh), 1, stream) &&
           lfh.signature == 0x04034B50 &&
           lfh.file_name_length < sizeof(filename) &&
           fread(filename, lfh.file_name_length, 1, stream)) {

        printf("F %.*s\n", lfh.file_name_length, filename);
        struct central_dir_header cdh = {
            .signature = 0x02014B50,
            .version = 10,
            .version_needed = lfh.version_needed,
            .flags = lfh.flags,
            .compression_method = lfh.compression_method,
            .last_mod_file_time = lfh.last_mod_file_time,
            .last_mod_file_date = lfh.last_mod_file_date,
            .crc_32 = lfh.crc_32,
            .compressed_size = lfh.compressed_size,
            .uncompressed_size = lfh.uncompressed_size,
            .file_name_length = lfh.file_name_length,
            .extra_field_length = 0,
            .file_comment_length = 0,
            .disk_number_start = 0,
            .internal_file_attributes = 0,
            .external_file_attributes = 0,
            .local_header_offset = offset,
        };
        fseeko(stream, 0, SEEK_END);
        fwrite(&cdh, sizeof(cdh), 1, stream);
        fwrite(filename, lfh.file_name_length, 1, stream);

        ++eocdr.num_entries;
        ++eocdr.disk_num_entries;

        offset += sizeof(lfh) + lfh.file_name_length + lfh.compressed_size;
    }

    fseeko(stream, 0, SEEK_END);
    eocdr.cdr_size = ftello(stream) - eocdr.cdr_offset;
    fwrite(&eocdr, sizeof(eocdr), 1, stream);

    return 0;
}

#endif // NOZIP_IMPLEMENTATION
