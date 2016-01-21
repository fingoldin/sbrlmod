/*
 * Scalable Bayesian Rulelist training
 */

#include <assert.h>
#include <errno.h>
#include <float.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "mytime.h"
#include "rule.h"
#include <gsl/gsl_rng.h>
#include <gsl/gsl_randist.h>
#include <gsl/gsl_cdf.h>
#include <gsl/gsl_sf.h>

#define EPSILON 1e-9
#define MAX_RULE_CARDINALITY 10

gsl_rng *RAND_GSL;

static double  *log_lambda_pmf, *log_eta_pmf;
static int n_add, n_delete, n_swap;
int debug = 0;

double compute_log_posterior(ruleset_t *,
    rule_t *, int, rule_t *, params_t *, int, int, double *);
int gen_poission(double);
double *get_theta(ruleset_t *, rule_t *, rule_t *, params_t *);
void gsl_ran_poisson_test();
void init_gsl_rand_gen();
ruleset_t *run_mcmc(int, int, int, int, rule_t *, rule_t *, params_t *, double);

/****** These are the heart of both MCMC and SA ******/
/*
 * Once we encapsulate the acceptance critera, we can use the same routine,
 * propose, to make proposals and determine acceptance. This leaves a lot
 * of the memory management nicely constrained in this routine.
 */

int
mcmc_accepts(double new_log_post, double old_log_post,
    double prefix_bound, double max_log_post, double *extra)
{
	/* Extra = jump_prob */
	return (prefix_bound > max_log_post &&
	    log((random() / (float)RAND_MAX)) < 
	    (new_log_post - old_log_post + log(*extra)));
}

int
sa_accepts(double new_log_post, double old_log_post,
    double prefix_bound, double max_log_post, double *extra)
{
	/* Extra = tk */
	return (prefix_bound > max_log_post &&
	    (new_log_post > old_log_post ||
	     (log((random() / (float)RAND_MAX)) < 
	     (new_log_post - old_log_post) / *extra)));
}


/*
 * Create a proposal; used both by simulated annealing and MCMC.
 * 1. Compute proposal parameters
 * 2. Create the new proposal ruleset
 * 3. Compute the log_posterior
 * 4. Call the appropriate function to determine acceptance criteria
 */
ruleset_t *
propose(ruleset_t *rs, rule_t *rules, rule_t *labels, int nrules,
    double *jump_prob, double *ret_log_post, double max_log_post,
    int *cnt, double *extra, params_t *params,
    int (*accept_func)(double, double, double, double, double *))
{
	char stepchar;
	double new_log_post, prefix_bound;
	int change_ndx, ndx1, ndx2;
	ruleset_t *rs_new, *rs_ret;

	if (ruleset_copy(&rs_new, rs) != 0)
		return (NULL);

	ruleset_proposal(rs_new, nrules, &ndx1, &ndx2, &stepchar, jump_prob);

	if (debug > 10) {
		printf("Given ruleset: \n");
		ruleset_print(rs, rules, (debug > 100));
		printf("Operation %c(%d)(%d) produced proposal:\n",
		    stepchar, ndx1, ndx2);
	}
	switch (stepchar) {
	case 'A':
		/* Add the rule whose id is ndx1 at position ndx2 */
		if (ruleset_add(rules, nrules, &rs_new, ndx1, ndx2) != 0)
			return (NULL);
		change_ndx = ndx2;
		n_add++;
		break;
	case 'D':
		/* Delete the rule at position ndx1. */
		ruleset_delete(rules, nrules, rs_new, ndx1);
		change_ndx = ndx1;
		n_delete++;
		break;
	case 'S':
		/* Swap the rules at ndx1 and ndx2. */
		ruleset_swap_any(rs_new, ndx1, ndx2, rules);
		change_ndx = ndx1;
		n_swap++;
		break;
	default:
		break;
	}

	new_log_post = compute_log_posterior(rs_new,
	    rules, nrules, labels, params, 0, change_ndx, &prefix_bound);

	if (debug > 10) {
		ruleset_print(rs_new, rules, (debug > 100));
		printf("With new log_posterior = %0.6f\n", new_log_post);
	}
	if (prefix_bound < max_log_post)
		(*cnt)++;

	if (accept_func(new_log_post,
	    *ret_log_post, prefix_bound, max_log_post, extra)) {
	    	if (debug > 10)
			printf("Accepted\n");
		rs_ret = rs_new;
		*ret_log_post = new_log_post;
		ruleset_destroy(rs);
	} else {
	    	if (debug > 10)
			printf("Rejected\n");
		rs_ret = rs;
		ruleset_destroy(rs_new);
	}

	return (rs_ret);
}

