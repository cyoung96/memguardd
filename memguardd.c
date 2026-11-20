/* memguardd — 膨胀进程守卫(仅单 PID,绝不级联)
 *
 * 循环:sleep(sweep) 定时扫 + PSI avg10 预检。本内核(6.8)实测 PSI fd 的
 * POLLIN 恒就绪(与边沿触发语义不符),不能当事件阻塞原语;触发器重武装在
 * 窗口内还返回 EBUSY。故 PSI 只作压力信号:some avg10>10% 时击杀基线收紧
 * 到 base/4,闲期用全量 base。
 *
 * 击杀规则:
 *   1) RSS ≥ hard            → kill(pid, SIGKILL)
 *   2) RSS ≥ base 且连续 streak 个间隔增长 ≥ growth → kill
 *   3) MemAvailable < emerg  → kill 最大单个非保护进程(5s 冷却)
 * 保护:自身、pid 1、祖先链、-p 追加。只用 kill(pid),从不 killpg。
 *
 * 构建:cc -O2 -Wall -o memguardd memguardd.c
 * 运行:./memguardd -i 60 -b 8192 -g 1500 -s 2 -H 22528 -e 1024 -l <log>
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/sysinfo.h>

#define MAXTRACK 8192
#define NAMELEN 64

struct rec { pid_t pid; long last_rss_mib; int streak; char comm[NAMELEN]; };

static struct rec tr[MAXTRACK];
static int ntr;
static const char *logfile = "/data/tools/memguardd/memguardd.log";
static FILE *lf;
static pid_t protect_extra[32];
static int nprotect;
static long page_kb;

static void logline(const char *fmt, ...) {
    va_list ap; time_t t = time(NULL); char ts[32];
    struct tm tm; localtime_r(&t, &tm); strftime(ts, sizeof ts, "%F %T", &tm);
    va_start(ap, fmt);
    fprintf(lf, "%s ", ts); vfprintf(lf, fmt, ap); fputc('\n', lf);
    fflush(lf);
}

static int is_protected(pid_t pid, pid_t self) {
    if (pid <= 1 || pid == self) return 1;
    char p[64]; FILE *f; char comm[NAMELEN] = "";
    snprintf(p, sizeof p, "/proc/%d/comm", pid);
    if ((f = fopen(p, "r")) && fgets(comm, sizeof comm, f)) fclose(f);
    if (strstr(comm, "memguardd")) return 1;
    for (int i = 0; i < nprotect; i++) if (protect_extra[i] == pid) return 1;
    pid_t cur = self;
    for (int hop = 0; hop < 16; hop++) {
        snprintf(p, sizeof p, "/proc/%d/status", cur);
        if (!(f = fopen(p, "r"))) break;
        char line[256]; pid_t ppid = 0;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "PPid:%d", &ppid) == 1) break;
        fclose(f);
        if (ppid <= 0) break;
        if (ppid == pid) return 1;
        cur = ppid;
    }
    return 0;
}

static struct rec *find(pid_t pid) {
    for (int i = 0; i < ntr; i++) if (tr[i].pid == pid) return &tr[i];
    return NULL;
}

static void scan_once(pid_t self, long base_mib, long growth_mib, int streak_need, long hard_mib, long emerg_mib, time_t *last_emerg) {
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *de;
    pid_t worst_pid = 0; long worst_rss = 0;
    while ((de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        pid_t pid = atoi(de->d_name);
        char p[64]; FILE *f;
        snprintf(p, sizeof p, "/proc/%d/statm", pid);
        if (!(f = fopen(p, "r"))) continue;
        long pages = 0;
        if (fscanf(f, "%*d %ld", &pages) != 1) { fclose(f); continue; }
        fclose(f);
        if (pages <= 0) continue;
        long rss_mib = pages * page_kb / 1024;
        if (is_protected(pid, self)) continue;
        if (rss_mib > worst_rss) { worst_rss = rss_mib; worst_pid = pid; }

        char comm[NAMELEN] = "";
        snprintf(p, sizeof p, "/proc/%d/comm", pid);
        if ((f = fopen(p, "r")) && fgets(comm, sizeof comm, f)) {
            comm[strcspn(comm, "\n")] = 0; fclose(f);
        } else if (f) fclose(f);

        struct rec *r = find(pid);
        if (!r && ntr < MAXTRACK) {
            r = &tr[ntr++]; r->pid = pid; r->last_rss_mib = rss_mib; r->streak = 0;
            snprintf(r->comm, NAMELEN, "%s", comm); continue;
        }
        if (!r) continue;
        long delta = rss_mib - r->last_rss_mib;
        r->streak = (delta >= growth_mib) ? r->streak + 1 : 0;
        snprintf(r->comm, NAMELEN, "%s", comm);
        r->last_rss_mib = rss_mib;

        if (rss_mib >= hard_mib) {
            logline("HARD-KILL pid=%d comm=%s rss=%ldMiB (>= %ldMiB)", pid, r->comm, rss_mib, hard_mib);
            kill(pid, SIGKILL);
            r->streak = 0;
        } else if (rss_mib >= base_mib && r->streak >= streak_need) {
            logline("GROWTH-KILL pid=%d comm=%s rss=%ldMiB streak=%d", pid, r->comm, rss_mib, r->streak);
            kill(pid, SIGKILL);
            r->streak = 0;
        }
    }
    closedir(d);
    for (int i = 0; i < ntr; ) {
        char p[64]; snprintf(p, sizeof p, "/proc/%d/statm", tr[i].pid);
        if (access(p, F_OK) != 0) { tr[i] = tr[--ntr]; } else i++;
    }
    struct sysinfo si;
    if (!sysinfo(&si)) {
        long avail_mib = ((long)si.freeram + (long)si.bufferram) * si.mem_unit / 1048576;
        if (avail_mib < emerg_mib && worst_pid && time(NULL) - *last_emerg >= 5) {
            logline("EMERGENCY-KILL pid=%d rss=%ldMiB avail=%ldMiB<%ldMiB", worst_pid, worst_rss, avail_mib, emerg_mib);
            kill(worst_pid, SIGKILL);
            *last_emerg = time(NULL);
        }
    }
}

int main(int argc, char **argv) {
    long sweep_s = 60, base_mib = 4096, growth_mib = 96, hard_mib = 22528, emerg_mib = 1024;
    int streak_need = 3;
    int c;
    while ((c = getopt(argc, argv, "i:b:g:s:H:e:p:l:")) != -1) {
        switch (c) {
        case 'i': sweep_s = atol(optarg); break;
        case 'b': base_mib = atol(optarg); break;
        case 'g': growth_mib = atol(optarg); break;
        case 's': streak_need = atoi(optarg); break;
        case 'H': hard_mib = atol(optarg); break;
        case 'e': emerg_mib = atol(optarg); break;
        case 'p': if (nprotect < 32) protect_extra[nprotect++] = atoi(optarg); break;
        case 'l': logfile = optarg; break;
        default: fprintf(stderr, "usage: %s [-i sweepS] [-b baseMiB] [-g growMiB] [-s streak] [-H hardMiB] [-e availMiB] [-p pid] [-l log]\n", argv[0]); return 2;
        }
    }
    page_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (!(lf = fopen(logfile, "a"))) { perror("log"); return 2; }
    pid_t self = getpid();
    signal(SIGCHLD, SIG_IGN);
    logline("memguardd start pid=%d sweep=%lds psi-prefilter>10%% base=%ldMiB(pressure %ldMiB) growth=%ldMiB/%lds streak=%d hard=%ldMiB emerg<%ldMiB (single-pid kills, no cascade)",
            self, sweep_s, base_mib, base_mib / 4, growth_mib, sweep_s, streak_need, hard_mib, emerg_mib);

    time_t last_emerg = 0;
    for (;;) {
        sleep((unsigned)sweep_s);
        /* PSI 采样:本内核(6.8) PSI fd POLLIN 恒就绪(实证),不能当阻塞原语;
         * 改作每扫预检——压力期收紧击杀基线,闲期放宽。 */
        long eff_base = base_mib;
        int psi_fd = open("/proc/pressure/memory", O_RDONLY);
        if (psi_fd >= 0) {
            char pb[256];
            ssize_t n = read(psi_fd, pb, sizeof pb - 1);
            close(psi_fd);
            if (n > 0) {
                pb[n] = 0;
                char *line = strstr(pb, "some avg10=");
                if (line) {
                    int some10 = atoi(line + 11);
                    logline("psi some avg10=%d%%", some10);
                    if (some10 > 10) {
                        eff_base = base_mib / 4;
                    }
                }
            }
        }
        scan_once(self, eff_base, growth_mib, streak_need, hard_mib, emerg_mib, &last_emerg);
    }
}
