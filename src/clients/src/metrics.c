#include "ctforge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/statvfs.h>

/* /proc/stat 采样差值计算 CPU 使用率 */
static int read_cpu(uint64_t *idle_all, uint64_t *total) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    char line[256];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    uint64_t user, nice, system, idle, iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 4)
        return -1;
    *idle_all = idle + iowait;
    *total = user + nice + system + idle + iowait + irq + softirq + steal;
    return 0;
}

static double cpu_percent_sample(void) {
    static uint64_t p_idle = 0, p_total = 0;
    uint64_t idle, total;
    if (read_cpu(&idle, &total) < 0) return 0;
    if (p_total == 0 || total <= p_total) { p_idle = idle; p_total = total; return 0; }
    uint64_t dt = total - p_total, di = idle - p_idle;
    p_idle = idle; p_total = total;
    if (dt == 0) return 0;
    double pct = 100.0 * (double)(dt - di) / (double)dt;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

void ctc_collect_metrics(ctc_metrics *m) {
    memset(m, 0, sizeof(*m));

    /* uptime & load */
    FILE *f = fopen("/proc/uptime", "r");
    if (f) { double up = 0; fscanf(f, "%lf", &up); m->uptime_sec = (uint64_t)up; fclose(f); }
    f = fopen("/proc/loadavg", "r");
    if (f) { fscanf(f, "%lf %lf %lf", &m->load1, &m->load5, &m->load15); fclose(f); }

    /* meminfo */
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char key[64]; unsigned long long val; char unit[16];
        uint64_t avail = 0, free_ = 0, buffers = 0, cached = 0, sfree = 0;
        while (fscanf(f, "%63s %llu %15s", key, &val, unit) == 3) {
            if (!strcmp(key, "MemTotal:")) m->mem_total_kb = val;
            else if (!strcmp(key, "MemAvailable:")) avail = val;
            else if (!strcmp(key, "MemFree:")) free_ = val;
            else if (!strcmp(key, "Buffers:")) buffers = val;
            else if (!strcmp(key, "Cached:")) cached = val;
            else if (!strcmp(key, "SwapTotal:")) m->swap_total_kb = val;
            else if (!strcmp(key, "SwapFree:")) sfree = val;
        }
        fclose(f);
        uint64_t used;
        if (avail) used = m->mem_total_kb - avail;
        else used = m->mem_total_kb - free_ - buffers - cached;
        m->mem_used_kb = used;
        if (m->mem_total_kb) m->mem_percent = 100.0 * (double)used / (double)m->mem_total_kb;
        m->swap_used_kb = m->swap_total_kb > sfree ? m->swap_total_kb - sfree : 0;
    }

    /* disk (/) */
    struct statvfs sv;
    if (statvfs("/", &sv) == 0) {
        uint64_t bsize = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
        m->disk_total_kb = (uint64_t)sv.f_blocks * bsize / 1024;
        uint64_t freekb = (uint64_t)sv.f_bavail * bsize / 1024;
        m->disk_used_kb = m->disk_total_kb > freekb ? m->disk_total_kb - freekb : 0;
        if (m->disk_total_kb) m->disk_percent = 100.0 * (double)m->disk_used_kb / (double)m->disk_total_kb;
    }

    /* process count */
    DIR *d = opendir("/proc");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
            if (isdigit((unsigned char)e->d_name[0])) m->procs++;
        closedir(d);
    }

    m->cpu_percent = cpu_percent_sample();
}
