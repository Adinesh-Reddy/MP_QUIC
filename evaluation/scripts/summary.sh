#!/bin/bash
# Prints the table of the measurements of evaluation/results/throughput.csv: for every
# protocol the median goodput of the successful runs, and the experimental aggregation
# benefit (EBen) that the paper uses to compare the multipath protocols (Section 4.1).
#
#   ./evaluation/scripts/summary.sh                 every label of the file
#   ./evaluation/scripts/summary.sh --label lossy   one network setting
#
# EBen compares the goodput of the multipath protocol (Gm) with the goodput of its
# single-path protocol on each link (G_a, G_b; Gmax is the larger one):
#     Gm >= Gmax:  EBen = (Gm - Gmax) / (G_a + G_b - Gmax)     1 = both links fully used
#     Gm <  Gmax:  EBen = (Gm - Gmax) / Gmax                   0 = as good as the best link
#                                                             -1 = nothing was transferred
# MP-QUIC is compared with single-path MP-QUIC, Multipath TCP with TCP, as in the paper.

cd "$(dirname "$0")/../.." || exit 1
. evaluation/scripts/config.sh

LABEL=""
while [ $# -gt 0 ]; do
    case "$1" in
        --label) LABEL=$2; shift 2 ;;
        *) echo "unknown argument: $1"; sed -n '2,7p' "$0"; exit 2 ;;
    esac
done

CSV="$RESULTS/throughput.csv"
if [ ! -f "$CSV" ]; then
    echo "no measurements yet: $CSV does not exist"
    exit 1
fi

awk -F, -v wanted="$LABEL" '
# Median of the goodputs stored under a key, -1 when there is none
function median(key,    count, i, j, t, a) {
    count = n[key]
    if (count == 0) return -1
    for (i = 1; i <= count; i++) a[i] = value[key, i]
    for (i = 2; i <= count; i++)                # insertion sort
        for (j = i; j > 1 && a[j - 1] > a[j]; j--) { t = a[j]; a[j] = a[j - 1]; a[j - 1] = t }
    if (count % 2 == 1) return a[(count + 1) / 2]
    return (a[count / 2] + a[count / 2 + 1]) / 2
}

# Experimental aggregation benefit of the paper
function eben(gm, ga, gb,    gmax) {
    gmax = (ga > gb) ? ga : gb
    if (gm >= gmax) return (gm - gmax) / (ga + gb - gmax)
    return (gm - gmax) / gmax
}

function row(label, protocol, config, text,    key, m, share) {
    key = label SUBSEP protocol SUBSEP config
    m = median(key)
    if (m < 0 && failed[key] == 0) return
    if (m < 0) {
        printf "  %-10s %-34s %4d   %10s\n", protocol, text, 0, "all failed"
        return
    }
    # Part of the received bytes that came over link A
    share = (rx_a[key] + rx_b[key] > 0) ? 100 * rx_a[key] / (rx_a[key] + rx_b[key]) : 0
    printf "  %-10s %-34s %4d   %10.0f   %6.2f    %3.0f %% / %3.0f %%\n", protocol, text, n[key], m, m / 1000, share, 100 - share
}

function benefit(label, text, multi, config, single,    gm, ga, gb) {
    gm = median(label SUBSEP multi SUBSEP config)
    ga = median(label SUBSEP single SUBSEP "single_a")
    gb = median(label SUBSEP single SUBSEP "single_b")
    if (gm < 0 || ga <= 0 || gb <= 0) return
    printf "  %-44s %5.2f   (%.0f kbit/s, best single link %.0f, sum of the links %.0f)\n", text, eben(gm, ga, gb), gm, (ga > gb ? ga : gb), ga + gb
}

NR == 1 { next }
wanted != "" && $1 != wanted { next }
{
    if (!($1 in seen)) { seen[$1] = 1; labels[++label_count] = $1 }
    key = $1 SUBSEP $2 SUBSEP $3
    if ($10 == "ok") {
        n[key]++
        value[key, n[key]] = $7 + 0
        rx_a[key] += $8
        rx_b[key] += $9
    } else {
        failed[key]++
    }
}

END {
    if (label_count == 0) { print "no measurements for this label"; exit 1 }
    for (l = 1; l <= label_count; l++) {
        label = labels[l]
        printf "=== Network setting: %s ===\n", label
        printf "  %-10s %-34s %4s   %10s   %6s    %s\n", "protocol", "configuration", "runs", "kbit/s", "Mbit/s", "link A / link B"
        row(label, "udp",    "single_a",          "link A only")
        row(label, "udp",    "single_b",          "link B only")
        row(label, "tcp",    "single_a",          "link A only")
        row(label, "tcp",    "single_b",          "link B only")
        row(label, "mptcp",  "multipath_a_first", "both links, first subflow on A")
        row(label, "mptcp",  "multipath_b_first", "both links, first subflow on B")
        row(label, "mpquic", "single_a",          "link A only (one path)")
        row(label, "mpquic", "single_b",          "link B only (one path)")
        row(label, "mpquic", "multipath_a_first", "both links, initial path on A")
        row(label, "mpquic", "multipath_b_first", "both links, initial path on B")
        print  "  (median of the successful runs; last column: share of the received bytes per link)"
        print  ""
        print  "  Experimental aggregation benefit (1 = sum of both links, 0 = best single link):"
        benefit(label, "MP-QUIC, initial path on A   vs one path",  "mpquic", "multipath_a_first", "mpquic")
        benefit(label, "MP-QUIC, initial path on B   vs one path",  "mpquic", "multipath_b_first", "mpquic")
        benefit(label, "MPTCP,   first subflow on A  vs TCP",       "mptcp",  "multipath_a_first", "tcp")
        benefit(label, "MPTCP,   first subflow on B  vs TCP",       "mptcp",  "multipath_b_first", "tcp")
        print  ""
    }
}
' "$CSV"
