/**
 * @brief Utilities for 3d integrals (this is not about the k2
 *        project but a side project that uses k2 for convenience).
 *
 * Note that serializations are done in Python.
 *
 * @copyright
 * Copyright (c)  2020  Xiaomi Inc.  (author: Daniel Povey)
 *
 * @copyright
 * See LICENSE for clarification regarding multiple authors
 */

#ifndef K2_CSRC_FSA_INTEGRAL_H_
#define K2_CSRC_FSA_INTEGRAL_H_

#include <string>

#include "k2/csrc/array.h"
#include "k2/csrc/array_ops.h"
#include "k2/csrc/ragged.h"


namespace k2 {

struct Vec3 {
  double x[3];
};
inline std::ostream& operator << (std::ostream &os, const Vec3 &v) {
  return os << '(' << v.x[0] << "," << v.x[1] << "," << v.x[2] << ')';
}


struct Mat3 {
  double m[3][3];
};

inline std::ostream& operator << (std::ostream &os, const Mat3 &m) {
  os << '(';
  for (int i = 0; i < 3; i++) {
    for (int j = 0; i < 3; i++) os << m.m[i][j];
    os << "\n";
  }
  return os << ')';
}


// The problem configuration: a number of points and the locations of the
// points.


#define MAX_POINTS 4

struct Configuration {
  double max_allowed_density;  // e.g. 10000, this is to avoid inf's or nan's
                               // appearing in case we get extremely close to a
                               // point.  (The integrals dont diverge but the
                               // thing we're integrating is unbounded).
  Vec3 points[MAX_POINTS];  // Allow maximum 4 points.
  double masses[MAX_POINTS];   // The points' masses (>= 0; but 0 means effectively no point).
};

inline std::ostream &operator << (std::ostream &os, const Configuration &c) {
  os << "{ max-density=" << c.max_allowed_density;
  for (int i = 0; i < MAX_POINTS; i++) {
    if (c.masses[i] == 0) continue;
    os << ", mass=" << c.masses[i] << ", center=" << c.points[i];
  }
  return os << "}";
}

void InitConfigurationDefault(Configuration *c) {
  // zeroes points and masses, sets max_allowed_density to 1e+05;
  c->max_allowed_density = 1.0e+08;
  for (int i = 0; i < MAX_POINTS; i++) {
    c->masses[i] = 0.0;
    for (int j = 0; j < 3; j++)
      c->points[i].x[j] = 0.0;
  }
}

__host__ __device__ void SetZero(Vec3 *vec) {
#pragma unroll
  for (int i = 0; i < 3; i++)
    vec->x[i] = 0.0;
}

__host__ __device__ void SetZero(Mat3 *mat) {
#pragma unroll
  for (int i = 0; i < 3; i++)
#pragma unroll
    for (int j = 0; j < 3; j++)
      mat->m[i][j] = 0.0;
}

// mat += alpha * vec vec'
__host__ __device__ void AddVec2(double alpha, const Vec3 &vec,
                        Mat3 *mat) {
#pragma unroll
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      mat->m[i][j] += alpha * vec.x[i] * vec.x[j];
}

// mat += alpha * i
__host__ __device__ void AddScaledUnit(double alpha, Mat3 *mat) {
#pragma unroll
  for (int i = 0; i < 3; i++)
    mat->m[i][i] += alpha;
}


// c = a - b
__host__ __device__ __forceinline__ void Sub(const Vec3 &a,
                                    const Vec3 &b,
                                    Vec3 *c) {
#pragma unroll
  for (int i = 0; i < 3; i++)
    c->x[i] = a.x[i] - b.x[i];
}

__host__ __device__ __forceinline__ double DotProduct(const Vec3 &a,
                                             const Vec3 &b) {
  return a.x[0] * b.x[0] + a.x[1] * b.x[1] + a.x[2] * b.x[2];
}
__host__ __device__ __forceinline__ void Scale(double alpha, Vec3 *a) {
  a->x[0] *= alpha;
  a->x[1] *= alpha;
  a->x[2] *= alpha;
}

// returns the trace of the matrix product mat mat' (actually `mat` will be
// symmetric so transpose doesn't matter)... it simplifies to the sum of squared
// elements.  also checks that mat's trace is close to zero, which
// is an expectation specific to our problem.
__host__ __device__ __forceinline__ double TraceMatSq(const Mat3 &mat) {
  double ans = 0.0, trace = 0.0;
#pragma unroll
  for (int i = 0; i < 3; i++) {
    trace += mat.m[i][i];
#pragma unroll
    for (int j = 0; j < 3; j++)
      ans += mat.m[i][j] * mat.m[i][j];
  }
  // we should be dealing with trace-free matrices.
  K2_CHECK_LE(trace * trace, 0.001 * ans);
  return ans;
}

// returns trace of the product (a b') (we are assuming a commutes with b so we
// dont have to worry about transpose-ness).
__host__ __device__ __forceinline__ double TraceMatMat(const Mat3 &a, const Mat3 &b) {
  double ans = 0.0;
#pragma unroll
  for (int i = 0; i < 3; i++)
#pragma unroll
    for (int j = 0; j < 3; j++)
      ans += a.m[i][j] * b.m[i][j];
  return ans;
}

__host__ __device__ __forceinline__ double TraceMat(const Mat3 &a) {
  return a.m[0][0] + a.m[1][1] + a.m[2][2];
}

// matrix product where the result is known to be symmetric (intended for powers of a symmetric matrix).
__host__ __device__ __forceinline__ void MatmulSymmetric(const Mat3 &a, const Mat3 &b, Mat3 *c) {
#pragma unroll
  for (int i = 0; i < 3; i++) {
#pragma unroll
    for (int j = 0; j <= i; j++) {
      double sum = 0.0;
#pragma unroll
      for (int k = 0; k < 3; k++)
        sum += a.m[i][k] * b.m[j][k];
      c->m[i][j] = c->m[j][i] = sum;
    }
  }
}


__host__ __device__ void swap (double &a, double &b) {
  double temp = b; b = a; a = temp;
}
__host__ __device__ void GetEigs(const Mat3 &mat, Vec3 *eigs) {
  //  auto A = mat.m;
  double beta = mat.m[0][1]*mat.m[0][1] +  mat.m[0][2]*mat.m[0][2] + mat.m[1][2]*mat.m[1][2]
      - mat.m[0][0]*mat.m[1][1] - mat.m[1][1]*mat.m[2][2] - mat.m[2][2]*mat.m[0][0],
      gamma = mat.m[0][0]*mat.m[1][1]*mat.m[2][2] + 2*mat.m[0][1]*mat.m[1][2]*mat.m[0][2]
      - mat.m[0][0]*mat.m[1][2]*mat.m[1][2] - mat.m[0][1]*mat.m[0][1]*mat.m[2][2] - mat.m[0][2]*mat.m[0][2]*mat.m[1][1];
  if (beta < 1.0e-30) {
    eigs->x[0] = 0;
    eigs->x[1] = 0;
    eigs->x[2] = 0;
    return;
  }
  double sqrt_abs_p_3 = sqrt(beta / 3),
      val = gamma / (2*pow(sqrt_abs_p_3,3));
  // sometimes `val` goes very slightly out of the range [-1,1] which causes
  // NaNs; stop this from happening.
  if (val > 1.0) val = 1.0;
  if (val < -1.0) val = -1.0;
  double phi = acos(val);
  eigs->x[0] = 2 * sqrt_abs_p_3 * cos(phi / 3);
  eigs->x[1] = - 2 * sqrt_abs_p_3 * cos((phi - M_PI) / 3);
  eigs->x[2] = - 2 * sqrt_abs_p_3 * cos((phi + M_PI) / 3);

  // order from most positive to most negative.
  if ((eigs->x[0]) < (eigs->x[1])) swap(eigs->x[0], eigs->x[1]);
  if ((eigs->x[0]) < (eigs->x[2])) swap(eigs->x[0], eigs->x[2]);
  if ((eigs->x[1]) < (eigs->x[2])) swap(eigs->x[1], eigs->x[2]);
}

__host__ __device__ double DensityGivenMat(const Mat3 &mat) {
  //double tmm = TraceMatSq(mat);  // tr(mat mat') == tr(mat mat), since mat is
  //// symmetric

#if 0
  Mat3 mat_sq, mat_4;
  MatmulSymmetric(mat, mat, &mat_sq);
  MatmulSymmetric(mat_sq, mat_sq, &mat_4);
  double pow2 = TraceMat(mat_sq);
  if (pow2 == 0.0) return 0.0;
  double pow3 = TraceMatMat(mat, mat_sq),
      pow4 = TraceMat(mat_4),
      pow5 = TraceMatMat(mat_4, mat),
      pow6 = TraceMatMat(mat_4, mat_sq),
      pow3_norm = pow3 * pow(pow2, -3.0/2.0),
      pow4_norm = pow4 * pow(pow2, -4.0/2.0),
      pow5_norm = pow5 * pow(pow2, -5.0/2.0),
      pow6_norm = pow6 * pow(pow2, -6.0/2.0);

  double alpha = -0.40, beta = 0.0, gamma = 0.0, delta = -0.05,
      factor = 1.0 + pow3_norm * alpha + pow4_norm * beta +
          pow5_norm * gamma + pow6_norm * delta;
  K2_CHECK_GE(factor, 0);
  return factor * pow(pow2, 1.0/3);
#else
  Vec3 eigs;
  GetEigs(mat, &eigs);
  double tmm = eigs.x[0]*eigs.x[0] +
      eigs.x[1]*eigs.x[1] +
      eigs.x[2]*eigs.x[2];

  double pos_eigs = 0, pos_eigs_sq = 0,
      neg_eigs = 0, neg_eigs_sq = 0,
      pos_eigs_pow = 0, neg_eigs_pow = 0;
  for (int i = 0; i < 3; i++) {
    double eig = eigs.x[i];
    if (eig > 0) { pos_eigs += eig;  pos_eigs_sq += eig*eig; pos_eigs_pow += pow(eig, 1.0/3); }
    else { neg_eigs += eig;  neg_eigs_sq += eig*eig; neg_eigs_pow += pow(-eig, 1.0/3); }
  }
  //return pow(neg_eigs_sq + -0.05 * pos_eigs_sq, 1.0/3);
  //K2_CHECK(eigs.x[2] <= 0);
  //return pow(-eigs.x[2] - 0.5*eigs.x[1], 1.0/3);
  return pos_eigs_pow + -0.25 * neg_eigs_pow;
#endif
  //return pow(abs(eigs.x[0]), 2.0/3) + pow(abs(eigs.x[1]), 2.0/3) + pow(abs(eigs.x[2]), 2.0/3) + pow(tmm, 1.0/3);

  //return pow(abs(eigs.x[0]*eigs.x[1]) + abs(eigs.x[1]*eigs.x[2]) + abs(eigs.x[2]*eigs.x[0]), 1.0/3);

  // return pow(abs(eigs.x[0]-eigs.x[2]), 2.0/3);

  //  return pow(abs(eigs.x[0]), 2.0/3) + pow(abs(eigs.x[1]), 2.0/3) + pow(abs(eigs.x[2]), 2.0/3) + pow(tmm, 1.0/3);

  //      + abs(eigs.x[1]) + abs(eigs.x[2]) + 0.5*sqrt(tmm), 2.0 / 3);
      //return pow(tmm, 1.0 / 3.0) - pow(abs(eigs.x[0]*eigs.x[1]*eigs.x[2])
      //return pow(tmm, 1.0 / 3.0) - pow(abs(eigs.x[0]*eigs.x[1]*eigs.x[2])
}

__host__ __device__ double ComputeDensity(const Configuration configuration,
                                          const Vec3 &point) {

  Mat3 mat;  // we'll computing the 3rd root of tr(m * m), which is the 3rd root
             // of the sum of squared elements of m.
  SetZero(&mat);
  for (int p = 0; p < MAX_POINTS; p++) {
    // Update `mat` with contribution from this point
    if (configuration.masses[p] == 0)
      continue;
    Vec3 direction;
    Sub(configuration.points[p], point, &direction);
    double r = sqrt(DotProduct(direction, direction));
    if (r < 1.0e-20) r = 1.0e-20;  // avoid NaN or inf
    Scale(1.0 / r, &direction);  // make direction unit-length
    double scale = configuration.masses[p] * pow(r, -3.0);
    // sign might be opposite here and a scalar factor might be off,
    // but it won't matter for our purposes.
    AddVec2(scale, direction, &mat);
    AddScaledUnit(-scale / 3.0, &mat);
    // Note: trace of mat should stay zero.
  }
  double ans = DensityGivenMat(mat);
  // the following is to avoid problems as we approach a singularity (they are
  // integrable but I dont want to deal with super large numbers).
  if (ans > configuration.max_allowed_density)
    ans = configuration.max_allowed_density;
  return ans;
}

// A Volume represents a cubic volume centered at `center`, with radius `r`
// (meaning: it represents the space between x - r and x + r in each of the 3
// dimensions).
struct Volume {
  Vec3 center;
  double r;
};

inline std::ostream &operator << (std::ostream &os, const Volume &v) {
  return os << "{ center=" << v.center << ", r=" << v.r << "}";
}

/*
  IntegralPart represents a piece of the integral (a cubic region).  The thing
  (density) we're integrating is stored separately, as the Configuration.
 */
struct IntegralPart {
  Volume volume;
  // The density we're integrating, evaluated at the center of the cube.
  double density;