/********** End of proposal routines *******/

pred_model_t   *
train(data_t *train_data, int initialization, int method, params_t *params)
{
	pred_model_t *pred_model;
	ruleset_t *rs, *rs_temp;
	double max_pos, pos_temp, null_bound;

	max_pos = -1e9;
	pred_model = calloc(1, sizeof(pred_model_t));
	if (pred_model == NULL)
		return (NULL);

	rs = run_mcmc(params->iters, params->init_size, train_data->nsamples,
	    train_data->nrules, train_data->rules, train_data->labels, params,
	    max_pos);

	max_pos = compute_log_posterior(rs, train_data->rules,
	    train_data->nrules, train_data->labels, params, 1, -1, &null_bound);

	for (int chain = 1; chain < params->nchain; chain++) {
		rs_temp = run_mcmc(params->iters, params->init_size,
		    train_data->nsamples, train_data->nrules,
		    train_data->rules, train_data->labels, params, max_pos);
		pos_temp = compute_log_posterior(rs_temp, train_data->rules,
		    train_data->nrules, train_data->labels, params, 1, -1,
		    &null_bound);

		if (pos_temp >= max_pos) {
			ruleset_destroy(rs);
			rs = rs_temp;
			max_pos = pos_temp;
		} else {
			ruleset_destroy(rs_temp);
		}
	}

	pred_model->theta =
	    get_theta(rs, train_data->rules, train_data->labels, params);
	pred_model->rs = rs;

	/* Free allocated memory. */
	free(log_lambda_pmf);
	free(log_eta_pmf);
	return pred_model;
}

double *
get_theta(ruleset_t * rs, rule_t * rules, rule_t * labels, params_t *params)
{
	/* calculate captured 0's and 1's */
	VECTOR v0;
	double *theta;

	rule_vinit(rs->n_samples, &v0);
	theta = malloc(rs->n_rules * sizeof(double));
	if (theta == NULL)
		return (NULL);

	for (int j = 0; j < rs->n_rules; j++) {
		int n0, n1;

		rule_vand(v0, rs->rules[j].captures,
		    labels[0].truthtable, rs->n_samples, &n0);
		n1 = rs->rules[j].ncaptured - n0;
		theta[j] = (n1 + params->alpha[1]) * 1.0 /
		    (n1 + n0 + params->alpha[0] + params->alpha[1]);
		if (debug) {
			printf("n0=%d, n1=%d, captured=%d, training accuracy =",
			    n0, n1, rs->rules[j].ncaptured);
			if (theta[j] >= params->threshold)
				printf(" %.8f\n",
				    n1 * 1.0 / rs->rules[j].ncaptured);
			else
				printf(" %.8f\n",
				    n0 * 1.0 / rs->rules[j].ncaptured);
			printf("theta[%d] = %.8f\n", j, theta[j]);
		}
	}
	rule_vfree(&v0);
	return theta;
}

