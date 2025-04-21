/* Copyright (C) 2014 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 *
 */

#include "suricata-common.h"
#include "detect.h"
#include "pkt-var.h"
#include "conf.h"

#include "threads.h"
#include "threadvars.h"
#include "tm-threads.h"

#include "util-print.h"
#include "util-unittest.h"

#include "util-debug.h"

#include "output.h"
#include "log-stats.h"
#include "util-privs.h"
#include "util-buffer.h"

#include "util-logopenfile.h"
#include "util-time.h"

#include "util-device.h"

#include <rte_config.h>
#include <rte_common.h>
#include <rte_vect.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_hash.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_string_fns.h>

#include <rte_alarm.h>
#include <rte_timer.h>
#include <rte_jobstats.h>
#include <rte_version.h>

#define DEFAULT_LOG_FILENAME "stats.log"
#define MODULE_NAME "LogStatsLog"
#define OUTPUT_BUFFER_SIZE 16384

#define LOG_STATS_TOTALS  (1<<0)
#define LOG_STATS_THREADS (1<<1)
#define LOG_STATS_NULLS   (1<<2)

TmEcode LogStatsLogThreadInit(ThreadVars *, const void *, void **);
TmEcode LogStatsLogThreadDeinit(ThreadVars *, void *);
static void LogStatsLogDeInitCtx(OutputCtx *);

typedef struct LogStatsFileCtx_ {
    LogFileCtx *file_ctx;
    uint32_t flags; /** Store mode */
} LogStatsFileCtx;

typedef struct LogStatsLogThread_ {
    LogStatsFileCtx *statslog_ctx;
    MemBuffer *buffer;
} LogStatsLogThread;

#define DPDK_STATS_BUF_LEN 102400
typedef struct DpdkStatsDesc_
{
    LogFileCtx *file_cts;
    int logtype;
    unsigned buf_len;
    char buf[DPDK_STATS_BUF_LEN];
} DpdkStatsDesc;
DpdkStatsDesc DpdkStatsLog;

#define DPDK_STATS_LOG_WRITE(fmt, args...) do {\
    DpdkStatsLog.buf_len += snprintf(DpdkStatsLog.buf+DpdkStatsLog.buf_len,\
    (DPDK_STATS_BUF_LEN-DpdkStatsLog.buf_len),fmt , ##args);\
}while(0)

struct rte_eth_stats port_stats[RTE_MAX_ETHPORTS];
extern uint32_t stats_tts = 8;

static inline const char *output_norm(char *buf, uint64_t val)
{
    char *units[] = { "", "K", "M", "G", "T" };
    uint32_t i;
    uint64_t r_val = 0;

    for(i = 0; val >=1000 && i < sizeof(units)/sizeof(char *) - 1; i++)
    {
        r_val = val % 1000;
        val /= 1000;
    }

    sprintf(buf, "%lu.%03lu %s", val, r_val, units[i]);
    return buf;
}

static void nic_stats_display(unsigned port_id, struct rte_eth_stats *stats)
{
    uint8_t i;

    static const char *nic_stats_border = "########################";

    rte_eth_stats_get(port_id, stats);
    DPDK_STATS_LOG_WRITE("\n  %s NIC statistics for port %-2d %s\n",
                         nic_stats_border, port_id, nic_stats_border);

    DPDK_STATS_LOG_WRITE("  RX-packets: %'-18"PRIu64" RX-missed:    %'-10"PRIu64" RX-bytes:  "
                         "%'-"PRIu64"\n",
                         stats->ipackets, stats->imissed, stats->ibytes);
    DPDK_STATS_LOG_WRITE("  RX-pps:     %'-18"PRIu64" Missed-pps:   %'-10"PRIu64" RX-bps:    "
                         "%'-"PRIu64"\n",
                         (stats->ipackets - port_stats[port_id].ipackets)/stats_tts,
                         (stats->imissed - port_stats[port_id].imissed)/stats_tts,
                         (stats->ibytes -  port_stats[port_id].ibytes)*8/stats_tts);
    DPDK_STATS_LOG_WRITE("  RX-errors:  %'-18"PRIu64"                         "" RX-nombuf: "
                         "%'-"PRIu64"\n",
                         stats->ierrors, stats->rx_nombuf);
    DPDK_STATS_LOG_WRITE("  Avg-size:   %'-18"PRIu64"                         "" Mis-ratio: "
                         "%'-10.8f\n",
                         ((stats->ipackets) ? (stats->ibytes/stats->ipackets) : 0),
                         ((stats->ipackets)?(float)stats->imissed/
                          (float)(stats->ipackets+stats->imissed):1));

    DPDK_STATS_LOG_WRITE("\n");
    for(i = 0; i < RTE_ETHDEV_QUEUE_STAT_CNTRS; i++)
    {
        if(!stats->q_ipackets[i])
            continue;
        DPDK_STATS_LOG_WRITE("  Stats reg %2d RX-packets: %'10"PRIu64
                             "    RX-errors: %'10"PRIu64
                             "    RX-bytes: %'10"PRIu64"\n",
                             i, stats->q_ipackets[i], stats->q_errors[i], stats->q_ibytes[i]);
    }

    DPDK_STATS_LOG_WRITE("  %s############################%s\n",
                         nic_stats_border, nic_stats_border);

}
	

