// lbrctl_config.c
// Config subcommand for lbrctl.
// Supports:
//   --dev PATH
//   --depth N
//   --usr-only | --ker-only | --both
//   --callstack | --no-callstack
//   --filter-mask N | --clear-filters
// Also provides --show-basic to print current LBR status before/after changes.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "lbr_API.h"  // Must define: LBR_IOCTL_GET_BASIC/SET_CTL/SET_DEPTH and LBR_CTL_* bit masks

#ifndef LBR_DEVICE_PATH
#define LBR_DEVICE_PATH "/dev/lbr_device"
#endif

// Forward decl from main
int cmd_config(int argc, char **argv);

// Optional helper: pretty-print basic report
static void print_basic_report(const struct lbr_basic_report *r) {
    printf("LBR support       : %s\n", r->has_lbr ? "yes" : "no");
    printf("IA32_LBR_CTL      : 0x%016llx\n",
           (unsigned long long)r->lbr_config.lbr_ctl);

    __u64 ctl = r->lbr_config.lbr_ctl;
#ifdef LBR_CTL_ENABLE
    printf("  enable          : %s\n", (ctl & LBR_CTL_ENABLE) ? "on" : "off");
#endif
#ifdef LBR_CTL_USR
    printf("  user (USR)      : %s\n", (ctl & LBR_CTL_USR) ? "on" : "off");
#endif
#ifdef LBR_CTL_OS
    printf("  kernel (OS)     : %s\n", (ctl & LBR_CTL_OS) ? "on" : "off");
#endif
#ifdef LBR_CTL_CALL_STACK
    printf("  call-stack      : %s\n", (ctl & LBR_CTL_CALL_STACK) ? "on" : "off");
#endif

    printf("Depth current     : %u\n", r->lbr_limits.current_depth);
    printf("Depth max         : %u\n", r->lbr_limits.max_depth_supported);
    printf("Depth bitmap      : 0x%08x\n", r->lbr_limits.depth_bitmap);
}

static void print_usage(FILE *out) {
    fprintf(out,
        "Usage:\n"
        "  lbrctl config [--dev PATH] [--depth N] [--usr-only|--ker-only|--both]\n"
        "                [--callstack|--no-callstack] [--filter-mask N|--clear-filters]\n"
        "  lbrctl run    [--dev PATH] -- <program> [args...]\n");
}

// Parse int (0x.., 0.., or decimal)
static int parse_int(const char *s, int *out) {
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0') return -1;
    *out = (int)v;
    return 0;
}

// Parse unsigned long (mask)
static int parse_ulong(const char *s, unsigned long *out) {
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0') return -1;
    *out = v;
    return 0;
}

