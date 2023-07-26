#include "newton_optimizer.hh"

// In order to preserve the sparsity pattern, we enforce the active bound constraints by
// zeroing out the rows/cols corresponding to the variable, placing a 1 on its diagonal, and zeroing
// out its component of the gradient (instead of removing these rows/columns).
void fixVariablesInWorkingSet(const NewtonProblem &prob, SuiteSparseMatrix &H, Eigen::VectorXd &grad, const WorkingSet &ws) {
    if (ws.size() == 0) return;

    BENCHMARK_START_TIMER("fixVariablesInWorkingSet");
    // Zero out the rows corresponding to all variables in the working set
    for (size_t elem = 0; elem < H.Ai.size(); ++elem)
        if (ws.fixesVariable(H.Ai[elem])) H.Ax[elem] = 0.0;

    // Zero out working set vars' columns/gradient components, placing a 1 on the diagonal
    const SuiteSparseMatrix::index_type nv = prob.numVars();
    for (SuiteSparseMatrix::index_type var = 0; var < nv; ++var) {
        if (!ws.fixesVariable(var)) continue;
        grad[var] = 0;
        const auto start = H.Ap[var    ],
                   end   = H.Ap[var + 1];
        Eigen::Map<Eigen::VectorXd>(H.Ax.data() + start, end - start).setZero();
        H.Ax[end - 1] = 1.0; // Diagonal should be the column's last entry; we assume it exists in the sparsity pattern!
    }

    BENCHMARK_STOP_TIMER("fixVariablesInWorkingSet");
}

// Returns "tau", the coefficient of the metric term that was added to make the Hessian positive definite.
// "-tau" can be interpreted as an estimate (lower bound) for the smallest generalized eigenvalue for "H d = lambda M d"
// (Returns 0 if the Hessian is already positive definite).
// Upon return, "solver" holds a factorization of the matrix:
//     (H + tau (M / ||M||_2))
Real NewtonOptimizer::newton_step(Eigen::VectorXd &step, /* copy modified inside */ Eigen::VectorXd g, const WorkingSet &ws, Real &beta, const Real betaMin, const bool feasibility) {
    BENCHMARK_SCOPED_TIMER_SECTION ns_timer("newton_step");
    step.resize(g.size());

    BENCHMARK_START_TIMER_SECTION("hessEval");
    auto H_reduced = prob->hessian();
    fixVariablesInWorkingSet(*prob, H_reduced, g, ws);
    H_reduced.rowColRemoval([&](SuiteSparse_long i) { return isFixed[i]; });
    BENCHMARK_STOP_TIMER_SECTION("hessEval");

    // The following Hessian modification strategy is an improved version of
    // "Cholesky with added multiple of the identity" from
    // Nocedal and Wright 2006, pp 51.
    // We use a custom matrix instead of the identity, drawing an analogy
    // to trust region methods: the multiplier (scaledTau) that we use
    // corresponds to some trust region radius in the metric defined by the
    // added matrix, and some metrics can work much better than the
    // Euclidean distance in the parameter space. For instance,
    // the mass matrix is a good choice.
    Real tau = 0;

    // Though the full mass matrix is cached by NewtonProblem, we also want to cache
    // the reduced version (if it is ever needed).
    std::unique_ptr<SuiteSparseMatrix> M_reduced;

    Eigen::VectorXd x, gReduced;

    auto postprocessSolution = [&]() {
        extractFullSolution(x, step);
        step *= -1;
        // ws.validateStep(step);

        if (prob->hasLEQConstraint()) {
            // TODO: handle more than a single constraint...
            Eigen::VectorXd a = removeFixedEntries(ws.getFreeComponent(prob->LEQConstraintMatrix()));
            kkt_solver.update(solver(), a);
            const Real r = feasibility ? prob->LEQConstraintResidual() : 0.0;
            extractFullSolution(kkt_solver.solve(-x, r), step);
        }
    };

    if(!prob->sparsityPatternFactorizationUpToDate())
        updateSymbolicFactorization(H_reduced);

    Real currentTauScale = 0; // simple caching mechanism to avoid excessive calls to tauScale()
    while (true) {
        try {
            BENCHMARK_SCOPED_TIMER_SECTION timer("Newton solve");
            if (tau != 0) {
                if (!M_reduced) {
                    M_reduced = std::make_unique<SuiteSparseMatrix>(prob->metric());
                    fixVariablesInWorkingSet(*prob, *M_reduced, g, ws);
                    M_reduced->rowColRemoval([&](SuiteSparse_long i) { return isFixed[i]; });
                }

                auto Hmod = H_reduced;
                Hmod.addWithIdenticalSparsity(*M_reduced, tau * currentTauScale); // Note: rows/cols corresponding to vars with active bounds will now have a nonzero value different from 1 on the diagonal, but this is fine since the RHS component is zero...
                solver().updateFactorization(std::move(Hmod));
            }
            else {
                solver().updateFactorization(H_reduced);
            }

            BENCHMARK_SCOPED_TIMER_SECTION solve("Solve");

            gReduced = removeFixedEntries(g);
            solver().solve(gReduced, x);
            if (!solver().checkPosDef()) throw std::runtime_error("System matrix is not positive definite");
            postprocessSolution();

            break;
        }
        catch (std::exception &e) {
            tau  = std::max(  4 * tau, beta);
            beta = std::max(0.5 * tau, betaMin);
            std::cout << e.what() << "; increasing tau to " << tau << std::endl;
            if (currentTauScale == 0) currentTauScale = tauScale();
            if (tau > 1e80) {
                // prob->writeDebugFiles("tau_runaway");
                std::cout << "||H||_2: "    << prob->hessianL2Norm() << std::endl;
                std::cout << "||M||_2: "    << prob->metricL2Norm()  << std::endl;
                std::cout << "Scaled tau: " << tau * currentTauScale << std::endl;
                throw std::runtime_error("Tau running away");
            }
        }
    }

    return tau;
}

