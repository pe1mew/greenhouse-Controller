/**
 * @file m3_char.cpp
 * @brief The arithmetic of M3's characterisation run -- see m3_char.h.
 *
 * No allocation and no large buffers: the firmware calls this from T17, whose
 * stack is not the place for a copy of a phase's readings. A median is
 * therefore a selection over the caller's array, O(n^2) for the few hundred
 * readings a move holds at most.
 */

#include "m3_char.h"

#include <math.h>
#include <string.h>

namespace {

/** The k-th smallest (0-based) of the values `get(i)` for which `keep(i)`. */
template <typename Get, typename Keep>
double kth_smallest(size_t n, Get get, Keep keep, size_t k)
{
    for (size_t i = 0; i < n; i++) {
        if (!keep(i)) { continue; }
        const double v = get(i);
        size_t less = 0u, equal = 0u;
        for (size_t j = 0; j < n; j++) {
            if (!keep(j)) { continue; }
            const double w = get(j);
            if (w < v)       { less++; }
            else if (w == v) { equal++; }
        }
        if (less <= k && k < less + equal) { return v; }
    }
    return 0.0;   /* not reached when k is below the count kept */
}

/** The median of the values kept: the mean of the two middle ones for an even count. */
template <typename Get, typename Keep>
double median_of(size_t n, Get get, Keep keep, size_t *count)
{
    size_t m = 0u;
    for (size_t i = 0; i < n; i++) {
        if (keep(i)) { m++; }
    }
    *count = m;
    if (m == 0u) { return 0.0; }
    const double lo = kth_smallest(n, get, keep, (m - 1u) / 2u);
    const double hi = kth_smallest(n, get, keep, m / 2u);
    return (lo + hi) / 2.0;
}

uint16_t clamp_u16(double v)
{
    if (!(v > 0.0))     { return 0u; }   /* also NaN */
    if (v >= 65535.0)   { return 65535u; }
    return (uint16_t)lround(v);
}

int32_t round_i32(double v)
{
    if (v >= 2147483647.0)  { return INT32_MAX; }
    if (v <= -2147483648.0) { return INT32_MIN; }
    return (int32_t)lround(v);
}

}  // namespace

/* ---- phase 1 ------------------------------------------------------------------ */

bool m3c_speed(const m3c_sample_t *s, size_t n, int32_t from_x10, int32_t to_x10,
               m3c_speed_t *out)
{
    if (out == NULL) { return false; }
    memset(out, 0, sizeof(*out));
    if (s == NULL || n == 0u || n > M3C_MAX_SAMPLES) { return false; }
    const double span = (double)to_x10 - (double)from_x10;
    if (span == 0.0) { return false; }

    /* The middle 60 % of the move, whichever way it went. */
    const double a  = (double)from_x10 + 0.2 * span;
    const double b  = (double)from_x10 + 0.8 * span;
    const double lo = (a < b) ? a : b;
    const double hi = (a < b) ? b : a;

    size_t n_mid = 0u;
    const double v = median_of(
        n,
        [s](size_t i) { return fabs((double)s[i].rate_x10); },
        [s, lo, hi](size_t i) {
            return (double)s[i].pos_x10 >= lo && (double)s[i].pos_x10 <= hi;
        },
        &n_mid);
    if (n_mid < 3u) { return false; }

    /* Gaps between consecutive readings; unsigned subtraction survives a wrap. */
    size_t n_gap = 0u;
    const double g = median_of(
        n,
        [s](size_t i) { return (double)(uint32_t)(s[i].t_ms - s[i - 1u].t_ms); },
        [s](size_t i) { return i > 0u && (uint32_t)(s[i].t_ms - s[i - 1u].t_ms) > 0u; },
        &n_gap);

    out->speed_x10 = clamp_u16(v);
    out->read_ms   = (n_gap > 0u) ? clamp_u16(g) : 0u;
    out->n_mid     = (uint16_t)((n_mid > 65535u) ? 65535u : n_mid);
    return true;
}

