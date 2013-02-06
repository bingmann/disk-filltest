/******************************************************************************
 * disk-filltest.cc - Program to fill a hard disk with random data
 *
 * Usage: ./disk-filltest
 *
 * The program will fill the current directory with files called rand-#####.
 * Each file is up to 1 GiB in size and contains randomly generated integers.
 * When the disk is full, writing is finished and all files are read from disk.
 * During reading the file contents is checked against the pseudo-random
 * sequence to detect changed data blocks. Any unexpected integer will output
 * an error. Reading and writing speed are shown during operation.
 *
 ******************************************************************************
 * Copyright (C) 2012 Timo Bingmann <tb@panthema.net>
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

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

double timestamp()
{
    struct timeval tv;
    gettimeofday(&tv, 0);

    return ((double)(tv.tv_sec) + (double)(tv.tv_usec/1e6));
}

int main()
{
    time_t seed = time(NULL);

    /* unlink old random files */
    {
        unsigned int filenum = 0;

        while (filenum < UINT_MAX)
        {
            char filename[32];
            snprintf(filename, sizeof(filename), "rand-%010u", filenum++);

            if (unlink(filename) != 0)
                break;

            printf("Removed old file %s\n", filename);
        }
    }

    sync();

    /* fill disk */
    {
        unsigned int filenum = 0;
        int done = 0;

        while (!done && filenum < UINT_MAX)
        {
            char filename[32];
            int fd, blocknum;
            ssize_t wtotal, wb, wp;
            unsigned int i;
            double ts1, ts2;

            int block[1024*1024 / sizeof(int)];

            snprintf(filename, sizeof(filename), "rand-%010u", filenum++);

            fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0) {
                printf("Error opening next file %s: %s\n",
                       filename, strerror(errno));
                break;
            }

            /* reset random generator for each 1 GiB file */
            srand(seed + filenum);

            wtotal = 0;
            ts1 = timestamp();

            for (blocknum = 0; blocknum < 1024; ++blocknum)
            {
                for (i = 0; i < sizeof(block) / sizeof(int); ++i)
                    block[i] = rand();

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

            close(fd);

            ts2 = timestamp();

            printf("Wrote %.0f MiB random data to %s with %f MiB/s\n",
                   (wtotal / 1024.0 / 1024.0),
                   filename,
                   (wtotal / 1024.0 / 1024.0 / (ts2-ts1)));
        }
    }

    sync();

    /* read files and check random sequence*/
    {
        unsigned int filenum = 0;
        int done = 0;

        srand(seed);

        while (!done)
        {
            char filename[32];
            int fd, blocknum;
            ssize_t rtotal, rb;
            unsigned int i;
            double ts1, ts2;

            int block[1024*1024 / sizeof(int)];

            snprintf(filename, sizeof(filename), "rand-%010u", filenum++);

            fd = open(filename, O_RDONLY);
            if (fd < 0) {
                printf("Error opening next file %s: %s\n",
                       filename, strerror(errno));
                break;
            }

            srand(seed + filenum);

            rtotal = 0;
            ts1 = timestamp();

            for (blocknum = 0; blocknum < 1024; ++blocknum)
            {
                rb = read(fd, block, sizeof(block));

                if (rb <= 0) {
                    printf("Error reading file %s: %s\n",
                           filename, strerror(errno));
                    done = 1;
                    break;
                }

                for (i = 0; i < rb  / sizeof(int); ++i)
                {
                    if (block[i] != rand())
                    {
                        printf("Mismatch to random sequence in file %s block %d at offset %lu\n",
                               filename, blocknum, i * sizeof(int));
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
        }
    }

    return 0;
}