/* Print out statistics on packets dropped */
static void show_stats_cb(LogFileCtx *file_ctx)
{
    struct rte_eth_stats stats, total_stats, total_stats_last;
    unsigned portid = 0/*, lcore_id*/;
    struct timeval tval;
    struct tm *tms;
    char b1[32], b2[32], b3[32], b4[32];

    gettimeofday(&tval, NULL);
    struct tm local_tm;
    tms = SCLocalTime(tval.tv_sec, &local_tm);

    const char clr[] = { 27, '[', '2', 'J', '\0' };
    const char topLeft[] = { 27, '[', '1', ';', '1', 'H', '\0' };

    memset(&stats, 0x0, sizeof(struct rte_eth_stats));
    memset(&total_stats, 0x0, sizeof(struct rte_eth_stats));
    memset(&total_stats_last, 0x0, sizeof(struct rte_eth_stats));

    memset(&DpdkStatsLog.buf, 0x0, DPDK_STATS_BUF_LEN);
    DpdkStatsLog.buf_len = 0;

    DPDK_STATS_LOG_WRITE("%s%s\n"                        
                         "  =================================================================\n"
                         "  RTE Version: %s\n"
                         "  ========================= Port statistics =======================\n",
                         clr, topLeft, rte_version());

	int portnum = LiveGetDeviceCount();

    for(portid = 0; portid < portnum; portid++)
    {
        total_stats_last.ipackets += port_stats[portid].ipackets;
        total_stats_last.ibytes += port_stats[portid].ibytes;
        total_stats_last.imissed += port_stats[portid].imissed;
        total_stats_last.ierrors += port_stats[portid].ierrors;
        total_stats_last.rx_nombuf += port_stats[portid].rx_nombuf;

        nic_stats_display(portid, &stats);

        rte_memcpy(&port_stats[portid], &stats, sizeof(stats));

        total_stats.ipackets += stats.ipackets;
        total_stats.ibytes += stats.ibytes;
        total_stats.imissed += stats.imissed;
        total_stats.ierrors += stats.ierrors;
        total_stats.rx_nombuf += stats.rx_nombuf;
    }
    uint64_t rx_pps, rx_bps, mis_pps;
    float mis_ratio;

    rx_pps = (total_stats.ipackets - total_stats_last.ipackets)/stats_tts;
    rx_bps = (total_stats.ibytes - total_stats_last.ibytes)*8/stats_tts;
    mis_pps = (total_stats.imissed - total_stats_last.imissed)/stats_tts;
    mis_ratio = ((total_stats.ipackets - total_stats_last.ipackets)>0)?
                ((float)(total_stats.imissed - total_stats_last.imissed)/ \
                 (float)(total_stats.ipackets - total_stats_last.ipackets)): \
                (float)(total_stats.imissed - total_stats_last.imissed);

    DPDK_STATS_LOG_WRITE("\n Aggregate statistics ======================================="
                         "\n Total rx packets: %'18"PRIu64" %'15"PRIu64" pps (%spps)"
                         "\n Total rx bytes:   %'18"PRIu64" %'15"PRIu64" bps (%sbps, RAW %sbps)"
                         "\n Total rx missed:  %'18"PRIu64" %'15"PRIu64" pps (%spps %0.5f)"
                         "\n Total rx errors:  %'18"PRIu64
                         "\n Total rx nombuf:  %'18"PRIu64
                         "\n Total avg size:   %'18"PRIu64
                         "\n Total mis ratio:	 %'10.10f"
                         "\n ============================================================\n",
                         total_stats.ipackets,
                         rx_pps,output_norm(b1, rx_pps),
                         total_stats.ibytes,
                         rx_bps,output_norm(b2, rx_bps),
                         output_norm(b3, (rx_bps+(rx_pps*24*8))),
                         total_stats.imissed,
                         mis_pps,output_norm(b4, mis_pps),
                         mis_ratio,
                         total_stats.ierrors,total_stats.rx_nombuf,
                         ((total_stats.ipackets) ? (total_stats.ibytes/total_stats.ipackets) : 0),
                         ((total_stats.ipackets)?((float)total_stats.imissed/
                                 (float)(total_stats.ipackets+total_stats.imissed)):(float)1));


        file_ctx->Write(DpdkStatsLog.buf, DpdkStatsLog.buf_len, file_ctx);
		
		return;
}