/* ---- phases 2 and 3 ------------------------------------------------------------- */

void m3c_mark_reversals(m3c_pulse_t *p, size_t n, bool last_opening)
{
    if (p == NULL) { return; }
    bool prev = last_opening;
    for (size_t i = 0; i < n; i++) {
        p[i].reversal = (p[i].opening != prev);
        if (p[i].valid) { prev = p[i].opening; }
    }
}

bool m3c_reversal_loss(const m3c_pulse_t *p, size_t n, bool opening, uint16_t req_ms,
                       m3c_reversal_t *out)
{
    if (out == NULL) { return false; }
    memset(out, 0, sizeof(*out));
    if (p == NULL) { return false; }

    int64_t sum_rev = 0, sum_same = 0;
    uint32_t n_rev = 0u, n_same = 0u;
    for (size_t i = 0; i < n; i++) {
        if (!p[i].valid || p[i].opening != opening || p[i].req_ms != req_ms) { continue; }
        if (p[i].reversal) { sum_rev  += p[i].disp_x10; n_rev++; }
        else               { sum_same += p[i].disp_x10; n_same++; }
    }
    out->n_rev  = (uint8_t)((n_rev  > 255u) ? 255u : n_rev);
    out->n_same = (uint8_t)((n_same > 255u) ? 255u : n_same);
    /* Each mean is reported when it exists, as the harness prints the one it
     * has; the loss needs both. */
    const double m_rev  = (n_rev  > 0u) ? (double)sum_rev  / (double)n_rev  : 0.0;   /* 0.1 mm */
    const double m_same = (n_same > 0u) ? (double)sum_same / (double)n_same : 0.0;
    out->rev_mean_x100  = round_i32(m_rev * 10.0);
    out->same_mean_x100 = round_i32(m_same * 10.0);
    if (n_rev == 0u || n_same == 0u) { return false; }
    out->loss_x100      = round_i32((m_same - m_rev) * 10.0);
    return true;
}

uint32_t m3c_noise_x1000(const int32_t *pos_x10, size_t n)
{
    if (pos_x10 == NULL || n < 2u) { return 0u; }
    double mean = 0.0;
    for (size_t i = 0; i < n; i++) { mean += (double)pos_x10[i] / 10.0; }
    mean /= (double)n;
    double ss = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double d = (double)pos_x10[i] / 10.0 - mean;
        ss += d * d;
    }
    const double sd_mm = sqrt(ss / (double)(n - 1u));
    return (uint32_t)lround(sd_mm * 1000.0);
}

uint32_t m3c_moved_threshold_x1000(uint32_t noise_x1000)
{
    const uint64_t t = 3u * (uint64_t)noise_x1000;
    if (t < (uint64_t)M3C_MOVED_MIN_X1000) { return M3C_MOVED_MIN_X1000; }
    return (t > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)t;
}

