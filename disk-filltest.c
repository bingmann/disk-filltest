/******************************************************************************
 * disk-filltest.c - Program to fill a hard disk with random data
 *
 * Usage: ./disk-filltest
 *
 * The program will fill the current directory with files called random-#####.
 * Each file is up to 1 GiB in size and contains randomly generated integers.
 * When the disk is full, writing is finished and all files are read from disk.
 * During reading the file contents is checked against the pseudo-random
 * sequence to detect changed data blocks. Any unexpected value will output
 * an error. Reading and writing speed are shown during operation.
 *
 ******************************************************************************
 * Copyright (C) 2012-2020 Timo Bingmann <tb@panthema.net>
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

#define VERSION "0.8.2"

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

#if defined(_MSC_VER) || defined(__MINGW32__)
  #include <fileapi.h>
  #define HAVE_FILEAPI 1
#else
  #include <sys/statvfs.h>
  #define HAVE_STATVFS 1
#endif

/* random seed used */
unsigned int g_seed;

/* only perform read operation */
int gopt_readonly = 0;

/* immediately unlink files after open */
int gopt_unlink_immediate = 0;

/* unlink files after complete run */
int gopt_unlink_after = 0;

/* skip file verification (e.g. for wiping a disk) */
int gopt_skip_verify = 0;

/* individual file size in MiB */
unsigned int gopt_file_size = 0;

/* file number limit */
unsigned int gopt_file_limit = UINT_MAX;

/* number of repetitions */
int gopt_repeat = 1;

/* size of last file written */
unsigned int g_last_filesize = UINT_MAX;

/* return the current timestamp */
double timestamp(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return ((double)(tv.tv_sec) + (double)(tv.tv_usec/1e6));
}

/* simple linear congruential random generator, faster than rand() and totally
 * sufficient for this cause. */
uint64_t lcg_random(uint64_t *xn)
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
void filehandle_append(int fd)
{
    if (g_filehandle_size >= g_filehandle_limit)
    {
        int* new_filehandle;

        g_filehandle_limit *= 2;
        if (g_filehandle_limit < 128) g_filehandle_limit = 128;

        new_filehandle = realloc(
            g_filehandle, sizeof(int) * g_filehandle_limit);
        if (!new_filehandle) {
            fprintf(stderr,
                    "Out of memory when allocating new file handle buffer.\n");
            exit(EXIT_FAILURE);
        }
        g_filehandle = new_filehandle;
    }

    g_filehandle[ g_filehandle_size++ ] = fd;
}

/* produce nicely formatted time in seconds */
void format_time(unsigned int sec, char output[64])
{
    /* maximum digits of 32-bit unsigned int are 9. */
    if (sec >= 24 * 3600) {
        unsigned int days = sec / (24 * 3600);
        sec -= days * (24 * 3600);
        unsigned int hours = sec / 3600;
        sec -= hours * 3600;
        unsigned int minutes = sec / 60;
        sec -= minutes * 60;
        sprintf(output, "%ud%uh%um%us", days, hours, minutes, sec);
    }
    else if (sec >= 3600) {
        unsigned int hours = sec / 3600;
        sec -= hours * 3600;
        unsigned int minutes = sec / 60;
        sec -= minutes * 60;
        sprintf(output, "%uh%um%us", hours, minutes, sec);
    }
    else if (sec >= 60) {
        unsigned int minutes = sec / 60;
        sec -= minutes * 60;
        sprintf(output, "%um%us", minutes, sec);
    }
    else {
        sprintf(output, "%us", sec);
    }
}

/* for compatibility with windows, use O_BINARY if available */
#ifndef O_BINARY
#define O_BINARY 0
#endif

/* print command line usage */
void print_usage(char* argv[])
{
    fprintf(stderr,
            "Usage: %s [-s seed] [-f files] [-S size] [-r] [-u] [-U] [-C dir]\n"
            "\n"
            "disk-filltest " VERSION " is a simple program which fills a path with random\n"
            "data and then rereads the files to check that the random sequence was\n"
            "correctly stored.\n"
            "\n"
            "Options: \n"
            "  -C <dir>          Change into given directory before starting work.\n"
            "  -f <file number>  Only write this number of 1 GiB sized files.\n"
            "  -N                Skip verification, e.g. for just wiping a disk.\n"
            "  -r                Only verify existing data files with given random seed.\n"
            "  -R <times>        Repeat fill/test/wipe steps given number of times.\n"
            "  -s <random seed>  Use random seed to write or verify data files.\n"
            "  -S <size>         Size of each random file in MiB (default: 1024).\n"
            "  -u                Remove files after successful test.\n"
            "  -U                Immediately remove files, write and verify via file handles.\n"
            "  -V                Print version and exit.\n"
            "\n",
            argv[0]);
    exit(EXIT_FAILURE);
}