static int LogStatsLogger(ThreadVars *tv, void *thread_data, const StatsTable *st)
{
    SCEnter();
    LogStatsLogThread *aft = (LogStatsLogThread *)thread_data;

    struct timeval tval;
    struct tm *tms;

    gettimeofday(&tval, NULL);
    struct tm local_tm;
    tms = SCLocalTime(tval.tv_sec, &local_tm);

    /* Calculate the Engine uptime */
    double up_time_d = difftime(tval.tv_sec, st->start_time);
    int up_time = (int)up_time_d; // ignoring risk of overflow here
    int sec = up_time % 60;     // Seconds in a minute
    int in_min = up_time / 60;
    int min = in_min % 60;      // Minutes in a hour
    int in_hours = in_min / 60;
    int hours = in_hours % 24;  // Hours in a day
    int days = in_hours / 24;

    MemBufferWriteString(aft->buffer, "----------------------------------------------"
            "--------------------------------------\n");
    MemBufferWriteString(aft->buffer, "Date: %" PRId32 "/%" PRId32 "/%04d -- "
            "%02d:%02d:%02d (uptime: %"PRId32"d, %02dh %02dm %02ds)\n",
            tms->tm_mon + 1, tms->tm_mday, tms->tm_year + 1900, tms->tm_hour,
            tms->tm_min, tms->tm_sec, days, hours, min, sec);
    MemBufferWriteString(aft->buffer, "----------------------------------------------"
            "--------------------------------------\n");
    MemBufferWriteString(aft->buffer, "%-45s | %-25s | %-s\n", "Counter", "TM Name",
            "Value");
    MemBufferWriteString(aft->buffer, "----------------------------------------------"
            "--------------------------------------\n");

    /* global stats */
    uint32_t u = 0;
    if (aft->statslog_ctx->flags & LOG_STATS_TOTALS) {
        for (u = 0; u < st->nstats; u++) {
            if (st->stats[u].name == NULL)
                continue;

            if (!(aft->statslog_ctx->flags & LOG_STATS_NULLS) && st->stats[u].value == 0)
                continue;

            char line[256];
            size_t len = snprintf(line, sizeof(line), "%-45s | %-25s | %-" PRIu64 "\n",
                    st->stats[u].name, st->stats[u].tm_name, st->stats[u].value);

            /* since we can have many threads, the buffer might not be big enough.
             * Expand if necessary. */
            if (MEMBUFFER_OFFSET(aft->buffer) + len >= MEMBUFFER_SIZE(aft->buffer)) {
                MemBufferExpand(&aft->buffer, OUTPUT_BUFFER_SIZE);
            }

            MemBufferWriteString(aft->buffer, "%s", line);
        }
    }

    /* per thread stats */
    if (st->tstats != NULL && aft->statslog_ctx->flags & LOG_STATS_THREADS) {
        /* for each thread (store) */
        uint32_t x;
        for (x = 0; x < st->ntstats; x++) {
            uint32_t offset = x * st->nstats;

            /* for each counter */
            for (u = offset; u < (offset + st->nstats); u++) {
                if (st->tstats[u].name == NULL)
                    continue;

                if (!(aft->statslog_ctx->flags & LOG_STATS_NULLS) && st->tstats[u].value == 0)
                    continue;

                char line[256];
                size_t len = snprintf(line, sizeof(line), "%-45s | %-25s | %-" PRIi64 "\n",
                        st->tstats[u].name, st->tstats[u].tm_name, st->tstats[u].value);

                /* since we can have many threads, the buffer might not be big enough.
                 * Expand if necessary. */
                if (MEMBUFFER_OFFSET(aft->buffer) + len >= MEMBUFFER_SIZE(aft->buffer)) {
                    MemBufferExpand(&aft->buffer, OUTPUT_BUFFER_SIZE);
                }

                MemBufferWriteString(aft->buffer, "%s", line);
            }
        }
    }

    aft->statslog_ctx->file_ctx->Write((const char *)MEMBUFFER_BUFFER(aft->buffer),
        MEMBUFFER_OFFSET(aft->buffer), aft->statslog_ctx->file_ctx);

    MemBufferReset(aft->buffer);

    show_stats_cb(aft->statslog_ctx->file_ctx);

    SCReturnInt(0);
}