ruleset_t *
run_mcmc(int iters, int init_size, int nsamples, int nrules,
    rule_t *rules, rule_t *labels, params_t *params, double v_star)
{
	ruleset_t *rs;
	double jump_prob, log_post_rs;
	int *rs_idarray, len, nsuccessful_rej;
	double max_log_posterior, prefix_bound;

	rs = NULL;
	rs_idarray = NULL;
	log_post_rs = 0.0;
	nsuccessful_rej = 0;
	prefix_bound = -1e10;
	n_add = n_delete = n_swap = 0;

	/* initialize random number generator for some distrubitions */
	init_gsl_rand_gen();

	/* Initialize the ruleset. */
	if (debug > 10)
		printf("Prefix bound = %10f v_star = %f\n",
		    prefix_bound, v_star);
	while (prefix_bound < v_star) {
		if (rs != NULL)
			ruleset_destroy(rs);
		create_random_ruleset(init_size, nsamples, nrules, rules, &rs);
		log_post_rs = compute_log_posterior(rs, rules,
		    nrules, labels, params, 0, 0, &prefix_bound);
		if (debug > 10) {
			printf("Initial random ruleset\n");
			ruleset_print(rs, rules, 1);
			printf("Prefix bound = %f v_star = %f\n",
			    prefix_bound, v_star);
		}
	}

	/*
	 * The initial ruleset is our best ruleset so far, so keep a
	 * list of the rules it contains.
	 */
	ruleset_backup(rs, &rs_idarray);
	max_log_posterior = log_post_rs;
	len = rs->n_rules;

	for (int i = 0; i < iters; i++) {
		rs = propose(rs, rules, labels, nrules, &jump_prob,
		    &log_post_rs, max_log_posterior, &nsuccessful_rej,
		    &jump_prob, params, mcmc_accepts);

		if (log_post_rs > max_log_posterior) {
			ruleset_backup(rs, &rs_idarray);
			max_log_posterior = log_post_rs;
			len = rs->n_rules;
		}
	}

	/* Regenerate the best rule list */
	ruleset_destroy(rs);
	ruleset_init(len, nsamples, rs_idarray, rules, &rs);
	free(rs_idarray);

	if (debug) {
		printf("\n%s%d #add=%d #delete=%d #swap=%d):\n",
		"The best rule list is (#reject=", nsuccessful_rej,
		n_add, n_delete, n_swap);

		printf("max_log_posterior = %6f\n", max_log_posterior);
		printf("max_log_posterior = %6f\n",
		    compute_log_posterior(rs, rules,
		    nrules, labels, params, 1, -1, &prefix_bound));
		ruleset_print(rs, rules, (debug > 100));
	}
	return rs;
}

ruleset_t *
run_simulated_annealing(int iters, int init_size, int nsamples,
    int nrules, rule_t * rules, rule_t * labels, params_t *params)
{
	ruleset_t *rs;
	double jump_prob;
	int dummy, iters_per_step, *rs_idarray = NULL, len;
	double log_post_rs, max_log_posterior = -1e9, prefix_bound = 0.0;

	log_post_rs = 0.0;
	iters_per_step = 200;

	/* Initialize random number generator for some distrubitions. */
	init_gsl_rand_gen();

	/* Initialize the ruleset. */
	if (create_random_ruleset(init_size, nsamples, nrules, rules, &rs) != 0)
		return (NULL);

	log_post_rs = compute_log_posterior(rs,
	    rules, nrules, labels, params, 0, -1, &prefix_bound);
	ruleset_backup(rs, &rs_idarray);
	max_log_posterior = log_post_rs;
	len = rs->n_rules;

	if (debug > 10) {
		printf("Initial ruleset: \n");
		ruleset_print(rs, rules, (debug > 100));
	}

	/* Pre-compute the cooling schedule. */
	double T[100000], tmp[50];
	int ntimepoints = 0;

	tmp[0] = 1;
	for (int i = 1; i < 28; i++) {
		tmp[i] = tmp[i - 1] + exp(0.25 * (i + 1));
		for (int j = (int)tmp[i - 1]; j < (int)tmp[i]; j++)
			T[ntimepoints++] = 1.0 / (i + 1);
	}

	if (debug > 0)
		printf("iters_per_step = %d, #timepoints = %d\n",
		    iters_per_step, ntimepoints);

	for (int k = 0; k < ntimepoints; k++) {
		double tk = T[k];
		for (int iter = 0; iter < iters_per_step; iter++) {
    			rs = propose(rs, rules, labels, nrules, &jump_prob,
			    &log_post_rs, max_log_posterior, &dummy, &tk,
			    params, sa_accepts);

			if (log_post_rs > max_log_posterior) {
				ruleset_backup(rs, &rs_idarray);
				max_log_posterior = log_post_rs;
				len = rs->n_rules;
			}
		}
	}
	/* Regenerate the best rule list. */
	ruleset_destroy(rs);
	printf("\n\n/*----The best rule list is: */\n");
	ruleset_init(len, nsamples, rs_idarray, rules, &rs);
	printf("max_log_posterior = %6f\n\n", max_log_posterior);
	printf("max_log_posterior = %6f\n\n",
	    compute_log_posterior(rs, rules,
	    nrules, labels, params, 1, -1, &prefix_bound));
	ruleset_print(rs, rules, (debug > 100));

	return rs;
}

