#include "aa.h"
#include "scs_blas.h"

/* This file uses Anderson acceleration to improve the convergence of
 * a fixed point mapping.
 * At each iteration we need to solve a (small) linear system, we
 * do this using LAPACK ?gesv.
 */

/* contains the necessary parameters to perform aa at each step */
struct ACCEL_WORK {
  aa_int type1; /* bool, if true type 1 aa otherwise type 2 */
  aa_int k;     /* aa memory */
  aa_int l;     /* variable dimension */
  aa_int iter;  /* current iteration */

  aa_float *x; /* x input to map*/
  aa_float *f; /* f(x) output of map */
  aa_float *g; /* x - f(x) */

  /* from previous iteration */
  aa_float *g_prev; /* x - f(x) */

  aa_float *y; /* g - g_prev */
  aa_float *s; /* x - x_prev */
  aa_float *d; /* f - f_prev */

  aa_float *Y; /* matrix of stacked y values */
  aa_float *S; /* matrix of stacked s values */
  aa_float *D; /* matrix of stacked d values = (S-Y) */
  aa_float *M; /* S'Y or Y'Y depending on type of aa */

  /* workspace variables */
  aa_float *work;
  blas_int *ipiv;
};

/* BLAS functions used */
aa_float BLAS(nrm2)(blas_int *n, aa_float *x, blas_int *incx);
void BLAS(axpy)(blas_int *n, aa_float *a, const aa_float *x, blas_int *incx,
                aa_float *y, blas_int *incy);
void BLAS(gemv)(const char *trans, const blas_int *m, const blas_int *n,
                const aa_float *alpha, const aa_float *a, const blas_int *lda,
                const aa_float *x, const blas_int *incx, const aa_float *beta,
                aa_float *y, const blas_int *incy);
void BLAS(gesv)(blas_int *n, blas_int *nrhs, aa_float *a, blas_int *lda,
                blas_int *ipiv, aa_float *b, blas_int *ldb, blas_int *info);
void BLAS(gemm)(const char *transa, const char *transb, aa_int *m, aa_int *n,
                aa_int *k, aa_float *alpha, aa_float *a, aa_int *lda,
                aa_float *b, aa_int *ldb, aa_float *beta, aa_float *c,
                aa_int *ldc);

/* sets a->M to S'Y or Y'Y depending on type of aa used */
static void set_m(AaWork *a) {
  blas_int bl = (blas_int)(a->l), bk = (blas_int)a->k;
  aa_float onef = 1.0, zerof = 0.0;
  BLAS(gemm)
  ("Trans", "No", &bk, &bk, &bl, &onef, a->type1 ? a->S : a->Y, &bl, a->Y, &bl,
   &zerof, a->M, &bk);
  return;
}

/* updates the workspace parameters for aa for this iteration */
static void update_accel_params(const aa_float *x, const aa_float *f,
                                AaWork *a) {
  /* at the start a->x = x_prev and a->f = f_prev */
  aa_int idx = a->iter % a->k;
  aa_int l = a->l;

  blas_int one = 1;
  blas_int bl = (blas_int)l;
  aa_float neg_onef = -1.0;

  /* g = x */
  memcpy(a->g, x, sizeof(aa_float) * l);
  /* s = x */
  memcpy(a->s, x, sizeof(aa_float) * l);
  /* d = f */
  memcpy(a->d, f, sizeof(aa_float) * l);
  /* g -= f */
  BLAS(axpy)(&bl, &neg_onef, f, &one, a->g, &one);
  /* s -= x_prev */
  BLAS(axpy)(&bl, &neg_onef, a->x, &one, a->s, &one);
  /* d -= f_prev */
  BLAS(axpy)(&bl, &neg_onef, a->f, &one, a->d, &one);

  /* g, s, d correct here */

  /* y = g */
  memcpy(a->y, a->g, sizeof(aa_float) * l);
  /* y -= g_prev */
  BLAS(axpy)(&bl, &neg_onef, a->g_prev, &one, a->y, &one);

  /* y correct here */

  /* copy y into idx col of Y */
  memcpy(&(a->Y[idx * l]), a->y, sizeof(aa_float) * l);
  /* copy s into idx col of S */
  memcpy(&(a->S[idx * l]), a->s, sizeof(aa_float) * l);
  /* copy d into idx col of D */
  memcpy(&(a->D[idx * l]), a->d, sizeof(aa_float) * l);

  /* Y, S,D correct here */

  memcpy(a->f, f, sizeof(aa_float) * l);
  memcpy(a->x, x, sizeof(aa_float) * l);

  /* x, f correct here */

  /* set M = S'*Y */
  set_m(a);

  /* M correct here */

  memcpy(a->g_prev, a->g, sizeof(aa_float) * l);

  /* g_prev set for next iter here */
  return;
}