  // density_deriv_norm is an approxmation to the 2-norm of the derivative
  // of the function we're computing; it's used to figure out the computation/accuracy
  // tradeoff from subdividing or not subdividing.  It is estimated from the
  // set of 8 densities each time we subdivide.
  double density_deriv_norm;

  // `integral` is the contribution of this cube to the integral we are
  // computing.
  double integral;

  // `integral_error >= 0` is the error in the integral we are computing,
  // i.e. the amount by which `integral` could differ from the real value,
  // based on the deriv_norm.
  double integral_error;

  // `integral_diff` is the difference between the parent 'integral'
  // divided by 8, and this integral.  This is used for diagnostics but not for
  // deciding when to stop splitting.
  double integral_diff;

  // we'll set this to true if we'll subdivide this piece of integral.
  bool will_subdivide;
};

inline std::ostream &operator << (std::ostream &os, const IntegralPart &i) {
  os << "{ volume=" << i.volume << ", density=" << i.density
     << ", density_deriv_norm=" << i.density_deriv_norm
     << ", integral=" << i.integral
     << ", integral-error=" << i.integral_error
     << ", integral-diff=" << i.integral_diff
     << ", will-subdivide=" << i.will_subdivide << "}\n";
  return os;
}


/*
  Return a sign that's used in subdividing a cube into 8 pieces.
     @param [in] n    0 <= n < 8, this dictates which of the 8 pieces
                      we want.
     @param [in] dim  0 <= dim < 3, this says which of 3 dimensions of
                      space we are talking about
     @return        Returns a sign, 1 or -1, which says whether, in
                    this dimension, this sub-cube has a more positive (+1)
                    or more negative (-1) co-ordinate.    The idea is
                    to have all combinations of signs present when
                    called for n = 0 through 7.
 */
__host__ __device__ __forceinline__ int GetSign(int n, int dim) {
  // return (n & (1 << dim)) != 0 ? 1 : -1
  // the following is an optimization of the expression above which is intended
  // to avoid conditionals.
  return (((n & (1 << dim)) != 0) * 2) - 1;
}


/*
  Compute the `density_deriv_norm` elements of `dest`, which will be used to
  estimate error bars on the integral
     @param [in] src  The higher-level (enclosing cub)
     @param [in,out] dest   One of the 8 sub-cubes of `src`, which is expected
                      to have its `volume` and `density` members
                      already set.  It is expected to point to the n'th
                      element of an array of 8 sub-cubes of `src`; in fact
                      there will be one large array whose dimension is
                      a multiple of 8 but for the purposes of this function
                      we are only concerned with the 8 sub-cubes of `src`.
                      This function sets dest->density_deriv_norm.
 */
__device__ void SetDerivNorm(const IntegralPart &src,
                             IntegralPart *dest,
                             int n) {
  __shared__ double deriv_sq[3];  // contains the squares of elements of the
                                  // derivative.
  __shared__ double deriv_norm;

  __syncthreads();
  if (n < 3) {
    int32_t mask = 1 << n;
    double tot = 0.0;
#pragma unroll
    for (int32_t i = 0; i < 8; i++) {
      int sign = ((i & mask != 0) * 2) - 1;  // == (i & mask) ? 1 : -1
      tot += dest[i - n].density * sign;
    }
    // There are 4 pairs of points, get the average.
    tot *= (1.0 / 4.0);
    // Each pair of points is separated by a distance of 2.0 * r where r is the
    // distance from the center to edge of one of the 8 cubes.
    // Note: all these r's are the same, we might as well have all the threads
    // use the same one.  (Note: `dest` points to th en'th element.
    tot /= (2.0 * dest[0 - n].volume.r);
    deriv_sq[n] = tot * tot;
  }
  __syncthreads();
  if (n == 0) {
    deriv_norm = sqrt(deriv_sq[0] + deriv_sq[1] + deriv_sq[2]);
  }
  __syncthreads();
  // We actually interpolate with the parent's deriv_norm, because we want to
  // avoid it approaching exactly zero too quickly.
  dest->density_deriv_norm = (0.8 * deriv_norm + 0.2 * src.density_deriv_norm);
  // To avoid estimating close to zero derivative if the center is at a point, we
  // add this extra term...
  dest->density_deriv_norm += 0.2 * (abs(src.density - dest->density) / dest->volume.r);
}


// Create a volume representing one of the 8 sub-cubes of `src`.
// Require 0 <= n < 8.
// This only computes the 'volume' and 'density' elements.
__device__ void SubdivideIntegral(const Configuration configuration,
                                  const IntegralPart &src,
                                  IntegralPart *dest,
                                  int n) {
  dest->volume.r = src.volume.r * 0.5;
#pragma unroll
  for (int d = 0; d < 3; d++)
    dest->volume.center.x[d] = src.volume.center.x[d] +
                               dest->volume.r * GetSign(n, d);
  dest->density = ComputeDensity(configuration,
                                 dest->volume.center);

  SetDerivNorm(src, dest, n);

  double volume = pow(2.0 * dest->volume.r, 3.0);
  dest->integral = dest->density * volume;
  dest->integral_error = (dest->density_deriv_norm * dest->volume.r) * volume;
  dest->integral_diff = dest->integral - (src.integral / 8.0);
}


double ComputeIntegral(ContextPtr &c,
                       const Configuration configuration,
                       double r,  // radius of cube about origin to integrate over
                       double cutoff,  // e.g. 1.0e-06, as a cutoff of error in
                                       // integral per cube,determines how fine
                                       // mesh we use
                       double *integral_error,
                       double *integral_diff,
                       double *abs_integral_diff) {
  K2_CHECK(c->GetDeviceType() == kCuda);  // these kernels are GPU-only.

  K2_LOG(INFO) << "Configuration is " << configuration;

  IntegralPart part;
  SetZero(&part.volume.center);
  part.volume.r = r;
  part.density = 10.0;
  part.density_deriv_norm = 10.0;
  part.will_subdivide = true;


  // formerly we only had one of these arrays at a time, but for memory
  // reasons we now break them up..
  std::vector<std::unique_ptr<Array1<IntegralPart> > > parts_vec;
  parts_vec.push_back(std::make_unique<Array1<IntegralPart> >(c, 1));
  *(parts_vec.back()) = part;   // set that one element to `part`

  double tot_integral = 0.0,
      tot_integral_error = 0.0,
      tot_integral_diff = 0.0,
      tot_abs_integral_diff = 0.0;
  for (int32_t iter =  0; !parts_vec.empty(); ++iter) {
    std::unique_ptr<Array1<IntegralPart> > parts;
    parts.swap(parts_vec.back());
    parts_vec.pop_back();
    const int32_t size_limit = (1 << 17);
    if (parts->Dim() > size_limit) {
      int32_t num_pieces = 0;
      for (int32_t offset = 0; offset < parts->Dim(); offset += size_limit, num_pieces++) {
        int32_t block_size = std::min<int32_t>(size_limit, parts->Dim() - offset);
        parts_vec.push_back(std::make_unique<Array1<IntegralPart> >(
            parts->Range(offset, block_size)));
      }
      continue;
    }

    int32_t num_parts = parts->Dim();

    // This renumbering will count only those parts to be subdivided.
    Renumbering renumbering(c, parts->Dim());
    char *keep_data = renumbering.Keep().Data();

    // we'll use integral_vec this with ExclusiveSum (since we have no plain
    // Sum() in k2 right now), to figure out the sum of the non-subdivided parts
    // of `parts`.  This will contain zeros for the parts to be subdivided.
    Array1<double> integral_vec(c, num_parts + 1),
        integral_error_vec(c, num_parts + 1),
        integral_diff_vec(c, num_parts + 1),
        abs_integral_diff_vec(c, num_parts + 1);
    double *integral_vec_data = integral_vec.Data(),
        *integral_error_vec_data = integral_error_vec.Data(),
        *integral_diff_vec_data = integral_diff_vec.Data(),
        *abs_integral_diff_vec_data = abs_integral_diff_vec.Data();
    const IntegralPart *parts_data = parts->Data();
    auto lambda_set_keep_and_integral = [=] __host__ __device__ (int32_t i) -> void {
      if (!(keep_data[i] = (parts_data[i].will_subdivide == true))) {
        integral_vec_data[i] = parts_data[i].integral;
        integral_error_vec_data[i] = parts_data[i].integral_error;
        integral_diff_vec_data[i] = parts_data[i].integral_diff;
        abs_integral_diff_vec_data[i] = abs(parts_data[i].integral_diff);
      } else {
        integral_vec_data[i] = 0.0;
        integral_error_vec_data[i] = 0.0;
        integral_diff_vec_data[i] = 0.0;
        abs_integral_diff_vec_data[i] = 0.0;
      }
    };
    Eval(c, num_parts, lambda_set_keep_and_integral);
    ExclusiveSum(integral_vec, &integral_vec);
    ExclusiveSum(integral_error_vec, &integral_error_vec);
    ExclusiveSum(integral_diff_vec, &integral_diff_vec);
    ExclusiveSum(abs_integral_diff_vec, &abs_integral_diff_vec);
    //K2_LOG(INFO) << "Last elem of parts is " << parts->Back();
    tot_integral += integral_vec[num_parts];
    tot_integral_error += integral_error_vec[num_parts];
    tot_integral_diff += integral_diff_vec[num_parts];
    tot_abs_integral_diff += abs_integral_diff_vec[num_parts];

    int32_t num_subdivided = renumbering.NumNewElems();
    K2_CHECK_LE(num_subdivided, num_parts);
    if (iter % 5000 == 0) {
      K2_LOG(INFO) << "after iter " << iter << ", tot_integral = "
                   << tot_integral << ", (error,diff,abs-diff) = "
                   << tot_integral_error << ","
                   << tot_integral_diff << "," << tot_abs_integral_diff
                   <<", num-parts=" << num_parts << ", num-kept = "
                   << num_subdivided << ", parts_vec.size()="
                   << parts_vec.size();
    }
    if (num_subdivided == 0) continue;

    int32_t new_num_parts = 8 * num_subdivided;
    K2_CHECK(new_num_parts > num_subdivided);
    int32_t *new2old_data = renumbering.New2Old().Data();
    Array1<IntegralPart> new_parts(c, new_num_parts);
    IntegralPart *new_parts_data = new_parts.Data();

    auto lambda_set_new_parts = [=] __device__ (int32_t i) -> void {
      int32_t n = i % 8,
        group_i = i / 8,
        old_i = new2old_data[group_i];
      K2_CHECK_LE(static_cast<uint32_t>(group_i),
                  static_cast<uint32_t>(num_subdivided));
      K2_CHECK_LE(static_cast<uint32_t>(old_i),
                  static_cast<uint32_t>(num_parts));
      const IntegralPart *old_part = parts_data + old_i;
      IntegralPart *this_part = new_parts_data + i;
      SubdivideIntegral(configuration,
                        *old_part, this_part, n);
      new_parts_data[i].will_subdivide =
         (new_parts_data[i].integral_error >= cutoff);
    };
    EvalDevice(c, new_num_parts, lambda_set_new_parts);
    parts_vec.push_back(std::make_unique<Array1<IntegralPart> >(new_parts));
  }
  *integral_error = tot_integral_error;
  *integral_diff = tot_integral_diff;
  *abs_integral_diff = tot_abs_integral_diff;
  return tot_integral;
}

} // namespace k2
#endif  //  K2_CSRC_FSA_INTEGRAL_H_
