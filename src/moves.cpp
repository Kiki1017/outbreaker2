#include <Rcpp.h>
#include <Rmath.h>
#include "internals.h"
#include "likelihoods.h"



// IMPORTANT: ON INDEXING VECTORS AND ANCESTRIES

// Most of the functions implemented here are susceptible to be called from R
// via Rcpp, and are therefore treated as interfaces. This causes a number of
// headaches when using indices of cases defined in R (1:N) to refer to elements
// in Rcpp / Cpp vectors (0:N-1). By convention, we store all data on the
// original scale (1:N), and modify indices whenever accessing elements of
// vectors. In other words, in an expression like 'alpha[j]', 'j' should always
// be on the internal scale (0:N-1).

// In all these functions, 'SEXP i' is an optional vector of case indices, on
// the 1:N scale.





// ---------------------------

// Movement of the mutation rate 'mu' is done using a dumb normal proposal. This
// is satisfying for now - we only reject a few non-sensical values outside the
// range [0;1]. The SD of the proposal (implicitely contained in rand$mu.rnorm1,
// but really provided through 'config', seems fine as the range of real values
// will never change much. Probably not much point in using auto-tuning here.

// [[Rcpp::export("cpp.move.mu", rng = true)]]
Rcpp::List cpp_move_mu(Rcpp::List data, Rcpp::List param, Rcpp::List config) {
  Rcpp::List new_param = clone(param); // deep copy here for now, ultimately should be an arg.
  Rcpp::NumericVector mu = param["mu"];
  Rcpp::NumericVector new_mu = new_param["mu"];

  double sd_mu = static_cast<double>(config["sd.mu"]);

  double old_loglike = 0.0, new_loglike = 0.0, p_accept = 0.0;

  // loglike with current value
  old_loglike = cpp_ll_genetic(data, param, R_NilValue);

  // proposal (normal distribution with SD: config$sd.mu)
  new_mu[0] += R::rnorm(0, sd_mu); // new proposed value


  // loglike with current value
  new_param["mu"] = new_mu;
  new_loglike = cpp_ll_genetic(data, new_param, R_NilValue);

  // acceptance term
  p_accept = exp(new_loglike - old_loglike);

  // acceptance: the new value is already in mu, so we only act if the move is rejected, in
  // which case we restore the previous ('old') value
  if (p_accept < unif_rand()) { // reject new values
    new_mu[0] = mu[0];
    new_param["mu"] = new_mu;
  }

  return new_param;
}





// ---------------------------

// Movement of infection dates are +/- 1 from current states. These movements
// are currently vectorised, i.e. a bunch of dates are proposed all together;
// this may not be sustainable for larger datasets. The non-vectorised option
// will be slower and speed-up with C/C++ will be more substantial then.

// This version differs from the initial R implementation in several points:
// 1. all cases are moved
// 2. cases are moved one by one
// 3. movement for each case is +/- 1 time unit

// Notes

// - when computing the timing log-likelihood, the descendents of each case are
// also affected.

// - we generate a new vector 'new_t_inf', which will replace the previous
// pointer defining param["t.inf"].

// [[Rcpp::export("cpp.move.t.inf", rng = true)]]
Rcpp::List cpp_move_t_inf(Rcpp::List data, Rcpp::List param) {
  Rcpp::List new_param = clone(param); // deep copy here for now, ultimately should be an arg.
  Rcpp::NumericVector t_inf = param["t.inf"];
  Rcpp::NumericVector new_t_inf = new_param["t.inf"];

  size_t N = static_cast<size_t>(data["N"]);

  size_t i = 0;

  double old_loglike = 0.0, new_loglike = 0.0, p_accept = 0.0;


  for (i = 0; i < N; i++) {
    // loglike with current value
    // old_loglike = cpp_ll_timing(data, param, i); // term for case 'i'
    // old_loglike = cpp_ll_timing(data, param, find_descendents(i)); // term descendents of 'i'
    old_loglike = cpp_ll_timing(data, param, R_NilValue);

    // proposal (+/- 1)
    new_t_inf[i] += unif_rand() > 0.5 ? 1 : -1; // new proposed value

    // loglike with current value
    // new_loglike = cpp_ll_timing(data, param, i); // term for case 'i'
    // new_loglike = cpp_ll_timing(data, param, find_descendents(i)); // term descendents of 'i'
    new_param["t.inf"] = new_t_inf;
    new_loglike = cpp_ll_timing(data, new_param, R_NilValue);

    // acceptance term
    p_accept = exp(new_loglike - old_loglike);

    // acceptance: the new value is already in t_inf, so we only act if the move is rejected, in
    // which case we restore the previous ('old') value
    if (p_accept < unif_rand()) { // reject new values
      new_t_inf[i] = t_inf[i];
      new_param["t.inf"] = t_inf;
    }
  }

  return new_param;

}






// ---------------------------

// Movement of ancestries ('alpha') is not vectorised, movements are made one
// case at a time. This procedure is simply about picking an infector at
// random amongst cases preceeding the case considered. The original version
// in 'outbreaker' used to move simultaneously 'alpha', 'kappa' and 't.inf',
// but current implementation is simpler and seems to mix at least as
// well. Proper movement of 'alpha' needs this procedure as well as a swapping
// procedure (swaps are not possible through move.alpha only).

// [[Rcpp::export("cpp.move.alpha", rng = true)]]
Rcpp::List cpp_move_alpha(Rcpp::List data, Rcpp::List param) {
  Rcpp::List new_param = clone(param);
  Rcpp::IntegerVector alpha = param["alpha"]; // pointer to param$t_inf
  Rcpp::IntegerVector t_inf = param["t.inf"]; // pointer to param$t_inf
  Rcpp::IntegerVector new_alpha = new_param["alpha"];

  size_t N = static_cast<size_t>(data["N"]);

  size_t i = 0;

  double old_loglike = 0.0, new_loglike = 0.0, p_accept = 0.0;
  double old_loglike2 = 0.0, new_loglike2 = 0.0, p_accept2 = 0.0;

  for (i = 0; i < N; i++) {

    // only non-NA ancestries are moved, if there is at least 1 choice
    if (alpha[i] != NA_INTEGER && sum(t_inf < t_inf[i]) > 0) {

      // loglike with current value
      old_loglike = cpp_ll_all(data, param, R_NilValue);
      old_loglike2 = cpp_ll_all(data, param, i+1);

      // proposal (+/- 1)
      new_alpha[i] = cpp_pick_possible_ancestor(t_inf, i); // new proposed value (on scale 1 ... N)

      // loglike with current value
      new_param["alpha"] = new_alpha;
      new_loglike = cpp_ll_all(data, new_param, R_NilValue);
      new_loglike2 = cpp_ll_all(data, new_param, i+1);

      // acceptance term
      p_accept = exp(new_loglike - old_loglike);
      p_accept2 = exp(new_loglike2 - old_loglike2);
      std::vector<int> calpha = Rcpp::as<std::vector<int> >(alpha);
      // Rcpp::Rcout << "\ni: " << i << " old1: " << old_loglike << "  new1: " << new_loglike << "  ratio1: " << accept_ratio << std::endl;
      // Rcpp::Rcout << "\ni: " << i << " old2: " << old_loglike2 << "  new2: " << new_loglike2 << "  ratio2: " << accept_ratio2 << std::endl;

      // acceptance: the new value is already in alpha, so we only act if the move is rejected, in
      // which case we restore the previous ('old') value
      if (p_accept < unif_rand()) { // reject new values
	new_alpha[i] = alpha[i];
	new_param["alpha"] = new_alpha;
      }
    }
  }

  return new_param;
}

