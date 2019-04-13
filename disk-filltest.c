/******************************************************************************
 * disk-filltest.c - Program to fill a hard disk with random data
 *
 * Usage: ./disk-filltest
 *
 * The program will fill the current directory with files called random-#####.
 * Each file is up to 1 GiB in size and contains randomly generated integers.
 * When the disk is full, writing is finished and all files are read from disk.
 * During reading the file contents is checked against the pseudo-random
 * sequence to detect changed data blocks. Any unexpected integer will output
 * an error. Reading and writing speed are shown during operation.
 *
 ******************************************************************************
 * Copyright (C) 2012-2013 Timo Bingmann <tb@panthema.net>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* random seed used */
unsigned int g_seed;

/* only perform read operation */
int gopt_readonly = 0;

/* immediately unlink files after open */
int gopt_unlink_immediate = 0;

/* unlink files after complete run */
int gopt_unlink_after = 0;

/* individual file size in MiB */
unsigned int gopt_file_size = 0;

/* file number limit */
unsigned int gopt_file_limit = 0;

/* return the current timestamp */
static inline double timestamp(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return ((double)(tv.tv_sec) + (double)(tv.tv_usec/1e6));
}

/* simple linear congruential random generator, faster than rand() and totally
 * sufficient for this cause. */
static inline uint64_t lcg_random(uint64_t *xn)
{
    *xn = 0x27BB2EE687B0B0FDLLU * *xn + 0xB504F32DLU;
    return *xn;
}

/* item type used in blocks written to disk */
typedef uint64_t item_type;

/* a list of open file handles */
int* g_filehandle = NULL;
unsigned int g_filehandle_size = 0;
unsigned int g_filehandle_limit = 0;

/* append to the list of open file handles */
static inline void filehandle_append(int fd)
{
    if (g_filehandle_size >= g_filehandle_limit)
    {
        g_filehandle_limit *= 2;
        if (g_filehandle_limit < 128) g_filehandle_limit = 128;

        g_filehandle = realloc(g_filehandle, sizeof(int) * g_filehandle_limit);
    }

    g_filehandle[ g_filehandle_size++ ] = fd;
}

/* for compatiblity with windows, use O_BINARY if available */
#ifndef O_BINARY
#define O_BINARY 0
#endif

/* print command line usage */
void print_usage(char* argv[])
{
    fprintf(stderr,
            "Usage: %s [-s seed] [-f files] [-S size] [-r] [-u] [-U] [-C dir]\n"
            "\n"
            "Options: \n"
            "  -C <dir>          Change into given directory before starting work.\n"
            "  -f <file number>  Only write this number of 1 GiB sized files.\n"
            "  -r                Only verify existing data files with given random seed.\n"
            "  -s <random seed>  Use random seed to write or verify data files.\n"
            "  -S <size>         Size of each random file in MiB (default: 1024).\n"
            "  -u                Remove files after successful test.\n"
            "  -U                Immediately remove files, write and verify via file handles.\n"
            "\n",
            argv[0]);
    exit(EXIT_FAILURE);
}

/* parse command line parameters */
void parse_commandline(int argc, char* argv[])
{
    int opt;

    while ((opt = getopt(argc, argv, "hs:S:f:ruUC:")) != -1) {
        switch (opt) {
        case 's':
            g_seed = atoi(optarg);
            break;
        case 'S':
            gopt_file_size = atoi(optarg);
            break;
        case 'f':
            gopt_file_limit = atoi(optarg);
            break;
        case 'r':
            gopt_readonly = 1;
            break;
        case 'u':
            gopt_unlink_after = 1;
            break;
        case 'U':
            gopt_unlink_immediate = 1;
            break;
        case 'C':
            if (chdir(optarg) != 0) {
                printf("Error chdir to %s: %s\n", optarg, strerror(errno));
                exit 0;
            }
            break;
        case 'h':
        default:
            print_usage(argv);
        }
    }

    if (optind < argc)
        print_usage(argv);

    if (gopt_file_size == 0)
        gopt_file_size = 1024;
}

/* unlink old random files */
void unlink_randfiles(void)
{
    unsigned int filenum = 0;

    while (filenum < UINT_MAX)
    {
        char filename[32];
        snprintf(filename, sizeof(filename), "random-%08u", filenum);

        if (unlink(filename) != 0)
            break;

        if (filenum == 0)
            printf("Removing old files .");
        else
            printf(".");
        fflush(stdout);

        ++filenum;
    }

    if (filenum > 0)
        printf(" total: %u.\n", filenum);
}

