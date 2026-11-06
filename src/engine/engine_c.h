/* NovaInsert engine C surface (plan Task 13): the correlator and lock evaluator for non-C++ callers and test harnesses.
   Thin wrapper over engine/correlator.hpp; reasons are ni::engine::Reason values. Not real-time safe (create allocates). */
#ifndef NOVAINSERT_ENGINE_C_H
#define NOVAINSERT_ENGINE_C_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ni_corr ni_corr;

typedef struct ni_corr_result
{
    int lag;
    double mainAmp;
    int negative;
    double fractional, pbrDb, altPeakRelDb;
    int altLag;
    double returnPeakDbfs, returnRmsDbfs, preSilenceRmsDbfs, snrDb;
    int clipped;
} ni_corr_result;

ni_corr* ni_corr_create(int fs);                                                  /* NULL on fs <= 0 or allocation failure */
void ni_corr_destroy(ni_corr* c);                                                 /* NULL is a no-op */
int ni_corr_analyse(ni_corr* c, const float* x, int n, int hwBlock, ni_corr_result* out);   /* 0 ok, -1 bad argument */
int ni_lock_evaluate(const ni_corr_result r[3], unsigned genStart, unsigned genNow, int hwBlock,
                     int* lagOut, int* reasonOut);                                /* 1 locked, 0 rejected, -1 bad argument */

#ifdef __cplusplus
}
#endif

#endif