TmEcode LogStatsLogThreadInit(ThreadVars *t, const void *initdata, void **data)
{
    LogStatsLogThread *aft = SCMalloc(sizeof(LogStatsLogThread));
    if (unlikely(aft == NULL))
        return TM_ECODE_FAILED;
    memset(aft, 0, sizeof(LogStatsLogThread));

    if(initdata == NULL)
    {
        SCLogDebug("Error getting context for LogStats.  \"initdata\" argument NULL");
        SCFree(aft);
        return TM_ECODE_FAILED;
    }

    aft->buffer = MemBufferCreateNew(OUTPUT_BUFFER_SIZE);
    if (aft->buffer == NULL) {
        SCFree(aft);
        return TM_ECODE_FAILED;
    }

    /* Use the Output Context (file pointer and mutex) */
    aft->statslog_ctx= ((OutputCtx *)initdata)->data;

    *data = (void *)aft;
    return TM_ECODE_OK;
}

TmEcode LogStatsLogThreadDeinit(ThreadVars *t, void *data)
{
    LogStatsLogThread *aft = (LogStatsLogThread *)data;
    if (aft == NULL) {
        return TM_ECODE_OK;
    }

    MemBufferFree(aft->buffer);
    /* clear memory */
    memset(aft, 0, sizeof(LogStatsLogThread));

    SCFree(aft);
    return TM_ECODE_OK;
}

/** \brief Create a new http log LogFileCtx.
 *  \param conf Pointer to ConfNode containing this loggers configuration.
 *  \return NULL if failure, LogFileCtx* to the file_ctx if succesful
 * */
static OutputInitResult LogStatsLogInitCtx(ConfNode *conf)
{
    OutputInitResult result = { NULL, false };
    LogFileCtx *file_ctx = LogFileNewCtx();
    if (file_ctx == NULL) {
        SCLogError("couldn't create new file_ctx");
        return result;
    }

    if (SCConfLogOpenGeneric(conf, file_ctx, DEFAULT_LOG_FILENAME, 1) < 0) {
        LogFileFreeCtx(file_ctx);
        return result;
    }

    LogStatsFileCtx *statslog_ctx = SCMalloc(sizeof(LogStatsFileCtx));
    if (unlikely(statslog_ctx == NULL)) {
        LogFileFreeCtx(file_ctx);
        return result;
    }
    memset(statslog_ctx, 0x00, sizeof(LogStatsFileCtx));

    statslog_ctx->flags = LOG_STATS_TOTALS;

    if (conf != NULL) {
        const char *totals = ConfNodeLookupChildValue(conf, "totals");
        const char *threads = ConfNodeLookupChildValue(conf, "threads");
        const char *nulls = ConfNodeLookupChildValue(conf, "null-values");
        SCLogDebug("totals %s threads %s", totals, threads);

        if ((totals != NULL && ConfValIsFalse(totals)) &&
                (threads != NULL && ConfValIsFalse(threads))) {
            LogFileFreeCtx(file_ctx);
            SCFree(statslog_ctx);
            SCLogError("Cannot disable both totals and threads in stats logging");
            return result;
        }

        if (totals != NULL && ConfValIsFalse(totals)) {
            statslog_ctx->flags &= ~LOG_STATS_TOTALS;
        }
        if (threads != NULL && ConfValIsTrue(threads)) {
            statslog_ctx->flags |= LOG_STATS_THREADS;
        }
        if (nulls != NULL && ConfValIsTrue(nulls)) {
            statslog_ctx->flags |= LOG_STATS_NULLS;
        }
        SCLogDebug("statslog_ctx->flags %08x", statslog_ctx->flags);
    }

    statslog_ctx->file_ctx = file_ctx;

    OutputCtx *output_ctx = SCCalloc(1, sizeof(OutputCtx));
    if (unlikely(output_ctx == NULL)) {
        LogFileFreeCtx(file_ctx);
        SCFree(statslog_ctx);
        return result;
    }

    output_ctx->data = statslog_ctx;
    output_ctx->DeInit = LogStatsLogDeInitCtx;

    SCLogDebug("STATS log output initialized");

    result.ctx = output_ctx;
    result.ok = true;
    return result;
}

static void LogStatsLogDeInitCtx(OutputCtx *output_ctx)
{
    LogStatsFileCtx *statslog_ctx = (LogStatsFileCtx *)output_ctx->data;
    LogFileFreeCtx(statslog_ctx->file_ctx);
    SCFree(statslog_ctx);
    SCFree(output_ctx);
}

void LogStatsLogRegister (void)
{
    OutputRegisterStatsModule(LOGGER_STATS, MODULE_NAME, "stats",
        LogStatsLogInitCtx, LogStatsLogger, LogStatsLogThreadInit,
        LogStatsLogThreadDeinit, NULL);
}
