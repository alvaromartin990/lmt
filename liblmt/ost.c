/*****************************************************************************
 *  Copyright (C) 2007-2010 Lawrence Livermore National Security, LLC.
 *  This module written by Jim Garlick <garlick@llnl.gov>.
 *  UCRL-CODE-232438
 *  All Rights Reserved.
 *
 *  This file is part of Lustre Monitoring Tool, version 2.
 *  Authors: H. Wartens, P. Spencer, N. O'Neill, J. Long, J. Garlick
 *  For details, see http://code.google.com/p/lmt/.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License (as published by the
 *  Free Software Foundation) version 2, dated June 1991.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software Foundation,
 *  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA or see
 *  <http://www.gnu.org/licenses/>.
 *****************************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#if STDC_HEADERS
#include <string.h>
#endif /* STDC_HEADERS */
#include <errno.h>
#include <sys/utsname.h>
#include <stdint.h>
#include <math.h>

#include "list.h"
#include "hash.h"

#include "proc.h"
#include "stat.h"
#include "meminfo.h"
#include "lustre.h"

#include "lmt.h"
#include "ost.h"
#include "util.h"

typedef struct {
    uint64_t    usage[2];
    uint64_t    total[2];
    int         valid;      /* number of valid samples [0,1,2] */
} usage_t;

static int
_get_cpu_usage (pctx_t ctx, double *fp)
{
    static usage_t u = { .valid = 0 };

    u.usage[0] = u.usage[1];
    u.total[0] = u.total[1];

    if (proc_stat2 (ctx, &u.usage[1], &u.total[1]) < 0) {
        if (u.valid > 0)
            u.valid--;
    } else {
        if (u.valid < 2)
            u.valid++;
    }
    if (u.valid == 2) {
        *fp = fabs ((double)(u.usage[1] - u.usage[0]) 
                  / (double)(u.total[1] - u.total[0])) * 100.0;
        return 0;
    }
    return -1;
}

static int
_get_mem_usage (pctx_t ctx, double *fp)
{
    uint64_t kfree, ktot;

    if (proc_meminfo (ctx, &ktot, &kfree) < 0)
        return -1;
    *fp = ((double)(ktot - kfree) / (double)(ktot)) * 100.0;
    return 0;
}

static int
_get_oststring (pctx_t ctx, char *name, char *s, int len)
{
    char *uuid = NULL;
    uint64_t filesfree, filestotal;
    uint64_t kbytesfree, kbytestotal;
    uint64_t read_bytes, write_bytes;
    int n, retval = -1;

    if (proc_lustre_uuid (ctx, name, &uuid) < 0)
        goto done;
    if (proc_lustre_files (ctx, name, &filesfree, &filestotal) < 0)
        goto done;
    if (proc_lustre_kbytes (ctx, name, &kbytesfree, &kbytestotal) < 0)
        goto done;
    if (proc_lustre_rwbytes (ctx, name, &read_bytes, &write_bytes) < 0)
        goto done;
    n = snprintf (s, len, "%s;%lu;%lu;%lu;%lu;%lu;%lu",
                  uuid,
                  filesfree,
                  filestotal,
                  kbytesfree,
                  kbytestotal,
                  read_bytes,
                  write_bytes);
    if (n >= len) {
        errno = E2BIG;
        return -1;
    }
    retval = 0;
done:
    if (uuid)
        free (uuid);
    return retval;
}

int
lmt_ost_string_v2 (pctx_t ctx, char *s, int len)
{
    ListIterator itr = NULL;
    List ostlist = NULL;
    struct utsname uts;
    double cpupct, mempct;
    int used, n, retval = -1;
    char *name;

    if (proc_lustre_ostlist (ctx, &ostlist) < 0)
        goto done;
    if (list_count (ostlist) == 0) {
        errno = 0;
        goto done;
    }
    if (uname (&uts) < 0)
        goto done;
    if (_get_cpu_usage (ctx, &cpupct) < 0)
        goto done;
    if (_get_mem_usage (ctx, &mempct) < 0)
        goto done;
    n = snprintf (s, len, "2;%s;%f;%f",
                  uts.nodename,
                  cpupct,
                  mempct);
    if (n >= len) {
        errno = E2BIG;
        goto done;
    }
    if (!(itr = list_iterator_create (ostlist)))
        goto done;
    while ((name = list_next (itr))) {
        used = strlen (s);
        if (_get_oststring (ctx, name, s + used, len - used) < 0)
            goto done;
    }
    retval = 0;
done:
    if (itr)
        list_iterator_destroy (itr);
    if (ostlist)
        list_destroy (ostlist);
    return retval;
}

int
lmt_ost_decode_v2 (char *s, char **namep, float *pct_cpup, float *pct_memp,
                   List *ostinfop)
{
    int retval = -1;
    char *name, *cpy = NULL;
    float pct_mem, pct_cpu;
    List ostinfo = NULL;

    if (!(name = malloc (strlen(s) + 1))) {
        errno = ENOMEM;
        goto done;
    }
    if (sscanf (s, "%*s;%s;%f;%f;", name, &pct_cpu, &pct_mem) != 3) {
        errno = EIO;
        goto done;
    }
    if (!(s = strskip (s, 4, ';'))) {
        errno = EIO;
        goto done;
    }
    if (!(ostinfo = list_create ((ListDelF)free)))
        goto done;
    while ((cpy = strskipcpy (&s, 7, ';'))) {
        if (!list_append (ostinfo, cpy)) {
            free (cpy);
            goto done;
        }
    }
    if (strlen (s) > 0) {
        errno = EIO;
        goto done;
    }
    *namep = name;
    *pct_cpup = pct_cpu;
    *pct_memp = pct_mem;
    *ostinfop = ostinfo;
    retval = 0;
done:
    if (retval < 0) {
        if (name)
            free (name);
        if (ostinfo)
            list_destroy (ostinfo);
    }
    return retval;
}

int
lmt_ost_decode_v2_ostinfo (char *s, char **namep,
                           uint64_t *read_bytesp, uint64_t *write_bytesp,
                           uint64_t *kbytes_freep, uint64_t *kbytes_totalp,
                           uint64_t *inodes_freep, uint64_t *inodes_totalp)
{
    int retval = -1;
    char *name;
    uint64_t read_bytes, write_bytes;
    uint64_t kbytes_free, kbytes_total;
    uint64_t inodes_free, inodes_total;

    if (!(name = malloc (strlen (s) + 1))) {
        errno = ENOMEM;
        goto done;
    }
    if (sscanf (s, "%s;%lu;%lu;%lu;%lu;%lu;%lu", name, &inodes_free,
                  &inodes_total, &kbytes_free, &kbytes_total, &read_bytes,
                  &write_bytes) != 7) {
        errno = EIO;
        goto done;
    }
    *namep = name;
    *read_bytesp = read_bytes;
    *write_bytesp = write_bytes;
    *kbytes_freep = kbytes_free;
    *kbytes_totalp = kbytes_total;
    *inodes_freep = inodes_free;
    *inodes_totalp = inodes_total;
    retval = 0;
done:
    if (retval < 0) {
        if (name)
            free (name);
    }
    return -1;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */