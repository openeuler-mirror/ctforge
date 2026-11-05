/* 硬件信息与磁盘 IO 采集
 * 全部基于 /sys、/proc、dmidecode（可选），最小化外部依赖。
 */
#define _GNU_SOURCE
#include "ctforge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>

static char *hEsc(const char *s) {
    if (!s) return strdup("");
    size_t cap = strlen(s) * 2 + 2, n = 0;
    char *out = malloc(cap);
    if (!out) return strdup("");
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') { out[n++] = '\\'; out[n++] = *p; }
        else if (*p == '\n' || *p == '\t' || *p == '\r') { out[n++] = ' '; }
        else if ((unsigned char)*p < 0x20) { out[n++] = ' '; }
        else out[n++] = *p;
    }
    out[n] = 0;
    return out;
}

/* 读文件首行并去空白 */
static void readFirstLine(const char *path, char *buf, size_t sz) {
    buf[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (!fgets(buf, (int)sz, f)) buf[0] = 0;
    fclose(f);
    size_t n = strlen(buf);
    while (n && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ')) buf[--n] = 0;
}

/* ---- CPU ---- */
static void collectCPU(char *model, size_t msz, int *cores, int *threads, double *mhz) {
    model[0] = 0; *cores = 0; *threads = 0; *mhz = 0;

    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[512];
        int phys_ids[256] = {0}, core_ids_seen = 0;
        int cpu_cores_field = 0, siblings = 0;
        int last_phys = -1, last_core = -1;
        int pairs[512][2]; int npairs = 0;
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "model name", 10) && !model[0]) {
                char *c = strchr(line, ':');
                if (c) { c++; while (*c == ' ') c++; snprintf(model, msz, "%s", c);
                         size_t n = strlen(model);
                         while (n && (model[n-1]=='\n'||model[n-1]=='\r')) model[--n]=0; }
            } else if (!strncmp(line, "cpu cores", 9)) {
                char *c = strchr(line, ':'); if (c) cpu_cores_field = atoi(c+1);
            } else if (!strncmp(line, "siblings", 8)) {
                char *c = strchr(line, ':'); if (c) siblings = atoi(c+1);
            } else if (!strncmp(line, "physical id", 11)) {
                char *c = strchr(line, ':'); if (c) last_phys = atoi(c+1);
            } else if (!strncmp(line, "core id", 7)) {
                char *c = strchr(line, ':'); if (c) last_core = atoi(c+1);
            } else if (!strncmp(line, "cpu MHz", 7)) {
                char *c = strchr(line, ':'); if (c && *mhz == 0) *mhz = atof(c+1);
            } else if (line[0] == '\n') {
                /* 一个处理器块结束 */
                if (last_phys >= 0 && last_core >= 0 && npairs < 512) {
                    pairs[npairs][0] = last_phys; pairs[npairs][1] = last_core; npairs++;
                }
                (*threads)++;
                last_phys = last_core = -1;
            }
        }
        fclose(f);

        /* 去重 (phys, core) 得到物理核数 */
        int uniq = 0;
        for (int i = 0; i < npairs; i++) {
            int dup = 0;
            for (int j = 0; j < i; j++)
                if (pairs[j][0] == pairs[i][0] && pairs[j][1] == pairs[i][1]) { dup = 1; break; }
            if (!dup) uniq++;
        }
        if (uniq > 0) *cores = uniq;
        else if (cpu_cores_field > 0) *cores = cpu_cores_field;
        else *cores = *threads > 0 ? *threads : 1;

        (void)phys_ids; (void)core_ids_seen; (void)siblings;
    }
    if (*threads == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        *threads = n > 0 ? (int)n : 1;
        if (*cores == 0) *cores = *threads;
    }
}

/* ---- 内存 ---- */
static void collectMem(uint64_t *mem_kb, uint64_t *swap_kb) {
    *mem_kb = 0; *swap_kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char key[64]; unsigned long long v; char unit[16];
    while (fscanf(f, "%63s %llu %15s", key, &v, unit) == 3) {
        if (!strcmp(key, "MemTotal:")) *mem_kb = v;
        else if (!strcmp(key, "SwapTotal:")) *swap_kb = v;
    }
    fclose(f);
}

/* ---- 磁盘 ---- */
static void collectDisks(char *out, size_t outsz) {
    size_t n = 0;
    n += snprintf(out + n, outsz - n, "[");

    /* 1) lsblk 获取物理盘大小型号 */
    typedef struct { char name[64]; char model[128]; double size_gb; int rot; } DiskRec;
    DiskRec disks[64]; int nd = 0;

    FILE *f = popen("lsblk -dbno NAME,SIZE,MODEL,ROTA 2>/dev/null | head -40", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f) && nd < 64) {
            char name[64] = {0}, model[128] = {0};
            unsigned long long bytes = 0; int rota = 0;
            /* 格式：sda  512110190592  Samsung SSD 860 1
             * MODEL 可能含空格，故从尾部解析 rota 和 size */
            char *tok = strtok(line, " \n");
            if (!tok) continue;
            snprintf(name, sizeof(name), "%s", tok);
            tok = strtok(NULL, " \n");
            if (!tok) continue;
            bytes = strtoull(tok, NULL, 10);
            /* 剩余部分是 model + rota（最后一段是 rota） */
            char rest[256] = {0};
            char *p = tok + strlen(tok) + 1;
            snprintf(rest, sizeof(rest), "%s", p);
            size_t rl = strlen(rest);
            while (rl && (rest[rl-1]=='\n'||rest[rl-1]==' ')) rest[--rl]=0;
            char *lastsp = strrchr(rest, ' ');
            if (lastsp) {
                rota = atoi(lastsp + 1);
                *lastsp = 0;
            }
            snprintf(model, sizeof(model), "%s", rest);
            /* 跳过 loop/ram/sr/zram 设备 */
            if (!strncmp(name, "loop", 4) || !strncmp(name, "ram", 3) || !strncmp(name, "sr", 2) ||
                !strncmp(name, "zram", 4) || !strncmp(name, "dm-", 3))
                continue;
            snprintf(disks[nd].name, sizeof(disks[nd].name), "%s", name);
            snprintf(disks[nd].model, sizeof(disks[nd].model), "%s", model);
            disks[nd].size_gb = (double)bytes / 1024.0 / 1024.0 / 1024.0;
            disks[nd].rot = rota;
            nd++;
        }
        pclose(f);
    }

    /* 2) 挂载点映射 / 文件系统 */
    typedef struct { char dev[64]; char mount[128]; char fstype[32]; } MountRec;
    MountRec mounts[128]; int nm = 0;
    FILE *mf = fopen("/proc/mounts", "r");
    if (mf) {
        char dev[128], mnt[256], fst[64];
        while (fscanf(mf, "%127s %255s %63s %*[^\n]\n", dev, mnt, fst) == 3 && nm < 128) {
            if (strncmp(dev, "/dev/", 5) != 0) continue;
            snprintf(mounts[nm].dev, sizeof(mounts[nm].dev), "%s", dev);
            snprintf(mounts[nm].mount, sizeof(mounts[nm].mount), "%s", mnt);
            snprintf(mounts[nm].fstype, sizeof(mounts[nm].fstype), "%s", fst);
            nm++;
        }
        fclose(mf);
    }

    for (int i = 0; i < nd; i++) {
        char *e_name = hEsc(disks[i].name);
        char *e_model = hEsc(disks[i].model);
        const char *type = disks[i].rot ? "hdd" : "ssd";
        if (n + 512 > outsz) break;
        if (i > 0) n += snprintf(out + n, outsz - n, ",");
        n += snprintf(out + n, outsz - n,
            "{\"name\":\"%s\",\"model\":\"%s\",\"size_gb\":%.1f,\"type\":\"%s\"",
            e_name, e_model, disks[i].size_gb, type);

        /* 找该盘的分区挂载点（/dev/sdaN 或 /dev/nvme0n1pN） */
        for (int j = 0; j < nm; j++) {
            if (strncmp(mounts[j].dev, "/dev/", 5) != 0) continue;
            const char *base = mounts[j].dev + 5;
            if (strncmp(base, disks[i].name, strlen(disks[i].name)) != 0) continue;
            /* 前缀匹配且下一个字符是数字或 p（分区） */
            char next = base[strlen(disks[i].name)];
            if (next == 0 || isdigit((unsigned char)next) || next == 'p') {
                char *e_m = hEsc(mounts[j].mount);
                char *e_f = hEsc(mounts[j].fstype);
                n += snprintf(out + n, outsz - n,
                    ",\"mount_on\":\"%s\",\"fs_type\":\"%s\"", e_m, e_f);
                free(e_m); free(e_f);
                break;
            }
        }
        n += snprintf(out + n, outsz - n, "}");
        free(e_name); free(e_model);
    }
    snprintf(out + n, outsz - n, "]");
}

/* ---- 网卡 ---- */
static void collectNetCards(char *out, size_t outsz) {
    size_t n = 0;
    n += snprintf(out + n, outsz - n, "[");
    DIR *d = opendir("/sys/class/net");
    if (!d) { snprintf(out + n, outsz - n, "]"); return; }
    struct dirent *e;
    int first = 1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!strcmp(e->d_name, "lo")) continue;
        if (!strncmp(e->d_name, "docker", 6) || !strncmp(e->d_name, "veth", 4) ||
            !strncmp(e->d_name, "br-", 3)) continue;

        char path[256], mac[64] = "", speed[32] = "", driver[128] = "";
        char link[512] = "";
        snprintf(path, sizeof(path), "/sys/class/net/%s/address", e->d_name);
        readFirstLine(path, mac, sizeof(mac));
        snprintf(path, sizeof(path), "/sys/class/net/%s/speed", e->d_name);
        readFirstLine(path, speed, sizeof(speed));
        if (speed[0] == 0 || atoi(speed) <= 0) snprintf(speed, sizeof(speed), "-");
        /* driver 是符号链接 */
        snprintf(path, sizeof(path), "/sys/class/net/%s/device/driver", e->d_name);
        ssize_t ln = readlink(path, link, sizeof(link) - 1);
        if (ln > 0) {
            link[ln] = 0;
            char *slash = strrchr(link, '/');
            snprintf(driver, sizeof(driver), "%s", slash ? slash + 1 : link);
        }

        char *e_n = hEsc(e->d_name), *e_mac = hEsc(mac), *e_sp = hEsc(speed), *e_dr = hEsc(driver);
        if (!first) n += snprintf(out + n, outsz - n, ",");
        n += snprintf(out + n, outsz - n,
            "{\"name\":\"%s\",\"mac\":\"%s\",\"speed\":\"%s\",\"driver\":\"%s\"}",
            e_n, e_mac, e_sp, e_dr);
        free(e_n); free(e_mac); free(e_sp); free(e_dr);
        first = 0;
    }
    closedir(d);
    snprintf(out + n, outsz - n, "]");
}

/* ---- 主板 / BIOS / 虚拟化 ---- */
static void collectSysInfo(char *bios, size_t bsz, char *board, size_t brsz, char *virt, size_t vsz) {
    bios[0] = 0; board[0] = 0; virt[0] = 0;
    readFirstLine("/sys/class/dmi/id/bios_vendor", bios, bsz);
    readFirstLine("/sys/class/dmi/id/board_name", board, brsz);

    /* 虚拟化检测 */
    if (access("/.dockerenv", F_OK) == 0) snprintf(virt, vsz, "docker");
    else if (access("/run/.containerenv", F_OK) == 0) snprintf(virt, vsz, "podman");
    else {
        FILE *f = fopen("/proc/cpuinfo", "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, " hypervisor") || strstr(line, "hypervisor ")) {
                    snprintf(virt, vsz, "hypervisor");
                    break;
                }
            }
            fclose(f);
        }
        if (!virt[0]) snprintf(virt, vsz, "bare-metal");
    }
}

char *ctc_hardware_json(void) {
    char model[256] = "", bios[128] = "", board[128] = "", virt[64] = "", machine_id[128] = "";
    int cores = 0, threads = 0;
    double mhz = 0;
    uint64_t mem = 0, swap = 0;

    collectCPU(model, sizeof(model), &cores, &threads, &mhz);
    collectMem(&mem, &swap);
    collectSysInfo(bios, sizeof(bios), board, sizeof(board), virt, sizeof(virt));
    readFirstLine("/etc/machine-id", machine_id, sizeof(machine_id));

    char disks[8192];
    collectDisks(disks, sizeof(disks));
    char nics[4096];
    collectNetCards(nics, sizeof(nics));

    char *e_model = hEsc(model), *e_bios = hEsc(bios), *e_board = hEsc(board);
    char *e_virt = hEsc(virt), *e_mid = hEsc(machine_id);

    size_t cap = strlen(disks) + strlen(nics) + 2048;
    char *out = malloc(cap);
    snprintf(out, cap,
        "{\"cpu_model\":\"%s\",\"cpu_cores\":%d,\"cpu_threads\":%d,\"cpu_mhz\":%.0f,"
        "\"mem_total_kb\":%llu,\"swap_total_kb\":%llu,"
        "\"disks\":%s,\"net_cards\":%s,"
        "\"virtualization\":\"%s\",\"bios_vendor\":\"%s\",\"board_name\":\"%s\",\"machine_id\":\"%s\"}",
        e_model, cores, threads, mhz, (unsigned long long)mem, (unsigned long long)swap,
        disks, nics, e_virt, e_bios, e_board, e_mid);

    free(e_model); free(e_bios); free(e_board); free(e_virt); free(e_mid);
    return out;
}