/* parse command line parameters */
void parse_commandline(int argc, char* argv[])
{
    int opt;

    while ((opt = getopt(argc, argv, "hs:S:f:ruUC:NR:V")) != -1) {
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
                exit(EXIT_FAILURE);
            }
            break;
        case 'N':
            gopt_skip_verify = 1;
            break;
        case 'R':
            gopt_repeat = atoi(optarg);
            break;
	case 'V':
	    printf("disk-filltest " VERSION "\n");
            exit(EXIT_SUCCESS);
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
        sprintf(filename, "random-%08u", filenum);

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
void write_randfiles(void)
{
    unsigned int filenum = 0;
    int done = 0;
    unsigned int expected_file_limit = UINT_MAX;

    if (gopt_file_limit == UINT_MAX) {
#if HAVE_FILEAPI
        ULARGE_INTEGER free_size;

        if (GetDiskFreeSpaceEx(NULL, &free_size, NULL, NULL)) {
            expected_file_limit = (free_size.QuadPart + gopt_file_size - 1)
                / (1024 * 1024) / gopt_file_size;
        }
#elif HAVE_STATVFS
        struct statvfs buf;

        if (statvfs(".", &buf) == 0) {
            uint64_t free_size =
                (uint64_t)(buf.f_blocks) * (uint64_t)(buf.f_bsize);

            expected_file_limit = (free_size + gopt_file_size - 1)
                / (1024 * 1024) / gopt_file_size;
        }
#endif
    }
    else {
        expected_file_limit = gopt_file_limit;
    }

    printf("Writing files random-######## with seed %u\n", g_seed);

    while (!done && filenum < gopt_file_limit)
    {
        char filename[32], eta[64];
        int fd;
        ssize_t wb;
        unsigned int i, blocknum, wp;
        uint64_t wtotal;
        double ts1, ts2, speed;
        uint64_t rnd;

        item_type block[(1024 * 1024) / sizeof(item_type)];

        sprintf(filename, "random-%08u", filenum);

        fd = open(filename, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0600);
        if (fd < 0) {
            printf("Error opening next file %s: %s\n",
                   filename, strerror(errno));
            break;
        }

        if (gopt_unlink_immediate) {
            if (unlink(filename) != 0) {
                printf("Error unlinking opened file %s: %s\n",
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

            while ( wp != sizeof(block) && !done )
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

        speed = wtotal / 1024.0 / 1024.0 / (ts2 - ts1);
        g_last_filesize = wtotal;

        if (expected_file_limit != UINT_MAX && filenum <= expected_file_limit) {
            format_time(
                (expected_file_limit - filenum) * gopt_file_size / speed, eta);

            printf("Wrote %.0f MiB random data to %s with %f MiB/s, eta %s.\n",
                   (wtotal / 1024.0 / 1024.0), filename, speed, eta);
        }
        else {
            printf("Wrote %.0f MiB random data to %s with %f MiB/s.\n",
                   (wtotal / 1024.0 / 1024.0), filename, speed);
        }
        fflush(stdout);
    }

    errno = 0;
}

/* read files and check random sequence*/
void read_randfiles(void)
{
    unsigned int filenum = 0;
    int done = 0;
    unsigned int expected_file_limit = UINT_MAX;

    if (gopt_unlink_immediate) {
        expected_file_limit = g_filehandle_size;
    }
    else {
        char filename[32];
        int fd;

        for (expected_file_limit = 0; ; ++expected_file_limit) {
            /* attempt to open file */
            sprintf(filename, "random-%08u", expected_file_limit);
            fd = open(filename, O_RDONLY | O_BINARY);
            if (fd < 0)
                break;
            close(fd);
        }
    }

    printf("Verifying %u files random-######## with seed %u\n",
           expected_file_limit, g_seed);

    while (!done)
    {
        char filename[32], eta[64];
        int fd;
        ssize_t rb;
        unsigned int i, blocknum;
        uint64_t rtotal;
        double ts1, ts2, speed;
        uint64_t rnd;

        item_type block[(1024 * 1024) / sizeof(item_type)];

        sprintf(filename, "random-%08u", filenum);

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
                exit(EXIT_FAILURE);
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
            unsigned int read_size = sizeof(block);
            if (filenum == expected_file_limit && g_last_filesize != UINT_MAX &&
                blocknum * sizeof(block) > g_last_filesize) {
                read_size = g_last_filesize - (blocknum - 1) * sizeof(block);
            }
            rb = read(fd, block, read_size);

            if (rb == 0) {
                /* got EOF on file */
                if (filenum != expected_file_limit ||
                    (g_last_filesize != UINT_MAX && rtotal != g_last_filesize))
                {
                    printf("Unexpectedly short file %s: "
                           "read %u of expected %"PRIu64" bytes\n",
                           filename, g_last_filesize, rtotal);
                    done = 1;
                    exit(EXIT_FAILURE);
                }

                done = 1;
                break;
            }
            else if (rb < 0) {
                printf("Error reading file %s: %s\n",
                       filename, strerror(errno));
                done = 1;
                exit(EXIT_FAILURE);
            }

            for (i = 0; i < rb  / sizeof(item_type); ++i)
            {
                if (block[i] != lcg_random(&rnd))
                {
                    printf("Mismatch to random sequence "
                           "in file %s block %d at offset %lu\n",
                           filename, blocknum,
                           (long unsigned)(i * sizeof(int)));
                    gopt_unlink_after = 0;
                    exit(EXIT_FAILURE);
                }
            }

            rtotal += rb;
        }

        close(fd);

        ts2 = timestamp();

        speed = rtotal / 1024.0 / 1024.0 / (ts2 - ts1);
        format_time(
            (expected_file_limit - filenum) * gopt_file_size / speed, eta);

        printf("Read %.0f MiB random data from %s with %f MiB/s, eta %s.\n",
               (rtotal / 1024.0 / 1024.0), filename, speed, eta);
        fflush(stdout);
    }

    printf("Successfully verified %u files random-######## with seed %u\n",
           expected_file_limit, g_seed);
}

int main(int argc, char* argv[])
{
    int r;

    g_seed = time(NULL);

    parse_commandline(argc, argv);

    for (r = 0; r < gopt_repeat; ++r)
    {
        if (gopt_readonly)
        {
            read_randfiles();
            if (gopt_unlink_after)
                unlink_randfiles();
        }
        else
        {
            unlink_randfiles();
            write_randfiles();
            if (!gopt_skip_verify)
                read_randfiles();
            if (gopt_unlink_after)
                unlink_randfiles();
        }
    }

    return 0;
}

/******************************************************************************/
