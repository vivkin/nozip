#include "nozip.h"
#include "stb_inflate.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

int main(int argc, char **argv) {
#if 0
    ZIP_GENERATE(ZIP_EXTRA_FIELD_HEADER_NEW);
    ZIP_GENERATE(ZIP64_EXTENDED_INFORMATION_EXTRA_FIELD_NEW);
    ZIP_GENERATE(ZIP_LOCAL_FILE_HEADER);
    ZIP_GENERATE(ZIP_CENTRAL_DIR_HEADER_NEW);
    ZIP_GENERATE(ZIP64_END_OF_CENTRAL_DIR_RECORD_NEW);
    ZIP_GENERATE(ZIP64_END_OF_CENTRAL_DIR_LOCATOR_NEW);
    ZIP_GENERATE(ZIP_END_OF_CENTRAL_DIR_RECORD_NEW);

    dump(u, ZIP_SIZEOF(ZIP_LOCAL_FILE_HEADER));
    dump(u, ZIP_CENTRAL_DIR_HEADER(ZIP_POS_SIZE));
    dump(u, ZIP_END_OF_CENTRAL_DIR_RECORD(ZIP_POS_SIZE));
    dump(u, ZIP64_END_OF_CENTRAL_DIR_RECORD(ZIP_POS_SIZE));
    dump(u, ZIP64_END_OF_CENTRAL_DIR_LOCATOR(ZIP_POS_SIZE));
#endif

    if (argc < 3) {
        fprintf(stderr, "usage: %s [-lvx] file [file ...]\n", argv[0]);
        return EXIT_FAILURE;
    }

    int mode;
    if (!strcmp("-l", argv[1]))
        mode = 'l';
    else if (!strcmp("-v", argv[1]))
        mode = 'v';
    else if (!strcmp("-x", argv[1]))
        mode = 'x';
    else if (!strcmp("-z", argv[1]))
        mode = 'z';
    else {
        fprintf(stderr, "%s: illegal option -- %s\n", argv[0], argv[1]);
        return EXIT_FAILURE;
    }

    FILE *fp = fopen(argv[2], "rb");
    if (!fp) {
        perror(argv[2]);
        return EXIT_FAILURE;
    }

    struct zip_entry *entries = NULL;
    size_t num_entries = zip_read(&entries, fp);
    if (num_entries == 0 || entries == NULL) {
        perror(argv[2]);
        return EXIT_FAILURE;
    }

    switch (mode) {
    case 'l':
        for (size_t i = 0; i < num_entries; ++i) {
            printf("%s\n", entries[i].filename);
        }
        break;
    case 'v':
        for (size_t i = 0; i < num_entries; ++i) {
            struct zip_entry *e = entries + i;
            char buf[32];
            strftime(buf, sizeof(buf), "%Y %b %d %H:%M", localtime(&e->mtime));
            printf("%10" PRIu64 " %10" PRIu64 " %10" PRIu64 " %s %s\n",
                   e->local_header_offset, e->compressed_size, e->uncompressed_size, buf, e->filename);
        }
        break;
    case 'x':
    case 'z':
        for (int argi = 3; argi < argc; ++argi) {
            for (size_t i = 0; i < num_entries; ++i) {
                struct zip_entry *e = entries + i;
                if (!strcmp(argv[argi], e->filename)) {
                    if (e->uncompressed_size == 0)
                        continue;
                    if (zip_seek(fp, e)) {
                        perror(argv[argi]);
                        return EXIT_FAILURE;
                    }
                    if (e->compressed_size == e->uncompressed_size) {
                        void *buf = malloc(e->compressed_size);
                        if (!buf) {
                            perror(argv[argi]);
                            return EXIT_FAILURE;
                        }
                        if (fread(buf, e->compressed_size, 1, fp) == 0) {
                            perror(argv[argi]);
                            return EXIT_FAILURE;
                        }
#if 1
                        int zip_store(FILE *stream, const char *filename, const void *data, size_t size);
                        int zip_finalize(FILE *stream);
                        FILE *fp = fopen("test.zip", "w+b");
                        if (fp) {
                            zip_store(fp, e->filename, buf, e->compressed_size);
                            zip_store(fp, "foo", "hello world\n", 12);
                            zip_finalize(fp);
                            fclose(fp);
                        }
#endif

                        if (fwrite(buf, e->compressed_size, 1, stdout) == 0) {
                            perror(argv[argi]);
                            return EXIT_FAILURE;
                        }
                        free(buf);
                    } else {
#if 0
                        void *buf = malloc(e->compressed_size);
                        void *out = malloc(e->uncompressed_size);
                        if (fread(buf, e->compressed_size, 1, fp) == 0) {
                            perror(argv[argi]);
                            return EXIT_FAILURE;
                        }

                        if (mode == 'z') {
                            z_stream stream = {
                                .next_in = buf,
                                .avail_in = e->compressed_size,
                                .next_out = out,
                                .avail_out = e->uncompressed_size,
                            };
                            inflateInit2(&stream, -MAX_WBITS);
                            while (stream.avail_out) {
                                int ret = inflate(&stream, Z_NO_FLUSH);
                                if (ret == Z_STREAM_END)
                                    break;
                                if (ret != Z_OK) {
                                    fprintf(stderr, "error: uncompress: %d\n", ret);
                                    break;
                                }
                            }
                            inflateEnd(&stream);
                        } else {
                            struct stbi__stream stream;
                            memset(&stream, 0, sizeof(stream));

                            stream.start_in = stream.next_in = buf;
                            stream.end_in = stream.start_in + e->compressed_size;

                            if (!stb_inflate(&stream)) {
                                perror(argv[argi]);
                                return EXIT_FAILURE;
                            }
                        }

                        if (mode == 'z' && fwrite(out, e->uncompressed_size, 1, stdout) == 0) {
                            perror(argv[argi]);
                            return EXIT_FAILURE;
                        }
                        free(buf);
                        free(out);
#else
                        struct stbi__stream stream;
                        memset(&stream, 0, sizeof(stream));

                        uint8_t buffer[BUFSIZ];
                        stream.start_in = buffer;
                        stream.end_in = stream.next_in = buffer + sizeof(buffer);
                        stream.cookie_in = fp;
                        stream.refill = refill_stdio;

                        uint8_t window[1 << 15];
                        stream.start_out = stream.next_out = window;
                        stream.end_out = window + sizeof(window);
                        stream.cookie_out = stdout;
                        stream.flush = flush_stdio;

                        if (!stb_inflate(&stream)) {
                            perror(argv[argi]);
                            return EXIT_FAILURE;
                        }
#endif
                    }
                }
            }
        }
        break;
    }

    free(entries);
    fclose(fp);

    return 0;
}