double
compute_log_posterior(ruleset_t *rs, rule_t *rules, int nrules, rule_t *labels,
    params_t *params, int ifPrint, int length4bound, double *prefix_bound)
{
	static double	eta_norm = 0;

	double log_prior = 0.0; 
	double log_likelihood = 0.0;
	double prefix_prior = 0.0;
	double norm_constant;
	int li;

	/* Prior pre-calculation */
	if (log_lambda_pmf == NULL) {
		log_lambda_pmf = malloc(nrules * sizeof(double));
		log_eta_pmf =
		    malloc((1 + MAX_RULE_CARDINALITY) * sizeof(double));
		for (int i = 0; i < nrules; i++) {
			log_lambda_pmf[i] =
			    log(gsl_ran_poisson_pdf(i, params->lambda));
			if (debug > 100)
				printf("log_lambda_pmf[ %d ] = %6f\n",
				    i, log_lambda_pmf[i]);
		}
		for (int i = 0; i <= MAX_RULE_CARDINALITY; i++) {
			log_eta_pmf[i] =
			    log(gsl_ran_poisson_pdf(i, params->eta));
			if (debug > 100)
				printf("log_eta_pmf[ %d ] = %6f\n",
				    i, log_eta_pmf[i]);
		}

		/*
		 * for simplicity, assume that all the cardinalities
		 * <= MAX_RULE_CARDINALITY appear in the mined rules
		 */
		eta_norm = gsl_cdf_poisson_P(MAX_RULE_CARDINALITY, params->eta)
		    - gsl_ran_poisson_pdf(0, params->eta);
		if (debug > 10)
			printf("eta_norm(Beta_Z) = %6f\n", eta_norm);
	}

	/* Calculate log_prior. */
	int maxcard = 0;
	int card_count[1 + MAX_RULE_CARDINALITY];

	for (int i = 0; i <= MAX_RULE_CARDINALITY; i++)
		card_count[i] = 0;

	for (int i = 0; i < nrules; i++) {
		card_count[rules[i].cardinality]++;
		if (rules[i].cardinality > maxcard)
			maxcard = rules[i].cardinality;
	}

	if (debug > 10)
		for (int i = 0; i <= MAX_RULE_CARDINALITY; i++)
			printf("There are %d rules with cardinality %d.\n",
			    card_count[i], i);
	norm_constant = eta_norm;
	log_prior += log_lambda_pmf[rs->n_rules - 1];


	if (rs->n_rules - 1 > params->lambda)
		prefix_prior += log_lambda_pmf[rs->n_rules - 1];
	else
		prefix_prior += log_lambda_pmf[(int)params->lambda];

	// Don't compute the last (default) rule.
	for (int i = 0; i < rs->n_rules - 1; i++) {
		li = rules[rs->rules[i].rule_id].cardinality;
		if (log(norm_constant) != log(norm_constant))
			printf("NAN log(eta_norm) at i= %d\teta_norm = %6f",
			    i, eta_norm);
		log_prior += log_eta_pmf[li] - log(norm_constant);

		if (log_prior != log_prior)
			printf("\n NAN here at i= %d, aa ", i);
		log_prior += -log(card_count[li]);
		if (log_prior != log_prior)
			printf("\n NAN here at i= %d, bb ", i);
		if (i <= length4bound) {
			//added for prefix_boud
			prefix_prior += log_eta_pmf[li] - 
			    log(norm_constant) - log(card_count[li]);
		}

		card_count[li]--;
		if (card_count[li] == 0)
			norm_constant -= exp(log_eta_pmf[li]);
	}
	/* Calculate log_likelihood */
	VECTOR v0;
	double prefix_log_likelihood = 0.0;
	int left0 = labels[0].support, left1 = labels[1].support;

	rule_vinit(rs->n_samples, &v0);
	for (int j = 0; j < rs->n_rules; j++) {
		int n0, n1;
		rule_vand(v0, rs->rules[j].captures,
		    labels[0].truthtable, rs->n_samples, &n0);
		n1 = rs->rules[j].ncaptured - n0;
		log_likelihood += gsl_sf_lngamma(n0 + params->alpha[0]) + 
		    gsl_sf_lngamma(n1 + params->alpha[1]) - 
		    gsl_sf_lngamma(n0 + n1 +
		        params->alpha[0] + params->alpha[1]);
		// Added for prefix_bound.
		left0 -= n0;
		left1 -= n1;
		if (j <= length4bound) {
			prefix_log_likelihood += gsl_sf_lngamma(n0 + 1) + 
			    gsl_sf_lngamma(n1 + 1) - 
			    gsl_sf_lngamma(n0 + n1 + 2);
			if (j == length4bound) {
				prefix_log_likelihood += gsl_sf_lngamma(1) + 
				    gsl_sf_lngamma(left0 + 1) - 
				    gsl_sf_lngamma(left0 + 2) + 
				    gsl_sf_lngamma(1) + 
				    gsl_sf_lngamma(left1 + 1) - 
				    gsl_sf_lngamma(left1 + 2);
			}
		}
	}
	*prefix_bound = prefix_prior + prefix_log_likelihood;
	if (debug > 20)
		printf("log_prior = %6f\t log_likelihood = %6f\n",
		    log_prior, log_likelihood);
	rule_vfree(&v0);
	return log_prior + log_likelihood;
}

