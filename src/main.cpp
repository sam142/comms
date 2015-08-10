/*
 *Copyright (c) 2013-2014, yinqiwen <yinqiwen@gmail.com>
 *All rights reserved.
 * 
 *Redistribution and use in source and binary forms, with or without
 *modification, are permitted provided that the following conditions are met:
 * 
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Redis nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 * 
 *THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS 
 *BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF 
 *THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "comms.hpp"

#include <signal.h>
#include <limits.h>
#include <stdlib.h>

void version()
{
    printf("comms v=%s bits=%d \n", COMMS_VERSION,
                    sizeof(long) == 4 ? 32 : 64);
    exit(0);
}

void usage()
{
    fprintf(stderr, "Usage: ./comms [/path/to/comms.conf] [options]\n");
    fprintf(stderr, "       ./comms -v or --version\n");
    fprintf(stderr, "       ./comms -h or --help\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr,
                    "       ./comms (run the server with default conf)\n");
    fprintf(stderr, "       ./comms /etc/comms/16379.conf\n");
    fprintf(stderr, "       ./comms /etc/comms.conf \n\n");
    exit(1);
}

int signal_setting()
{
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

int main(int argc, char** argv)
{
    Properties props;
    std::string confpath;
    if (argc >= 2)
    {
        int j = 1; /* First option to parse in argv[] */
        char *configfile = NULL;

        /* Handle special options --help and --version */
        if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) version();
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) usage();

        /* First argument is the config file name? */
        if (argv[j][0] != '-' || argv[j][1] != '-')
        {
            configfile = argv[j++];
            if (!parse_conf_file(configfile, props, " "))
            {
                printf("Error: Failed to parse conf file:%s\n", configfile);
                return -1;
            }
            char readpath_buf[4096];
            if(NULL != realpath(configfile, readpath_buf))
            {
                confpath = readpath_buf;
            }
        }
    }
    else
    {
        printf(
                        "Warning: no config file specified, using the default config. In order to specify a config file use %s /path/to/comms.conf\n",
                        argv[0]);
    }
    signal_setting();
    CommsConfig cfg;
    if (!cfg.Parse(props))
    {
        printf("Failed to parse config file.\n");
        return -1;
    }
    Comms server;
    if(0 == server.Init(cfg))
    {
        server.GetConfig().conf_path = confpath;
        server.Start();
    }
    return 0;
}