ConvergenceReport NewtonOptimizer::optimize() {
    const size_t nbacktrack_iter = 15;

    prob->setUseIdentityMetric(options.useIdentityMetric);
    prob->writeIterates = options.writeIterateFiles;

    prob->setVars(prob->applyBoundConstraints(prob->getVars()));
    Eigen::VectorXd vars, step, g(prob->numVars());

    // Indices of the bound constraints in our working set.
    WorkingSet workingSet(*prob);

    Real beta = options.beta;
    const Real betaMin = std::min(beta, 1e-6); // Initial shift "tau" to use when an indefinite matrix is detected.

    m_cachedHessianL2Norm.reset();

    if (prob->hasLEQConstraint()) {
        if (!prob->LEQConstraintIsFeasible()) {
            if (options.feasibilitySolve) {
                // std::cout << "Running feasibility solve with residual " << prob->LEQConstraintResidual() << ", energy " << prob->energy() << std::endl;
                Eigen::VectorXd step;
                prob->iterationCallback(0);
                newton_step(step, prob->gradient(true), workingSet, beta, betaMin, true);
                // We must take a full step to ensure feasibility
                // TODO: use multiple iterations and a line search to get feasible?
                prob->setVars(prob->applyBoundConstraints(step + prob->getVars()));
                // std::cout << "Post feasibility solve residual " << prob->LEQConstraintResidual() << ", energy " << prob->energy() << std::endl;
            }
            else {
                prob->LEQStepFeasible();
            }
        }
        prob->setVars(prob->applyBoundConstraints(prob->getVars()));
        if (!prob->LEQConstraintIsFeasible()) {
            std::cout << "Post feasibility step residual: " << prob->LEQConstraintResidual() << std::endl;
            throw std::runtime_error("Iterate still infeasible");
        }
    }

    const auto &fixedVars = prob->fixedVars();
    auto zeroOutFixedVars = [&](const Eigen::VectorXd &g) { auto result = g; for (size_t var : fixedVars) result[var] = 0.0; return result; };

    ConvergenceReport report;

    Real alpha = 0;
    bool isIndefinite = false;
    auto reportIterate = [&](size_t i, Real energy, const Eigen::VectorXd &g, const Eigen::VectorXd &g_free) {
        prob->writeIterateFiles(i);
        report.addEntry(energy, g.norm(), g_free.norm(), alpha, isIndefinite);

        if (options.verbose && ((i % options.verbose) == 0)) {
            std::cout << i << '\t';
            report.printEntry();
        }
    };

    BENCHMARK_START_TIMER_SECTION("Newton iterations");
    size_t it;

    Eigen::VectorXd za;
    if (prob->hasLEQConstraint()) { za = zeroOutFixedVars(prob->LEQConstraintMatrix()); }
    // Kill off components of "v" in the span of the LEQ constraint vectors
    auto projectOutLEQConstrainedComponents = [&](Eigen::VectorXd &v) { if (prob->hasLEQConstraint()) v -= za * (v.dot(za) / za.squaredNorm()); };

    for (it = 1; it <= options.niter; ++it) {
        BENCHMARK_SCOPED_TIMER_SECTION it_timer("Newton iterate");

        // std::cout << "pre-update gradient: " << zeroOutFixedVars(prob->gradient(false)).norm() << std::endl;
        prob->iterationCallback(it);
        vars = prob->getVars();

        g = prob->gradient(true);
        const Real currEnergy = prob->energy();
        auto zg = zeroOutFixedVars(g); // non-fixed components of the gradient; used for termination criteria
        projectOutLEQConstrainedComponents(zg);
        // Gradient with respect to the "free" variables (components corresponding to fixed/actively constrained variables zero-ed out)
        const auto g_free = workingSet.getFreeComponent(zg);

        if ((!isIndefinite) && (zg.norm() < options.gradTol)) {
            report.success = true;
            break; // TODO: termination criterion when bounds are active at the optimum
        }

        BENCHMARK_START_TIMER_SECTION("Compute descent direction");

        // Free variables in the working set from their bound constraints, if necessary
        workingSet.remove_if([&](size_t bc_idx) {
                bool shouldRemove = prob->boundConstraint(bc_idx).shouldRemoveFromWorkingSet(g, g_free);
                if (shouldRemove) { std::cout << "Removed constraint " << bc_idx << " from working set" << std::endl; }
                return shouldRemove;
            });

        Real old_beta = beta;
        Real tau;
        try {
            tau = newton_step(step, g, workingSet, beta, betaMin);
        }
        catch (std::exception &e) {
            // Tau ran away
            BENCHMARK_STOP_TIMER_SECTION("Compute descent direction");
            break;
        }
        isIndefinite = (tau != 0.0);

        // Only add in negative curvature directions when "tau" is a reasonable estimate for the smallest eigenvalue and the gradient has become small.
        if (options.useNegativeCurvatureDirection && ((tau > old_beta) || (tau == betaMin)) && (g_free.norm() < 100 * options.gradTol)) {
            BENCHMARK_SCOPED_TIMER_SECTION timer("Negative curvature dir");
            // std::cout.precision(19);
            std::cout << "Computing negative curvature direction for scaled tau = " << tau / prob->metricL2Norm() << std::endl;
            auto M_reduced = prob->metric();
            fixVariablesInWorkingSet(*prob, M_reduced, g, workingSet);
            M_reduced.rowColRemoval([&](SuiteSparse_long i) { return isFixed[i]; });
            auto d = negativeCurvatureDirection(solver(), M_reduced, 1e-6);
            {
                Real dnorm = d.norm();
                if (dnorm != 0.0) {
                    Eigen::VectorXd tmp(step.size());
                    extractFullSolution(d, tmp); // negative curvature direction was computed in reduced variables...
                    d = tmp;
                    // {
                    //     const SuiteSparseMatrix &H = prob->hessian();
                    //     H.applyRaw(d.data(), tmp.data());
                    //     Real lambda = d.dot(tmp);
                    //     std::cout << "Found negative curvature direction with eigenvalue " << lambda << std::endl;
                    // }
                    if (d.dot(g) > 0) d *= -1; // Move in the opposite direction as the gradient (So we still produce a descent direction)
                    const Real cd = prob->characteristicDistance(d);
                    if (cd <= 0) // problem doesn't provide one
                        step += std::sqrt(step.squaredNorm() / d.squaredNorm()) * d; // TODO: find a better balance between newton step and negative curvature.
                    else {
                        step += 1e-2 * (d / cd);
                    }
                }
                else { std::cout << "Negative curvature direction calculation failed" << std::endl; }
            }
        }

        Real directionalDerivative = g_free.dot(step);
        // if (options.verbose)
        //     std::cout << "Found step with directional derivative: " << directionalDerivative << std::endl;

        BENCHMARK_STOP_TIMER_SECTION("Compute descent direction");

        BENCHMARK_START_TIMER_SECTION("Backtracking");
        // Simple backtracking line search to ensure a sufficient decrease

        Real feasible_alpha;
        size_t blocking_idx;
        std::tie(feasible_alpha, blocking_idx) = prob->feasibleStepLength(vars, step);

        // To add multiple nearby bounds to the working set at once, we allow the
        // step to overshoot the bounds slightly (note: variables will be clamped to the bounds anyway before
        // evaluating the objective). Then any bound violated by the step length obtaining
        // sufficient decrease is added to the working set.
        alpha = std::min(1.0, feasible_alpha + 1e-3);

        const Real c_1 = 1e-2;
        size_t bit;

        Eigen::VectorXd steppedVars;
        for (bit = 0; bit < nbacktrack_iter; ++bit) {
            steppedVars = vars + alpha * step;
            prob->applyBoundConstraintsInPlace(steppedVars);
            prob->setVars(steppedVars);
            Real steppedEnergy = prob->energy();

            if  (steppedEnergy - currEnergy <= c_1 * alpha * directionalDerivative)
                break;
            if (alpha > feasible_alpha) {
                // It's possible that our slight overshooting and clamping to the bounds did not achieve a sufficient
                // decrease whereas a step to the first violated bound would; make sure we try this exact step too
                // before continuing the backtracking search.
                alpha = feasible_alpha;
            }
            else {
                alpha *= 0.5;
            }
        }
        BENCHMARK_STOP_TIMER_SECTION("Backtracking");

        reportIterate(it - 1, currEnergy, zg, g_free); // Record iterate statistics, now that we know alpha, isIndefinite
        prob->customIterateReport(report);

        // Add to the working set all bounds encountered by the step of length "alpha"
        for (size_t bci = 0; bci < prob->numBoundConstraints(); ++bci) {
            if (alpha >= prob->boundConstraint(bci).feasibleStepLength(vars, step)) {
                if (workingSet.contains(bci)) throw std::logic_error("Re-encountered bound in working set");
                workingSet.add(bci);
                std::cout << "Added constraint " << bci << " to working set" << std::endl;
            }
        }

        if (bit == nbacktrack_iter) {
            prob->setVars(vars);
            if (options.verbose) std::cout << "Backtracking failed.\n";
            // Stop optimizing.
            break;
        }
    }

    // Report the last iterate; gradient must be re-computed in case the iteration limit was exceeded
    if (it > options.niter) {
        prob->iterationCallback(it);
        g = prob->gradient(true);
    }
    auto zg = zeroOutFixedVars(g);
    projectOutLEQConstrainedComponents(zg);
    prob->customIterateReport(report);
    reportIterate(it - 1, prob->energy(), zg, workingSet.getFreeComponent(zg));

    if (workingSet.size()) {
        std::cout << "Terminated with working set:" << std::endl;
        vars = prob->getVars();
        for (size_t bci = 0; bci < prob->numBoundConstraints(); ++bci) {
            if (workingSet.contains(bci)) prob->boundConstraint(bci).report(vars, g);
        }
    }

    // std::cout << "Before apply bound constraints: " << prob->energy() << std::endl;
    // prob->setVars(prob->applyBoundConstraints(prob->getVars()));
    // std::cout << "After  apply bound constraints: " << prob->energy() << std::endl;

    BENCHMARK_STOP_TIMER_SECTION("Newton iterations");

    return report;
}