/* fill disk */
void fill_randfiles(void)
{
    unsigned int filenum = 0;
    int done = 0;

    if (gopt_file_limit == 0) gopt_file_limit = UINT_MAX;

    printf("Writing files random-######## with seed %u\n", g_seed);

    while (!done && filenum < gopt_file_limit)
    {
        char filename[32];
        int fd;
        ssize_t wtotal, wb, wp;
        unsigned int i, blocknum;
        double ts1, ts2;
        uint64_t rnd;

        item_type block[1024*1024 / sizeof(item_type)];

        snprintf(filename, sizeof(filename), "random-%08u", filenum);

        fd = open(filename, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0600);
        if (fd < 0) {
            printf("Error opening next file %s: %s\n",
                   filename, strerror(errno));
            break;
        }

        if (gopt_unlink_immediate) {
            if (unlink(filename) != 0) {
                printf("Error unlinkin opened file %s: %s\n",
                       filename, strerror(errno));
            }
        }

        /* reset random generator for each 1 GiB file */
        rnd = g_seed + (++filenum);

        wtotal = 0;
        ts1 = timestamp();

        for (blocknum = 0; blocknum < gopt_file_size; ++blocknum)
        {
            for (i = 0; i < sizeof(block) / sizeof(item_type); ++i)
                block[i] = lcg_random(&rnd);

            wp = 0;

            while ( wp != (ssize_t)sizeof(block) && !done )
            {
                wb = write(fd, (char*)block + wp, sizeof(block) - wp);

                if (wb <= 0) {
                    printf("Error writing next file %s: %s\n",
                           filename, strerror(errno));
                    done = 1;
                    break;
                }
                else {
                    wp += wb;
                }
            }

            wtotal += wp;
        }

        if (gopt_unlink_immediate) { /* do not close file handle! */
            filehandle_append(fd);
        }
        else {
            close(fd);
        }

        ts2 = timestamp();

        printf("Wrote %.0f MiB random data to %s with %f MiB/s\n",
               (wtotal / 1024.0 / 1024.0),
               filename,
               (wtotal / 1024.0 / 1024.0 / (ts2-ts1)));
        fflush(stdout);
    }

    errno = 0;
}

/* read files and check random sequence*/
void read_randfiles(void)
{
    unsigned int filenum = 0;
    int done = 0;

    printf("Verifying files random-######## with seed %u\n", g_seed);

    while (!done)
    {
        char filename[32];
        int fd;
        ssize_t rtotal, rb;
        unsigned int i, blocknum;
        double ts1, ts2;
        uint64_t rnd;

        item_type block[1024*1024 / sizeof(item_type)];

        snprintf(filename, sizeof(filename), "random-%08u", filenum);

        if (gopt_unlink_immediate)
        {
            if (filenum >= g_filehandle_size) {
                printf("Finished all opened file handles.\n");
                break;
            }

            fd = g_filehandle[filenum];

            if (lseek(fd, 0, SEEK_SET) != 0) {
                printf("Error seeking in next file %s: %s\n",
                       filename, strerror(errno));
                break;
            }
        }
        else
        {
            fd = open(filename, O_RDONLY | O_BINARY);
            if (fd < 0) {
                printf("Error opening next file %s: %s\n",
                       filename, strerror(errno));
                break;
            }
        }

        /* reset random generator for each 1 GiB file */
        rnd = g_seed + (++filenum);

        rtotal = 0;
        ts1 = timestamp();

        for (blocknum = 0; blocknum < gopt_file_size; ++blocknum)
        {
            rb = read(fd, block, sizeof(block));

            if (rb <= 0) {
                printf("Error reading file %s: %s\n",
                       filename, strerror(errno));
                done = 1;
                break;
            }

            for (i = 0; i < rb  / sizeof(item_type); ++i)
            {
                if (block[i] != lcg_random(&rnd))
                {
                    printf("Mismatch to random sequence in file %s block %d at offset %lu\n",
                           filename, blocknum, (long unsigned)(i * sizeof(int)));
                    gopt_unlink_after = 0;
                    break;
                }
            }

            rtotal += rb;
        }

        close(fd);

        ts2 = timestamp();

        printf("Read %.0f MiB random data from %s with %f MiB/s\n",
               (rtotal / 1024.0 / 1024.0),
               filename,
               (rtotal / 1024.0 / 1024.0 / (ts2-ts1)));
        fflush(stdout);
    }
}

int main(int argc, char* argv[])
{
    g_seed = time(NULL);

    parse_commandline(argc, argv);

    if (gopt_readonly)
    {
        read_randfiles();
    }
    else
    {
        unlink_randfiles();
        fill_randfiles();
        read_randfiles();
        if (gopt_unlink_after)
            unlink_randfiles();
    }

    return 0;
}