bool m3c_minmove(const m3c_pulse_t *p, size_t n, bool opening, uint32_t thresh_x1000,
                 m3c_minmove_t *out)
{
    if (out == NULL) { return false; }
    memset(out, 0, sizeof(*out));
    if (p == NULL) { return false; }

    /* Same-direction pulses only: not a take-up, not after a reversal. */
    const auto counts = [p, opening](size_t i) {
        return p[i].valid && p[i].opening == opening && !p[i].takeup && !p[i].reversal;
    };

    /* The distinct widths, longest first. */
    uint16_t w[M3C_MAX_WIDTHS];
    uint8_t  nw = 0u;
    for (size_t i = 0; i < n; i++) {
        if (!counts(i)) { continue; }
        bool seen = false;
        for (uint8_t k = 0u; k < nw; k++) {
            if (w[k] == p[i].req_ms) { seen = true; break; }
        }
        if (seen) { continue; }
        if (nw == M3C_MAX_WIDTHS) { return false; }
        uint8_t at = nw;
        while (at > 0u && w[at - 1u] < p[i].req_ms) {
            w[at] = w[at - 1u];
            at--;
        }
        w[at] = p[i].req_ms;
        nw++;
    }
    if (nw == 0u) { return false; }

    bool reliable[M3C_MAX_WIDTHS] = {};
    bool ok_so_far = true;
    for (uint8_t k = 0u; k < nw; k++) {
        m3c_width_row_t *r = &out->row[k];
        r->req_ms = w[k];
        int64_t  sum_x10 = 0;
        uint64_t sum_us  = 0u;
        uint32_t cnt = 0u, moved = 0u;
        int32_t  mn = INT32_MAX;
        for (size_t i = 0; i < n; i++) {
            if (!counts(i) || p[i].req_ms != w[k]) { continue; }
            cnt++;
            sum_x10 += p[i].disp_x10;
            sum_us  += (p[i].width_us != 0u) ? p[i].width_us : (uint32_t)p[i].req_ms * 1000u;
            if (p[i].disp_x10 < mn) { mn = p[i].disp_x10; }
            /* MORE than the threshold, as the harness: `x > thresh`. */
            if ((int64_t)p[i].disp_x10 * 100 > (int64_t)thresh_x1000) { moved++; }
        }
        r->n              = (uint8_t)((cnt > 255u) ? 255u : cnt);
        r->moved          = (uint8_t)((moved > 255u) ? 255u : moved);
        r->real_us_mean   = (uint32_t)llround((double)sum_us / (double)cnt);
        r->disp_mean_x100 = round_i32((double)sum_x10 * 10.0 / (double)cnt);
        r->disp_min_x10   = mn;

        /* Floor 2 counts only an unbroken run from the longest width down: one
         * width that failed ends it, however a shorter one then does. */
        ok_so_far = ok_so_far && (moved == cnt);
        if (ok_so_far) {
            out->floor2_ms        = w[k];
            out->floor2_real_us   = r->real_us_mean;
            out->floor2_disp_x100 = r->disp_mean_x100;
            reliable[k] = true;
        }
    }
    out->n_rows = nw;

    /* The line through the pulses of that run, at their real widths:
     * displacement [mm] = slope [mm/ms] x width [ms] + intercept. */
    const auto in_fit = [p, &counts, &w, &reliable, nw](size_t i) {
        if (!counts(i)) { return false; }
        for (uint8_t k = 0u; k < nw; k++) {
            if (w[k] == p[i].req_ms) { return reliable[k]; }
        }
        return false;
    };
    const auto x_of = [p](size_t i) {
        return (double)((p[i].width_us != 0u) ? p[i].width_us
                                              : (uint32_t)p[i].req_ms * 1000u) / 1000.0;
    };
    double sx = 0.0, sy = 0.0;
    uint32_t m = 0u;
    for (size_t i = 0; i < n; i++) {
        if (!in_fit(i)) { continue; }
        sx += x_of(i);
        sy += (double)p[i].disp_x10 / 10.0;
        m++;
    }
    out->fit_n = (uint16_t)((m > 65535u) ? 65535u : m);
    if (m < 3u) { return true; }
    const double mx = sx / (double)m, my = sy / (double)m;
    double sxx = 0.0, sxy = 0.0;
    for (size_t i = 0; i < n; i++) {
        if (!in_fit(i)) { continue; }
        const double dx = x_of(i) - mx;
        sxx += dx * dx;
        sxy += dx * ((double)p[i].disp_x10 / 10.0 - my);
    }
    if (!(sxx > 0.0)) { return true; }
    const double slope = sxy / sxx;
    const double icpt  = my - slope * mx;
    if (!(slope > 0.0)) { return true; }
    out->fit_ok        = true;
    out->fit_speed_x10 = clamp_u16(slope * 10000.0);   /* mm/ms -> 0.1 mm/s */
    const double dead  = -icpt / slope;
    out->dead_ms = (int16_t)((dead >= 32767.0) ? 32767 : (dead <= -32768.0) ? -32768
                                                : lround(dead));
    return true;
}

