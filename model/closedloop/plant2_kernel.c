/**
 * @file plant2_kernel.c
 * @brief The two-node greenhouse plant, as one C loop, for model/closedloop/.
 *
 * Why two nodes: the single-node plant the summer campaign calibrated moves
 * about ten times slower than the air the controller's sensor sees, so it
 * cannot reproduce the limit cycle 5C88 runs (closedloop/README.md). A small
 * air node carries the fast response; a large structure/soil node carries
 * the slow one -- the warm night, the re-heat after a flush.
 *
 *   Ca dTa/dt = ka*lux + Gas*(Ts - Ta) - UA*(Ta - To)          air, the sensor's node
 *   Cs dTs/dt = ks*lux + Gas*(Ta - Ts) - Gso*(Ts - To)         structure and soil
 *   V  dAH/dt = E - Q*(AH - AHo)                               absolute humidity
 *
 *   ach   = ach_m1*o1 + ach_m2*o2 + ach_m3(dir)*o3 + ach_door*doors     [1/h]
 *   UA    = UA0 + ach * V*rho*cp/3600                                   [W/K]
 *   Q     = (ach_inf + ach) * V/3600                                    [m3/s]
 *   E     = e0 + e1*lux                                                 [kg/s]
 *   ach_m3(dir) = ach_m3 + m3_ww * max(0, cos(dir - m3_dir0))           m3_ww = 0: no direction
 *
 * o1..o3 are the windows' openness 0..1 over the step, doors the number of
 * open doors. UA0 is everything that leaks heat with the house shut (cover
 * conduction plus infiltration); ach_inf is the air-exchange part of it,
 * which only the humidity can tell apart. AH is capped at saturation at Ta.
 *
 * Backward Euler over each step's own dt: stable for any time constant,
 * which a fit exploring small air capacities needs. Fitting and replay use
 * the same discretisation, so the parameters are tied to it.
 *
 * Built by plant2.py; host-only, never part of the firmware.
 */

#include <math.h>

/* g++ compiles this file as C++ (ventmodel.build_dll): keep the names unmangled. */
#if defined(__cplusplus)
#  define P2_EXTERN extern "C"
#else
#  define P2_EXTERN
#endif
#if defined(_WIN32)
#  define P2_EXPORT P2_EXTERN __declspec(dllexport)
#else
#  define P2_EXPORT P2_EXTERN __attribute__((visibility("default")))
#endif

/* Parameter vector layout -- plant2.py's PARAMS list mirrors it. */
enum {
    P_CA_MJ = 0, P_CS_MJ, P_GAS, P_GSO, P_UA0,
    P_ACH_M1, P_ACH_M2, P_ACH_M3, P_ACH_DOOR,
    P_KA, P_KS, P_V, P_ACH_INF, P_E0, P_E1,
    P_M3_WW, P_M3_DIR0,
    P_COUNT
};

#define RHO_AIR 1.2
#define CP_AIR  1005.0
#define DEG     0.017453292519943295

P2_EXPORT int p2_param_count(void)
{
    return P_COUNT;
}

/* The calibrator's psychrometrics (calibrate_plant_dynamic.ah_from_rh at 100 %). */
static double ah_sat(double t)
{
    const double es = 611.2 * exp((17.62 * t) / (t + 243.12));
    return 2.166e-3 * es / (t + 273.15);
}

/**
 * @brief Run n steps.
 *
 * state[3] = {Ta, Ts, AH}, read at entry and written back at exit, so a
 * caller can advance one step at a time (the closed loop) or a whole summer
 * at once (the fit). restart[i]:
 *   1  full restart: Ta and AH take the measurement, Ts the measured air
 *      temperature (the best guess there is); the step is not advanced --
 *      the calibrator's convention for a new segment. Used at data gaps.
 *   2  anchor: Ta and AH take the measurement, Ts runs on. A fit that
 *      anchors every H minutes scores H-minute predictions from the real
 *      air state -- the short-horizon dynamics a controller acts on --
 *      instead of a season-long free run, which rewards a sluggish air node
 *      because it smooths over input transients it cannot time.
 *
 * @return 0, or -1 on a parameter that makes the system meaningless.
 */
P2_EXPORT int p2_run(int n, const double *p,
                     const double *dt, const double *To, const double *AHo,
                     const double *lux, const double *o1, const double *o2,
                     const double *o3, const double *doors, const double *wdir,
                     const unsigned char *restart,
                     const double *Ta_meas, const double *AH_meas,
                     double *state, double *Ta_out, double *Ts_out, double *AH_out)
{
    const double Ca = p[P_CA_MJ] * 1e6;
    const double Cs = p[P_CS_MJ] * 1e6;
    const double V = p[P_V];
    const double vrc = V * RHO_AIR * CP_AIR / 3600.0;   /* W/K per 1/h */

    if (Ca <= 0.0 || Cs <= 0.0 || V <= 0.0 || p[P_GAS] < 0.0 || p[P_GSO] < 0.0 ||
        p[P_UA0] < 0.0) {
        return -1;
    }

    double Ta = state[0], Ts = state[1], AH = state[2];

    for (int i = 0; i < n; i++) {
        if (restart != 0 && restart[i] == 1) {
            Ta = Ta_meas[i];
            Ts = Ta_meas[i];
            AH = AH_meas[i];
        } else if (restart != 0 && restart[i] == 2) {
            Ta = Ta_meas[i];
            AH = AH_meas[i];
        } else {
            const double h = dt[i];
            double ach_m3 = p[P_ACH_M3];
            if (p[P_M3_WW] != 0.0 && wdir != 0) {
                const double c = cos((wdir[i] - p[P_M3_DIR0]) * DEG);
                if (c > 0.0) {
                    ach_m3 += p[P_M3_WW] * c;
                }
            }
            const double ach = p[P_ACH_M1] * o1[i] + p[P_ACH_M2] * o2[i] +
                               ach_m3 * o3[i] + p[P_ACH_DOOR] * doors[i];
            const double UA = p[P_UA0] + ach * vrc;

            /* [ a11 a12 ] [Ta']   [b1]
             * [ a21 a22 ] [Ts'] = [b2],   a12 = a21 = -Gas */
            const double Gas = p[P_GAS], Gso = p[P_GSO];
            const double a11 = Ca / h + Gas + UA;
            const double a22 = Cs / h + Gas + Gso;
            const double b1 = Ca / h * Ta + p[P_KA] * lux[i] + UA * To[i];
            const double b2 = Cs / h * Ts + p[P_KS] * lux[i] + Gso * To[i];
            const double det = a11 * a22 - Gas * Gas;
            Ta = (b1 * a22 + Gas * b2) / det;
            Ts = (a11 * b2 + Gas * b1) / det;

            const double Q = (p[P_ACH_INF] + ach) * V / 3600.0;
            const double E = p[P_E0] + p[P_E1] * lux[i];
            AH = (V / h * AH + E + Q * AHo[i]) / (V / h + Q);
            const double sat = ah_sat(Ta);
            if (AH > sat) {
                AH = sat;
            }
        }
        if (Ta_out) { Ta_out[i] = Ta; }
        if (Ts_out) { Ts_out[i] = Ts; }
        if (AH_out) { AH_out[i] = AH; }
    }

    state[0] = Ta;
    state[1] = Ts;
    state[2] = AH;
    return 0;
}
