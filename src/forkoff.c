/*
Copyright (c) 2014-2020, The Monero Project

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Parts of the project are originally copyright (c) 2012-2013 The Cryptonote
developers.
*/

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>

void
forkoff(const char *pidname)
{
    FILE *pidfile = NULL;
    if (pidname)
    {
        pidfile = fopen(pidname, "r");
        unsigned pid;
        if (pidfile)
        {
            if (fscanf(pidfile, "%u", &pid) > 0 && pid > 1 && kill(pid, 0) == 0)
            {
                printf("Already running, no need to start again.\n");
                fclose(pidfile);
                exit(1);
            }
            fclose(pidfile);
            pidfile = 0;
        }
        pidfile = fopen(pidname, "w");
        if (!pidfile)
        {
            printf("Cannot open PID file for writing. Aborting.\n");
            exit(1);
        }
    }

    pid_t pid;
    if ((pid = fork()))
    {
        if (pid > 0)
        {
            if (pidfile) fclose(pidfile);
            exit(0);
        }
        else
        {
            printf("First fork failed. Aborting.\n");
            exit(1);
        }
    }

    setsid();

    if ((pid = fork()))
    {
        if (pid > 0)
        {
            if (pidfile) fclose(pidfile);
            exit(0);
        }
        else
        {
            printf("Second fork failed. Aborting.\n");
            exit(1);
        }
    }

    if (pidfile)
    {
        fprintf(pidfile, "%u", getpid());
        fclose(pidfile);
    }

    close(0);
    close(1);
    close(2);

    /* no stdin */
    if (open("/dev/null", O_RDONLY) < 0)
    {
        printf("Failed to open /dev/null for input. Aborting.\n");
        exit(1);
    }
    /* no stdout */
    if (open("/dev/null", O_WRONLY) < 0)
    {
        printf("Failed to open /dev/null for output. Aborting.\n");
        exit(1);
    }
    /* stdout to stderror */
    if (dup(1) < 0)
    {
        printf("Unable to dup stdout to stderror\n");
        exit(1);
    }
}