/* ---- phase 3: the width search ----------------------------------------------- */

void m3c_search_start(m3c_search_t *s, uint16_t w0_ms)
{
    if (s == NULL) { return; }
    memset(s, 0, sizeof(*s));
    s->next_ms = w0_ms;
}

void m3c_search_result(m3c_search_t *s, bool all_moved, uint16_t min_ms)
{
    if (s == NULL || s->next_ms == 0u) { return; }
    const uint16_t w = s->next_ms;
    if (all_moved) {
        if (s->good_ms == 0u || w < s->good_ms) { s->good_ms = w; }
    } else if (w > s->bad_ms) {
        s->bad_ms = w;
    }

    if (s->bad_ms == 0u) {                       /* still halving */
        const uint16_t half = (uint16_t)(w / 2u);
        s->next_ms = (half > 0u && half >= min_ms) ? half : 0u;
        return;
    }
    /* The start width failed: there is nothing above it to refine towards. */
    if (s->good_ms == 0u || s->refined >= 2u) { s->next_ms = 0u; return; }
    const uint16_t mid = (uint16_t)(((uint32_t)s->good_ms + s->bad_ms + 1u) / 2u);
    if (mid >= s->good_ms || mid <= s->bad_ms) { s->next_ms = 0u; return; }
    s->refined++;
    s->next_ms = mid;
}

/* ---- phase 4a ------------------------------------------------------------------- */

bool m3c_wp02(const int16_t *rest_x10, const bool *from_below, size_t n, int16_t target_x10,
              m3c_wp02_t *out)
{
    if (out == NULL) { return false; }
    memset(out, 0, sizeof(*out));
    if (rest_x10 == NULL || from_below == NULL || n == 0u || n > 255u) { return false; }

    int64_t sum = 0, sum_b = 0, sum_a = 0;
    uint32_t n_b = 0u, n_a = 0u;
    int32_t mn = INT32_MAX, mx = INT32_MIN;
    double sq = 0.0;
    for (size_t i = 0; i < n; i++) {
        const int32_t v = rest_x10[i];
        sum += v;
        if (from_below[i]) { sum_b += v; n_b++; } else { sum_a += v; n_a++; }
        if (v < mn) { mn = v; }
        if (v > mx) { mx = v; }
        const double e = (double)v - (double)target_x10;
        sq += e * e;
    }
    const double mean = (double)sum / (double)n;   /* 0.1 % */
    out->n           = (uint8_t)n;
    out->n_below     = (uint8_t)n_b;
    out->n_above     = (uint8_t)n_a;
    out->mean_x100   = round_i32(mean * 10.0);
    out->err_x100    = round_i32((mean - (double)target_x10) * 10.0);
    out->spread_x100 = (mx - mn) * 10;
    out->hyst_x100   = (n_b > 0u && n_a > 0u)
        ? round_i32(((double)sum_b / (double)n_b - (double)sum_a / (double)n_a) * 10.0)
        : 0;
    out->rms_x100    = round_i32(sqrt(sq / (double)n) * 10.0);
    out->pass        = (mx - mn) <= M3C_WP02_PASS_X10;
    return true;
}

/* ---- phase 4b ------------------------------------------------------------------- */