/* ---- 磁盘 IO（基于 /proc/diskstats 两次采样差值，与上次调用间隔）---- */
char *ctc_disk_io_json(void) {
    typedef struct { char name[32]; unsigned long long reads, rmerge, rsect, rtime;
                     unsigned long long writes, wmerge, wsect, wtime, iotime; } DiskStat;
    static DiskStat prev[64];
    static int nprev = 0;
    static time_t prev_time = 0;

    DiskStat cur[64]; int ncur = 0;
    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) return strdup("{\"devices\":[]}");
    char line[512];
    while (fgets(line, sizeof(line), f) && ncur < 64) {
        char name[32] = {0};
        unsigned long long vals[20] = {0};
        int n = sscanf(line,
            " %*d %*d %31s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
            name, &vals[0], &vals[1], &vals[2], &vals[3],
            &vals[4], &vals[5], &vals[6], &vals[7], &vals[8], &vals[9]);
        if (n < 11) continue;
        /* 只看物理盘：sda / vda / xvda / nvme0n1（不含分区） */
        int is_partition = 0;
        size_t ln = strlen(name);
        if (ln > 1 && name[ln-1] >= '0' && name[ln-1] <= '9') {
            if (!strncmp(name, "nvme", 4)) {
                /* nvme0n1 是盘，nvme0n1p1 是分区 */
                if (strchr(name, 'p')) is_partition = 1;
            } else if (!strncmp(name, "mmcblk", 6)) {
                if (strchr(name, 'p')) is_partition = 1;
            } else {
                is_partition = 1; /* sda1, vda1 等 */
            }
        }
        if (is_partition) continue;
        if (!strncmp(name, "loop", 4) || !strncmp(name, "ram", 3) || !strncmp(name, "dm-", 3))
            continue;

        snprintf(cur[ncur].name, sizeof(cur[ncur].name), "%s", name);
        cur[ncur].reads  = vals[0];
        cur[ncur].rsect  = vals[2];
        cur[ncur].writes = vals[4];
        cur[ncur].wsect  = vals[6];
        cur[ncur].iotime = vals[9];
        ncur++;
    }
    fclose(f);

    time_t now = time(NULL);
    double dt = (prev_time > 0) ? (double)(now - prev_time) : 0;

    size_t cap = 4096;
    char *out = malloc(cap);
    size_t n = 0;
    struct timespec tsp;
    clock_gettime(CLOCK_REALTIME, &tsp);
    n += snprintf(out + n, cap - n, "{\"timestamp\":%lld,\"devices\":[",
                  (long long)tsp.tv_sec * 1000 + tsp.tv_nsec / 1000000);

    int first = 1;
    for (int i = 0; i < ncur; i++) {
        double read_kbps = 0, write_kbps = 0, read_iops = 0, write_iops = 0, util = 0;
        if (dt > 0.5) {
            for (int j = 0; j < nprev; j++) {
                if (strcmp(prev[j].name, cur[i].name) != 0) continue;
                double dr = (double)(cur[i].rsect - prev[j].rsect);
                double dw = (double)(cur[i].wsect - prev[j].wsect);
                double dri = (double)(cur[i].reads - prev[j].reads);
                double dwi = (double)(cur[i].writes - prev[j].writes);
                double dit = (double)(cur[i].iotime - prev[j].iotime);
                if (dr < 0) dr = 0;
                if (dw < 0) dw = 0;
                read_kbps  = dr * 512.0 / 1024.0 / dt;
                write_kbps = dw * 512.0 / 1024.0 / dt;
                read_iops  = dri / dt;
                write_iops = dwi / dt;
                util = dit / (dt * 1000.0) * 100.0;
                if (util > 100) util = 100;
                if (util < 0) util = 0;
                break;
            }
        }
        char *e_n = hEsc(cur[i].name);
        if (!first) n += snprintf(out + n, cap - n, ",");
        n += snprintf(out + n, cap - n,
            "{\"name\":\"%s\",\"read_kbps\":%.1f,\"write_kbps\":%.1f,"
            "\"read_iops\":%.1f,\"write_iops\":%.1f,\"util_pct\":%.1f}",
            e_n, read_kbps, write_kbps, read_iops, write_iops, util);
        free(e_n);
        first = 0;
    }
    snprintf(out + n, cap - n, "]}");

    /* 保存本次采样 */
    memcpy(prev, cur, sizeof(DiskStat) * (size_t)ncur);
    nprev = ncur;
    prev_time = now;
    return out;
}