int cmd_config(int argc, char **argv)
{
    const char *dev_path = LBR_DEVICE_PATH;

    // Options state
    int want_depth = 0, depth_value = -1;         // --depth N  (also accept legacy --set-depth N)
    int mode_set = 0, mode_usr = 0, mode_os = 0;  // --usr-only / --ker-only / --both
    int callstack_set = 0, callstack_on = 0;      // --callstack / --no-callstack
    int have_filter = 0; unsigned long filter_mask = 0; // --filter-mask N
    int clear_filters = 0;                        // --clear-filters
    int show_basic = 0;                           // --show-basic (extra helper)

    // Parse args
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            print_usage(stdout);
            return 0;
        } else if (!strcmp(a, "--dev")) {
            if (i + 1 >= argc) { fprintf(stderr, "config: missing value for --dev\n"); return 2; }
            dev_path = argv[++i];
        } else if (!strcmp(a, "--depth") || !strcmp(a, "--set-depth")) { // accept both
            if (i + 1 >= argc) { fprintf(stderr, "config: missing value for %s\n", a); return 2; }
            int d;
            if (parse_int(argv[++i], &d) != 0 || d < 0) {
                fprintf(stderr, "config: invalid depth: '%s'\n", argv[i]);
                return 2;
            }
            want_depth = 1; depth_value = d;
        } else if (!strcmp(a, "--usr-only")) {
            mode_set = 1; mode_usr = 1; mode_os = 0;
        } else if (!strcmp(a, "--ker-only")) {
            mode_set = 1; mode_usr = 0; mode_os = 1;
        } else if (!strcmp(a, "--both")) {
            mode_set = 1; mode_usr = 1; mode_os = 1;
        } else if (!strcmp(a, "--callstack")) {
            callstack_set = 1; callstack_on = 1;
        } else if (!strcmp(a, "--no-callstack")) {
            callstack_set = 1; callstack_on = 0;
        } else if (!strcmp(a, "--filter-mask")) {
            if (i + 1 >= argc) { fprintf(stderr, "config: missing value for --filter-mask\n"); return 2; }
            if (parse_ulong(argv[++i], &filter_mask) != 0) {
                fprintf(stderr, "config: invalid filter mask: '%s'\n", argv[i]);
                return 2;
            }
            have_filter = 1;
        } else if (!strcmp(a, "--clear-filters")) {
            clear_filters = 1;
        } else if (!strcmp(a, "--show-basic")) {
            show_basic = 1;
        } else {
            fprintf(stderr, "config: unknown option: %s\n", a);
            print_usage(stderr);
            return 2;
        }
    }

    // Open device
    int fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "failed to open %s: %s\n", dev_path, strerror(errno));
        return 1;
    }

    // Get baseline state (so we preserve unrelated bits)
    struct lbr_basic_report rep_before;
    __u64 ctl = 0;
    int have_baseline = 0;

    if (ioctl(fd, LBR_IOCTL_GET_BASIC, &rep_before) == 0) {
        have_baseline = 1;
        ctl = rep_before.lbr_config.lbr_ctl;
        if (show_basic) {
            printf("[Before]\n");
            print_basic_report(&rep_before);
        }
    } else {
        // Fallback if GET_BASIC not supported
        have_baseline = 0;
        ctl = 0;
    }

    // Apply depth if requested
    if (want_depth) {
        __u32 depth_arg = (depth_value < 0) ? 0u : (__u32)depth_value;
        if (ioctl(fd, LBR_IOCTL_SET_DEPTH, &depth_arg) < 0) {
            fprintf(stderr, "SET_DEPTH(%u) failed: %s\n", (unsigned)depth_arg, strerror(errno));
            close(fd);
            return 1;
        }
    }

    // Build new CTL value:
    // Priority:
    //   1) --clear-filters => ctl = 0
    //   2) --filter-mask N => ctl = N
    //   3) otherwise start from baseline (if available) and toggle bits
    if (clear_filters) {
        ctl = 0;
    } else if (have_filter) {
        ctl = (__u64)filter_mask;
    } else if (!have_baseline) {
        ctl = 0; // no baseline; start clean
    }

    // Toggle USR/OS bits if requested
#ifdef LBR_CTL_USR
    if (mode_set && mode_usr)  ctl |=  LBR_CTL_USR;
    if (mode_set && !mode_usr) ctl &= ~((__u64)LBR_CTL_USR);
#endif
#ifdef LBR_CTL_OS
    if (mode_set && mode_os)   ctl |=  LBR_CTL_OS;
    if (mode_set && !mode_os)  ctl &= ~((__u64)LBR_CTL_OS);
#endif

    // Toggle CALL_STACK if requested
#ifdef LBR_CTL_CALL_STACK
    if (callstack_set && callstack_on)  ctl |=  LBR_CTL_CALL_STACK;
    if (callstack_set && !callstack_on) ctl &= ~((__u64)LBR_CTL_CALL_STACK);
#endif

    // Apply CTL
    if (ioctl(fd, LBR_IOCTL_SET_CTL, &ctl) < 0) {
        fprintf(stderr, "SET_CTL(0x%llx) failed: %s\n",
                (unsigned long long)ctl, strerror(errno));
        close(fd);
        return 1;
    }

    // Read-back and print after state (verification)
    struct lbr_basic_report rep_after;
    if (ioctl(fd, LBR_IOCTL_GET_BASIC, &rep_after) == 0) {
        printf("\n[After]\n");
        print_basic_report(&rep_after);
    } else {
        // Not fatal; just report
        fprintf(stderr, "GET_BASIC (after) failed: %s\n", strerror(errno));
    }

    close(fd);
    return 0;
}