bool m3c_band_candidate(const m3c_band_in_t *in, uint16_t min_mm, uint16_t max_mm,
                        m3c_band_cand_t *out)
{
    if (out == NULL) { return false; }
    memset(out, 0, sizeof(*out));
    if (in == NULL) { return false; }

    const uint16_t v = (in->speed_x10[0] > in->speed_x10[1]) ? in->speed_x10[0]
                                                             : in->speed_x10[1];
    /* ms x 0.1 mm/s = 0.0001 mm; / 100 -> 0.01 mm */
    out->floor1_x100 = (int32_t)(((int64_t)in->read_ms * v + 50) / 100);
    const int32_t f2 = (in->floor2_x100[0] > in->floor2_x100[1]) ? in->floor2_x100[0]
                                                                 : in->floor2_x100[1];
    out->floor2_x100 = (f2 > 0) ? f2 : 0;
    if (in->window_mm == 0u) { return false; }
    /* 0.01 % of window_mm = window_mm / 10000 mm; x100 -> / 100 */
    const int64_t rms = (in->rms_x100 > 0) ? in->rms_x100 : 0;
    out->landing_x100 = (int32_t)((3 * rms * in->window_mm + 50) / 100);
    const int32_t ld = (in->lead_x100[0] > in->lead_x100[1]) ? in->lead_x100[0]
                                                             : in->lead_x100[1];
    out->lead_x100 = (ld > 0) ? (int32_t)(((int64_t)ld * in->window_mm + 50) / 100) : 0;

    /* The largest; on a tie the earlier term names it. */
    int32_t best = out->floor1_x100;
    out->which = M3C_TERM_FLOOR1;
    if (out->floor2_x100  > best) { best = out->floor2_x100;  out->which = M3C_TERM_FLOOR2; }
    if (out->landing_x100 > best) { best = out->landing_x100; out->which = M3C_TERM_LANDING; }
    if (out->lead_x100    > best) { best = out->lead_x100;    out->which = M3C_TERM_LEAD; }

    int64_t b0 = (best > 0) ? ((int64_t)best + 99) / 100 : 0;   /* rounded UP to whole mm */
    if (b0 < (int64_t)min_mm) { b0 = min_mm; }
    out->b0_mm = (uint16_t)((b0 > 65535) ? 65535 : b0);
    return b0 <= (int64_t)max_mm;
}

uint16_t m3c_check_step_x10(uint16_t band_mm, uint16_t window_mm)
{
    if (window_mm == 0u) { return 0u; }
    /* 1.25 x band [mm] in 0.1 % of the window: band x 125/100 x 1000 / window */
    const uint32_t num = (uint32_t)band_mm * (uint32_t)M3C_CHECK_FACTOR_PCT * 10u;
    const uint32_t v = (num + window_mm - 1u) / window_mm;
    return (uint16_t)((v > 65535u) ? 65535u : v);
}

m3c_check_t m3c_band_check(uint16_t band_mm, const int32_t *land_err_x10, size_t n,
                           uint8_t raises_done, uint16_t max_mm,
                           uint16_t *next_mm, int32_t *worst_x10)
{
    /* Unsigned: the magnitude of INT32_MIN does not fit in an int32, and with a
     * signed abs the compiler may assume it never overflows (the firmware
     * builds with -Wstrict-overflow=2, which says so). */
    uint32_t worst = 0u;
    if (land_err_x10 != NULL) {
        for (size_t i = 0; i < n; i++) {
            const int32_t  e = land_err_x10[i];
            const uint32_t a = (e < 0) ? 0u - (uint32_t)e : (uint32_t)e;
            if (a > worst) { worst = a; }
        }
    }
    if (worst_x10 != NULL) {
        *worst_x10 = (worst > (uint32_t)INT32_MAX) ? INT32_MAX : (int32_t)worst;
    }
    if (land_err_x10 == NULL || n == 0u) { return M3C_CHECK_GIVE_UP; }   /* nothing to judge */
    if (worst <= (uint32_t)band_mm * 10u) { return M3C_CHECK_PASS; }
    if (raises_done >= M3C_CHECK_MAX_RAISES) { return M3C_CHECK_GIVE_UP; }

    uint32_t next = ((uint32_t)band_mm * M3C_CHECK_FACTOR_PCT + 99u) / 100u;   /* x1.25, up */
    if (next <= band_mm) { next = (uint32_t)band_mm + 1u; }
    if (next > max_mm) { return M3C_CHECK_GIVE_UP; }
    if (next_mm != NULL) { *next_mm = (uint16_t)next; }
    return M3C_CHECK_RAISE;
}