/* solves the system of equations to perform the aa update
 * at the end f contains the next iterate to be returned
 */
static aa_int solve(aa_float *f, AaWork *a, aa_int len) {
  blas_int info = -1, bl = (blas_int)(a->l), one = 1, blen = (blas_int)len,
           bk = (blas_int)a->k;
  aa_float neg_onef = -1.0, onef = 1.0, zerof = 0.0, nrm;
  /* work = S'g or Y'g */
  BLAS(gemv)
  ("Trans", &bl, &blen, &onef, a->type1 ? a->S : a->Y, &bl, a->g, &one, &zerof,
   a->work, &one);
  /* work = M \ work, where M = S'Y or M = Y'Y */
  BLAS(gesv)(&blen, &one, a->M, &bk, a->ipiv, a->work, &blen, &info);
  nrm = BLAS(nrm2)(&bk, a->work, &one);
  if (info < 0 || nrm >= MAX_AA_NRM) {
    printf("Error in AA, iter: %i, info: %i, norm %.2e\n", a->iter, info, nrm);
    return -1;
  }
  /* if solve was successful then set f -= D * work */
  BLAS(gemv)
  ("NoTrans", &bl, &blen, &neg_onef, a->D, &bl, a->work, &one, &onef, f, &one);
  return (aa_int)info;
}

/*
 * API functions below this line, see aa.h for descriptions.
 */
AaWork *aa_init(aa_int l, aa_int aa_mem, aa_int type1) {
  AaWork *a = (AaWork *)calloc(1, sizeof(AaWork));
  if (!a) {
    printf("Failed to allocate memory for AA.\n");
    return (void *)0;
  }
  a->type1 = type1;
  a->iter = 0;
  a->l = l;
  a->k = aa_mem;
  if (a->k <= 0) {
    return a;
  }

  a->x = (aa_float *)calloc(a->l, sizeof(aa_float));
  a->f = (aa_float *)calloc(a->l, sizeof(aa_float));
  a->g = (aa_float *)calloc(a->l, sizeof(aa_float));

  a->g_prev = (aa_float *)calloc(a->l, sizeof(aa_float));

  a->y = (aa_float *)calloc(a->l, sizeof(aa_float));
  a->s = (aa_float *)calloc(a->l, sizeof(aa_float));
  a->d = (aa_float *)calloc(a->l, sizeof(aa_float));

  a->Y = (aa_float *)calloc(a->l * a->k, sizeof(aa_float));
  a->S = (aa_float *)calloc(a->l * a->k, sizeof(aa_float));
  a->D = (aa_float *)calloc(a->l * a->k, sizeof(aa_float));

  a->M = (aa_float *)calloc(a->k * a->k, sizeof(aa_float));
  a->work = (aa_float *)calloc(a->k, sizeof(aa_float));
  a->ipiv = (blas_int *)calloc(a->k, sizeof(blas_int));
  return a;
}

aa_int aa_apply(aa_float *f, const aa_float *x, AaWork *a) {
  if (a->k <= 0) {
    return 0;
  }
  update_accel_params(x, f, a);
  if (a->iter++ == 0) {
    return 0;
  }
  /* solve linear system, new point overwrites f if successful */
  return solve(f, a, MIN(a->iter - 1, a->k));
}

void aa_finish(AaWork *a) {
  if (a) {
    if (a->x) {
      free(a->x);
    }
    if (a->f) {
      free(a->f);
    }
    if (a->g) {
      free(a->g);
    }
    if (a->g_prev) {
      free(a->g_prev);
    }
    if (a->y) {
      free(a->y);
    }
    if (a->s) {
      free(a->s);
    }
    if (a->d) {
      free(a->d);
    }
    if (a->Y) {
      free(a->Y);
    }
    if (a->S) {
      free(a->S);
    }
    if (a->D) {
      free(a->D);
    }
    if (a->M) {
      free(a->M);
    }
    if (a->work) {
      free(a->work);
    }
    if (a->ipiv) {
      free(a->ipiv);
    }
    free(a);
  }
  return;
}