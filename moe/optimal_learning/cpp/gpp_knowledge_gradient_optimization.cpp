/*!
  \file gpp_knowledge_gradient_optimization.cpp
  \rst
\endrst*/


#include "gpp_knowledge_gradient_optimization.hpp"

#include <cmath>

#include <memory>

#include "gpp_common.hpp"
#include "gpp_domain.hpp"
#include "gpp_logging.hpp"
#include "gpp_math.hpp"
#include "gpp_optimizer_parameters.hpp"

namespace optimal_learning {

KnowledgeGradientEvaluator::KnowledgeGradientEvaluator(const GaussianProcess& gaussian_process_in,
                                                       double const * discrete_pts,
                                                       int num_pts,
                                                       int num_mc_iterations,
                                                       double const noise,
                                                       double best_so_far)
    : dim_(gaussian_process_in.dim()),
      num_mc_iterations_(num_mc_iterations),
      best_so_far_(best_so_far),
      gaussian_process_(&gaussian_process_in),
      noise_(noise),
      num_pts_(num_pts)
      to_sample_mean_(mean_value_discrete(discrete_pts, num_pts)){
      std::copy(discrete_pts, discrete_pts + num_pts, discrete_pts_.data());
}

/*!\rst
  Compute Knowledge Gradient
\endrst*/
double KnowledgeGradientEvaluator::ComputeKnowledgeGradient(StateType * kg_state) const {
  int num_union = kg_state->num_union;
  gaussian_process_->ComputeVarianceOfPoints(&(kg_state->points_to_sample_state), kg_state->cholesky_to_sample_var.data());
  //Adding the variance of measurement noise to the covariance matrix
  for (int i=0;i<num_union;++i){
     kg_state->cholesky_to_sample_var.data()[i*(num_union+1)]+=noise_;
  }
  int leading_minor_index = ComputeCholeskyFactorL(num_union, kg_state->cholesky_to_sample_var.data());
  if (unlikely(leading_minor_index != 0)) {
    OL_THROW_EXCEPTION(SingularMatrixException,
    "GP-Variance matrix singular. Check for duplicate points_to_sample/being_sampled or points_to_sample/being_sampled duplicating points_sampled with 0 noise.",
    kg_state->cholesky_to_sample_var.data(), num_union, leading_minor_index);
  }

  gaussian_process_->ComputeCovarianceOfPoints(kg_state->points_to_sample_state,
                                               discrete_pts_.data(), num_pts_,
                                               kg_state->covariance_union_discrete.data());

  std::copy(kg_state->covariance_union_discrete.data(), kg_state->covariance_union_discrete.data()+num_union*num_pts_,
            kg_state->inverse_cholesky_covariance.data());
  TriangularMatrixMatrixSolve(kg_state->cholesky_to_sample_var.data(), 'N',
                              num_union, num_pts_, num_union,
                              kg_state->inverse_cholesky_covariance.data());

  double aggregate = 0.0;
  for (int i = 0; i < num_mc_iterations_; ++i) {
    double improvement_this_step = 0.0;
    double norm = new double[num_union];
    for (int j = 0; j < num_union; ++j) {
      norm[j] = (*(kg_state->normal_rng))();
    }

    // compute KG_this_step_from_far = cholesky * normals   as  KG = cholesky * normal
    // b/c normals currently held in KG_this_step_from_var
    GeneralMatrixVectorMultiply(kg_state->inverse_cholesky_covariance.data(), 'T',
                                norm,
                                1,0,
                                num_union,
                                num_pts_,
                                num_union,
                                kg_state->KG_this_step_from_var);
    delete[] norm;
    for (int j = 0; j < num_pts_; ++j) {
      double KG_total = best_so_far_ - (to_sample_mean_[j] + kg_state->KG_this_step_from_var[j]);
      if (KG_total > improvement_this_step) {
        improvement_this_step = KG_total;
      }
    }

    aggregate += improvement_this_step;
  }

  return aggregate/static_cast<double>(num_mc_iterations_);
}

/*!\rst
  Computes gradient of KG (see KnowledgeGradientEvaluator::ComputeGradKnowledgeGradient) wrt points_to_sample (stored in
  ``union_of_points[0:num_to_sample]``).

  Mechanism is similar to the computation of KG, where points' contributions to the gradient are thrown out of their
  corresponding ``improvement <= 0.0``.

  Thus ``\nabla(\mu)`` only contributes when the ``winner`` (point w/best improvement this iteration) is the current point.
  That is, the gradient of ``\mu`` at ``x_i`` wrt ``x_j`` is 0 unless ``i == j`` (and only this result is stored in
  ``kg_state->grad_mu``).  The interaction with ``kg_state->grad_chol_decomp`` is harder to know a priori (like with
  ``grad_mu``) and has a more complex structure (rank 3 tensor), so the derivative wrt ``x_j`` is computed fully, and
  the relevant submatrix (indexed by the current ``winner``) is accessed each iteration.

  .. Note:: comments here are copied to _compute_grad_expected_improvement_monte_carlo() in python_version/expected_improvement.py
\endrst*/
void KnowledgeGradientEvaluator::ComputeGradKnowledgeGradient(StateType * kg_state, double * restrict grad_KG) const {
  const int num_union = kg_state->num_union;
  gaussian_process_->ComputeVarianceOfPoints(&(kg_state->points_to_sample_state), kg_state->cholesky_to_sample_var.data());
  //Adding the variance of measurement noise to the covariance matrix
  for (int i=0;i<num_union;++i){
     kg_state->cholesky_to_sample_var.data()[i*(num_union+1)]+=noise_;
  }
  int leading_minor_index = ComputeCholeskyFactorL(num_union, kg_state->cholesky_to_sample_var.data());
  if (unlikely(leading_minor_index != 0)) {
    OL_THROW_EXCEPTION(SingularMatrixException,
    "GP-Variance matrix singular. Check for duplicate points_to_sample/being_sampled or points_to_sample/being_sampled duplicating points_sampled with 0 noise.",
    kg_state->cholesky_to_sample_var.data(), num_union, leading_minor_index);
  }

  gaussian_process_->ComputeGradCovarianceOfPoints(&(kg_state->points_to_sample_state),
                                                   discrete_pts_.data(),
                                                   num_pts_,
                                                   kg_state->grad_covariance.data());

  gaussian_process_->ComputeGradCholeskyVarianceOfPoints(&(kg_state->points_to_sample_state),
                                                         kg_state->cholesky_to_sample_var.data(),
                                                         kg_state->grad_chol_decomp.data());

  // let L_{d,i,j,k} = grad_chol_decomp, d over dim_, i, j over num_union, k over num_to_sample
  // we want to compute: agg_dx_{d,*,*,k} = (L_{d,*,*,k}^{-1} * L_{d,*,*,k} * L_{d,*,*,k}^{-1})^T
  // TODO(GH-92): Form this as one GeneralMatrixVectorMultiply() call by storing data as L_{d,i,k,j} if it's faster.
  for (int k = 0; k < kg_state->num_to_sample; ++k) {
      for (int i = 0; i < dim_; ++i) {
         double* temp= new double[num_union*num_union];
         for (int j = 0; j < num_union; ++j){
            for (int l = 0; l < num_union; ++l){
               temp[j+l*num_union] = kg_state->grad_chol_decomp.data()[i + j*dim_ + l*dim_*num_union + k*dim_*num_union*num_union];
            }
         }
         TriangularMatrixMatrixSolve(kg_state->cholesky_to_sample_var.data(),'N',num_union,num_union,num_union,temp);
         MatrixTranspose(temp, num_union, num_union, temp);
         TriangularMatrixMatrixSolve(kg_state->cholesky_to_sample_var.data(),'T',num_union,num_union,num_union,temp);
         for (int j = 0; j < num_union; ++j){
            for (int l = 0; l < num_union; ++l){
               kg_state->grad_chol_decomp.data()[i + j*dim_ + l*dim_*num_union + k*dim_*num_union*num_union] = temp[j+l*num_union];
            }
         }
         delete[] temp;
      }
  }

  gaussian_process_->ComputeCovarianceOfPoints(kg_state->points_to_sample_state,
                                               discrete_pts_.data(), num_pts_,
                                               kg_state->covariance_union_discrete.data());
  std::copy(kg_state->covariance_union_discrete.data(), kg_state->covariance_union_discrete.data()+num_union*num_pts_,
            kg_state->inverse_cholesky_covariance.data());
  TriangularMatrixMatrixSolve(kg_state->cholesky_to_sample_var.data(), 'N',
                              num_union, num_pts_, num_union,
                              kg_state->inverse_cholesky_covariance.data());

  std::fill(kg_state->aggregate.begin(), kg_state->aggregate.end(), 0.0);

  for (int i = 0; i < num_mc_iterations_; ++i) {
    double norm = new double[num_union];
    for (int j = 0; j < num_union; ++j) {
      norm[j] = (*(kg_state->normal_rng))();
    }
    // compute KG_this_step_from_far = cholesky * normals   as  KG = inverse_cholesky_covariance^T * normal
    GeneralMatrixVectorMultiply(kg_state->inverse_cholesky_covariance.data(), 'T',
                                norm,
                                1,0,
                                num_union,
                                num_pts_,
                                num_union,
                                kg_state->KG_this_step_from_var);
    delete[] norm;
    double improvement_this_step = 0.0;
    int winner = num_pts_ + 1;  // an out of-bounds initial value
    for (int j = 0; j < num_pts_; ++j) {
      double KG_total = best_so_far_ - (to_sample_mean_[j] + kg_state->KG_this_step_from_var[j]);
      if (KG_total > improvement_this_step) {
        improvement_this_step = KG_total;
        winner = j;
      }
    }

    // let L_{d,i,j,k} = grad_chol_decomp, d over dim_, i, j over num_union, k over num_to_sample
    // we want to compute: agg_dx_{d,k} = L_{d,i,j=winner,k} * normals_i
    // TODO(GH-92): Form this as one GeneralMatrixVectorMultiply() call by storing data as L_{d,i,k,j} if it's faster.
    for (int k = 0; k < kg_state->num_to_sample; ++k) {
      for (int j = 0; j < dim_; ++j) {
         TriangularMatrixInverse(kg_state->cholesky_to_sample_var.data(),
                                 num_union,
                                 kg_state->cholesky_to_sample_var.data());
         MatrixTranspose(kg_state->cholesky_to_sample_var.data(),
                         num_union, num_union,
                         kg_state->cholesky_to_sample_var.data());
         double *grad_cov = new double[num_union];
         for (int l = 0; l < num_union; ++l){
            grad_cov[l] = kg_state->grad_covariance[j + l*dim_+ winner*dim_*num_union + k*dim_*num_union*num_pts_];
         }
         GeneralMatrixMatrixMultiply(grad_cov, 'N', kg_state->cholesky_to_sample_var.data(),
                                     1,0,
                                     1,num_uion,num_union,
                                     grad_cov);
         double *grad_chol=new double[num_union*num_union];
         double *cov = new double[num_union];
         for (int l = 0; l < num_union; ++l){
           for (int m = 0; m < num_union; ++m){
             grad_chol[l+m*num_union] = kg_state->grad_chol_decomp.data()[j + l*dim_ + m*dim_*num_union + k*dim_*num_union*num_union];
           }
         }
         for (int l = 0; l < num_union; ++l){
           cov[l] = kg_state->covariance_union_discrete[j + l*dim_+ winner*dim_*num_union + k*dim_*num_union*num_pts_];
         }
         GeneralMatrixMatrixMultiply(cov, 'N', grad_chol,
                                     1,1,
                                     1,num_uion,num_union,
                                     grad_cov);
         GeneralMatrixVectorMultiply(grad_cov, 'N', kg_state->normals.data(), -1.0, 1.0,
                                     1, num_union, 1, kg_state->aggregate.data() + j + k*dim_);
         delete[] cov;
         delete[] grad_chol;
         delete[] grad_cov;
      }
    }
  }  // end for i: num_mc_iterations_

  for (int k = 0; k < kg_state->num_to_sample*dim_; ++k) {
    grad_KG[k] = kg_state->aggregate[k]/static_cast<double>(num_mc_iterations_);
  }
}

void KnowledgeGradientState::SetCurrentPoint(const EvaluatorType& kg_evaluator,
                          double const * restrict points_to_sample) {
  // update points_to_sample in union_of_points
  std::copy(points_to_sample, points_to_sample + num_to_sample*dim, union_of_points.data());

  // evaluate derived quantities for the GP
  points_to_sample_state.SetupState(*kg_evaluator.gaussian_process(), union_of_points.data(),
                                    num_union, num_derivatives);
}

KnowledgeGradientState::KnowledgeGradientState(const EvaluatorType& kg_evaluator,
                                               double const * restrict points_to_sample,
                                               double const * restrict points_being_sampled,
                                               int num_to_sample_in, int num_being_sampled_in, int num_pts_in,
                                               bool configure_for_gradients, NormalRNGInterface * normal_rng_in)
    : dim(kg_evaluator.dim()),
      num_to_sample(num_to_sample_in),
      num_being_sampled(num_being_sampled_in),
      num_pts(num_pts_in),
      num_derivatives(configure_for_gradients ? num_to_sample : 0),
      num_union(num_to_sample + num_being_sampled),
      union_of_points(BuildUnionOfPoints(points_to_sample, points_being_sampled, num_to_sample, num_being_sampled, dim)),
      points_to_sample_state(*kg_evaluator.gaussian_process(), union_of_points.data(), num_union, num_derivatives),
      normal_rng(normal_rng_in),
      cholesky_to_sample_var(Square(num_union)),
      covariance_union_discrete(num_union*num_pts),
      inverse_cholesky_covariance(num_union*num_pts),
      grad_chol_decomp(dim*Square(num_union)*num_derivatives),
      grad_covariance(dim*num_union*num_pts*num_derivatives),
      KG_this_step_from_var(num_pts),
      aggregate(dim*num_derivatives),
      normals(num_union) {
}

KnowledgeGradientState::KnowledgeGradientState(KnowledgeGradientState&& OL_UNUSED(other)) = default;

void KnowledgeGradientState::SetupState(const EvaluatorType& kg_evaluator,
                                        double const * restrict points_to_sample) {
  if (unlikely(dim != kg_evaluator.dim())) {
    OL_THROW_EXCEPTION(InvalidValueException<int>, "Evaluator's and State's dim do not match!", dim, kg_evaluator.dim());
  }

  // update quantities derived from points_to_sample
  SetCurrentPoint(kg_evaluator, points_to_sample);
}


/*!\rst
  Routes the KG computation through MultistartOptimizer + NullOptimizer to perform KG function evaluations at the list of input
  points, using the appropriate KG evaluator (e.g., monte carlo vs analytic) depending on inputs.
\endrst*/
void EvaluateKGAtPointList(const GaussianProcess& gaussian_process, const ThreadSchedule& thread_schedule,
                           double const * restrict initial_guesses, double const * restrict points_being_sampled,
                           double const * restrict discrete_pts, int num_multistarts, int num_to_sample,
                           int num_being_sampled, int num_pts, double best_so_far,
                           int max_int_steps, bool * restrict found_flag, NormalRNG * normal_rng,
                           double * restrict function_values, double * restrict best_next_point) {
  if (unlikely(num_multistarts <= 0)) {
    OL_THROW_EXCEPTION(LowerBoundException<int>, "num_multistarts must be > 1", num_multistarts, 1);
  }

  using DomainType = DummyDomain;
  DomainType dummy_domain;
  bool configure_for_gradients = false;

  KnowledgeGradientEvaluator kg_evaluator(gaussian_process, discrete_pts, num_pts, max_int_steps, noise, best_so_far);

  std::vector<typename KnowledgeGradientEvaluator::StateType> kg_state_vector;
  SetupKnowledgeGradientState(kg_evaluator, start_point_set, points_being_sampled,
                              num_to_sample, num_being_sampled, num_pts, thread_schedule.max_num_threads,
                              configure_for_gradients, normal_rng, &kg_state_vector);

  // init winner to be first point in set and 'force' its value to be 0.0; we cannot do worse than this
  OptimizationIOContainer io_container(kg_state_vector[0].GetProblemSize(), 0.0, initial_guesses);

  NullOptimizer<KnowledgeGradientEvaluator, DomainType> null_opt;
  typename NullOptimizer<KnowledgeGradientEvaluator, DomainType>::ParameterStruct null_parameters;
  MultistartOptimizer<NullOptimizer<KnowledgeGradientEvaluator, DomainType> > multistart_optimizer;
  multistart_optimizer.MultistartOptimize(null_opt, kg_evaluator, null_parameters,
                                          dummy_domain, thread_schedule, initial_guesses,
                                          num_multistarts, kg_state_vector.data(), function_values, &io_container);
  *found_flag = io_container.found_flag;
  std::copy(io_container.best_point.begin(), io_container.best_point.end(), best_next_point);
}

/*!\rst
  This is a simple wrapper around ComputeKGOptimalPointsToSampleWithRandomStarts() and
  ComputeKGOptimalPointsToSampleViaLatinHypercubeSearch(). That is, this method attempts multistart gradient descent
  and falls back to latin hypercube search if gradient descent fails (or is not desired).
\endrst*/
template <typename DomainType>
void ComputeKGOptimalPointsToSample(const GaussianProcess& gaussian_process,
                                    const GradientDescentParameters& optimizer_parameters,
                                    const DomainType& domain, const ThreadSchedule& thread_schedule,
                                    double const * restrict points_being_sampled,
                                    double const * restrict discrete_pts,
                                    int num_to_sample, int num_being_sampled,
                                    int num_pts, double best_so_far,
                                    int max_int_steps, bool lhc_search_only,
                                    int num_lhc_samples, bool * restrict found_flag,
                                    UniformRandomGenerator * uniform_generator,
                                    NormalRNG * normal_rng, double * restrict best_points_to_sample) {
  if (unlikely(num_to_sample <= 0)) {
    return;
  }

  std::vector<double> next_points_to_sample(gaussian_process.dim()*num_to_sample);

  bool found_flag_local = false;
  if (lhc_search_only == false) {
    ComputeOptimalPointsToSampleWithRandomStarts(gaussian_process, optimizer_parameters,
                                                 domain, thread_schedule, points_being_sampled, discrete_pts,
                                                 num_to_sample, num_being_sampled, num_pts,
                                                 best_so_far, max_int_steps,
                                                 &found_flag_local, uniform_generator, normal_rng,
                                                 next_points_to_sample.data());
  }

  // if gradient descent EI optimization failed OR we're only doing latin hypercube searches
  if (found_flag_local == false || lhc_search_only == true) {
    if (unlikely(lhc_search_only == false)) {
      OL_WARNING_PRINTF("WARNING: %d,%d-EI opt DID NOT CONVERGE\n", num_to_sample, num_being_sampled);
      OL_WARNING_PRINTF("Attempting latin hypercube search\n");
    }

    if (num_lhc_samples > 0) {
      // Note: using a schedule different than "static" may lead to flakiness in monte-carlo KG optimization tests.
      // Besides, this is the fastest setting.
      ThreadSchedule thread_schedule_naive_search(thread_schedule);
      thread_schedule_naive_search.schedule = omp_sched_static;
      ComputeKGOptimalPointsToSampleViaLatinHypercubeSearch(gaussian_process, domain,
                                                          thread_schedule_naive_search,
                                                          points_being_sampled, discrete_pts,
                                                          num_lhc_samples, num_to_sample,
                                                          num_being_sampled, num_pts, best_so_far,
                                                          max_int_steps,
                                                          &found_flag_local, uniform_generator,
                                                          normal_rng, next_points_to_sample.data());

      // if latin hypercube 'dumb' search failed
      if (unlikely(found_flag_local == false)) {
        OL_ERROR_PRINTF("ERROR: %d,%d-EI latin hypercube search FAILED on\n", num_to_sample, num_being_sampled);
      }
    } else {
      OL_WARNING_PRINTF("num_lhc_samples <= 0. Skipping latin hypercube search\n");
    }
  }

  // set outputs
  *found_flag = found_flag_local;
  std::copy(next_points_to_sample.begin(), next_points_to_sample.end(), best_points_to_sample);
}

// template explicit instantiation definitions, see gpp_common.hpp header comments, item 6
extern template void ComputeKGOptimalPointsToSample(
    const GaussianProcess& gaussian_process, const GradientDescentParameters& optimizer_parameters,
    const TensorProductDomain& domain, const ThreadSchedule& thread_schedule,
    double const * restrict points_being_sampled, double const * restrict discrete_pts,
    int num_to_sample, int num_being_sampled,
    int num_pts, double best_so_far, int max_int_steps, bool lhc_search_only,
    int num_lhc_samples, bool * restrict found_flag, UniformRandomGenerator * uniform_generator,
    NormalRNG * normal_rng, double * restrict best_points_to_sample);
extern template void ComputeKGOptimalPointsToSample(
    const GaussianProcess& gaussian_process, const GradientDescentParameters& optimizer_parameters,
    const SimplexIntersectTensorProductDomain& domain, const ThreadSchedule& thread_schedule,
    double const * restrict points_being_sampled,double const * restrict discrete_pts,
    int num_to_sample, int num_being_sampled,
    int num_pts, double best_so_far, int max_int_steps, bool lhc_search_only, double best_so_far, int max_int_steps,
    bool lhc_search_only, int num_lhc_samples, bool * restrict found_flag,
    UniformRandomGenerator * uniform_generator, NormalRNG * normal_rng, double * restrict best_points_to_sample);

}  // end namespace optimal_learning