void
ruleset_proposal(ruleset_t * rs, int nrules,
    int *ndx1, int *ndx2, char *stepchar, double *jumpRatio){
	static double MOVEPROBS[15] = {
		0.0, 1.0, 0.0,
		0.0, 0.5, 0.5,
		0.5, 0.0, 0.5,
		1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0,
		1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0
	};
	static double JUMPRATIOS[15] = {
		0.0, 0.5, 0.0,
		0.0, 2.0 / 3.0, 2.0,
		1.0, 0.0, 2.0 / 3.0,
		1.0, 1.5, 1.0,
		1.0, 1.0, 1.0
	};

	double moveProbs[3], jumpRatios[3];
	int offset = 0;
	if (rs->n_rules == 1) {
		offset = 0;
	} else if (rs->n_rules == 2) {
		offset = 3;
	} else if (rs->n_rules == nrules - 1) {
		offset = 6;
	} else if (rs->n_rules == nrules - 2) {
		offset = 9;
	} else {
		offset = 12;
	}
	memcpy(moveProbs, MOVEPROBS + offset, 3 * sizeof(double));
	memcpy(jumpRatios, JUMPRATIOS + offset, 3 * sizeof(double));

	double u = ((double)rand()) / (RAND_MAX);
	int index1, index2;

	if (u < moveProbs[0]) {
		// Swap rules: cannot swap with the default rule
		index1 = rand() % (rs->n_rules - 1);

		// Make sure we do not swap with ourselves
		do {
			index2 = rand() % (rs->n_rules - 1);
		} while (index2 == index1);

		*jumpRatio = jumpRatios[0];
		*stepchar = 'S';
	} else if (u < moveProbs[0] + moveProbs[1]) {
		/* Add a new rule */
		index1 = pick_random_rule(nrules, rs);
		index2 = rand() % rs->n_rules;
		*jumpRatio = jumpRatios[1] * (nrules - 1 - rs->n_rules);
		*stepchar = 'A';
	} else if (u < moveProbs[0] + moveProbs[1] + moveProbs[2]) {
		/* delete an existing rule */
		index1 = rand() % (rs->n_rules - 1);
		//cannot delete the default rule
			index2 = 0;
		//index2 doesn 't matter in this case
			* jumpRatio = jumpRatios[2] * (nrules - rs->n_rules);
		*stepchar = 'D';
	} else {
		//should raise exception here.
	}
	*ndx1 = index1;
	*ndx2 = index2;
}

void
init_gsl_rand_gen()
{
	if (RAND_GSL != NULL) {
		gsl_rng_env_setup();
		RAND_GSL = gsl_rng_alloc(gsl_rng_default);
	}
}

int
gen_poisson(double mu)
{
	return (int)gsl_ran_poisson(RAND_GSL, mu);
}

double
gen_poission_pdf(int k, double mu)
{
	return gsl_ran_poisson_pdf(k, mu);
}

double
gen_gamma_pdf (double x, double a, double b)
{
	return gsl_ran_gamma_pdf(x, a, b);
}

void
gsl_ran_poisson_test()
{
	unsigned int k1 = gsl_ran_poisson(RAND_GSL, 5);
	unsigned int k2 = gsl_ran_poisson(RAND_GSL, 5);

	printf("k1 = %u , k2 = %u\n", k1, k2);

	//number of experiments
	const int nrolls = 10000;

	//maximum number of stars to distribute
	const int nstars = 100;

	int p[10] = {};
	for (int i = 0; i < nrolls; ++i) {
		unsigned int number = gsl_ran_poisson(RAND_GSL, 4.1);

		if (number < 10)
			++p[number];
	}

	printf("poisson_distribution (mean=4.1):\n");

	for (int i = 0; i < 10; ++i) {
		printf("%d, : ", i);
		for (int j = 0; j < p[i] * nstars / nrolls; j++)
			printf("*");
		printf("\n");
	}
}