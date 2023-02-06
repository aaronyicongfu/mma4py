#include "optimizer.h"

PetscErrorCode allocate_petsc_vec(Vec* x, const MPI_Comm comm,
                                  const PetscInt gsize, const PetscInt lsize) {
  // Create an empty vector object
  PetscCall(VecCreate(comm, &(*x)));

  // Set global and optionally local size
  PetscCall(VecSetSizes(*x, lsize, gsize));

  // This vector is an MPI vector
  PetscCall(VecSetType(*x, VECMPI));

  return 0;
}

PetscErrorCode bind_petsc_vec_to_array(Vec* x, const MPI_Comm comm,
                                       const PetscInt gsize,
                                       const PetscInt lsize,
                                       PetscScalar* data) {
  // Create an empty vector object
  PetscCall(VecCreate(comm, &(*x)));

  // Set global and optionally local size
  PetscCall(VecSetSizes(*x, lsize, gsize));

  // This vector is an MPI vector
  PetscCall(VecSetType(*x, VECMPI));

  // Use user provided memory
  PetscCall(VecPlaceArray(*x, data));

  return 0;
}

Optimizer::Optimizer(Problem* prob, const char* log_name)
    : prob(prob), log_name(log_name) {
  // Open the log file
  fp = std::fopen(log_name, "w+");  // Clean the file, if exist

  // Get meta data
  MPI_Comm comm = prob->get_mpi_comm();
  int nvars = prob->get_num_vars();
  int nvars_l = prob->get_num_vars_local();
  int ncons = prob->get_num_cons();

  // Allocate numpy arrays
  np_x = ndarray_t(nvars_l);
  np_lb = ndarray_t(nvars_l);
  np_ub = ndarray_t(nvars_l);
  np_g = ndarray_t(nvars_l);
  np_cons = ndarray_t(ncons);
  np_gcon = ndarray_t(ncons * nvars_l);
  np_gcon = np_gcon.reshape({ncons, nvars_l});

  // Allocate design variable and gradient vectors
  PetscCallAbort(comm, bind_petsc_vec_to_array(&x, comm, nvars, nvars_l,
                                               (double*)np_x.data()));
  PetscCallAbort(comm, bind_petsc_vec_to_array(&g, comm, nvars, nvars_l,
                                               (double*)np_g.data()));
  PetscCallAbort(comm, bind_petsc_vec_to_array(&lb, comm, nvars, nvars_l,
                                               (double*)np_lb.data()));
  PetscCallAbort(comm, bind_petsc_vec_to_array(&ub, comm, nvars, nvars_l,
                                               (double*)np_ub.data()));

  gcon = new Vec[ncons];

  double* gcon_data = (double*)np_gcon.data();
  int offset = 0;
  for (PetscInt i = 0; i < ncons; i++) {
    PetscCallAbort(comm, bind_petsc_vec_to_array(&gcon[i], comm, nvars, nvars_l,
                                                 &gcon_data[offset]));
    offset += nvars_l;
  }
}

Optimizer::~Optimizer() {
  // Close log file
  std::fclose(fp);

  MPI_Comm comm = prob->get_mpi_comm();
  int ncons = prob->get_num_cons();

  PetscCallAbort(comm, VecDestroy(&x));
  PetscCallAbort(comm, VecDestroy(&g));
  PetscCallAbort(comm, VecDestroy(&lb));
  PetscCallAbort(comm, VecDestroy(&ub));
  for (PetscInt i = 0; i < ncons; i++) {
    PetscCallAbort(comm, VecDestroy(&gcon[i]));
  }

  delete[] gcon;
}

PetscErrorCode Optimizer::optimize(int niter) {
  MPI_Comm comm = prob->get_mpi_comm();
  int nvars = prob->get_num_vars();
  int nvars_l = prob->get_num_vars_local();
  int ncons = prob->get_num_cons();

  // Hard-code parameters
  double movelim = 0.2;  // TODO: set this from option dictionary

  // Set initial design and bounds
  prob->getVarsAndBounds(np_x, np_lb, np_ub);

  // Allocate and initialize bounds
  Vec lb_temp, ub_temp;
  PetscCall(allocate_petsc_vec(&lb_temp, comm, nvars, nvars_l));
  PetscCall(allocate_petsc_vec(&ub_temp, comm, nvars, nvars_l));

  // Initialize temporary bounds to constrain moving limit
  double *lbt_vals, *ubt_vals, *lb_vals, *ub_vals;
  lb_vals = (double*)np_lb.data();
  ub_vals = (double*)np_ub.data();
  PetscCall(VecGetArray(lb_temp, &lbt_vals));
  PetscCall(VecGetArray(ub_temp, &ubt_vals));

  for (int i = 0; i < nvars_l; i++) {
    lbt_vals[i] = lb_vals[i];
    ubt_vals[i] = ub_vals[i];
  }

  PetscCall(VecRestoreArray(lb_temp, &lbt_vals));
  PetscCall(VecRestoreArray(ub_temp, &ubt_vals));

  // Allocate mma operator
  MMA mma(nvars, ncons, x);

  // Optimization loop body
  int iter = 0;
  while (iter < niter) {
    // Evaluate functions and gradients
    obj = prob->evalObjCon(np_x, np_cons);
    prob->evalObjConGrad(np_x, np_g, np_gcon);

    // Set move limits
    PetscCall(mma.SetOuterMovelimit(lb, ub, movelim, x, lb_temp, ub_temp));

    // Update design
    mma.Update(x, g, (double*)np_cons.data(), gcon, lb_temp, ub_temp);

    // Check KKT error
    PetscScalar kkterr_l2, kkterr_linf;
    mma.KKTresidual(x, g, (double*)np_cons.data(), gcon, lb_temp, ub_temp,
                    &kkterr_l2, &kkterr_linf);

    // Compute dv l1 norm
    double x_l1;
    PetscCall(VecNorm(x, NORM_1, &x_l1));

    // Compute maximum constraint violation
    double infeas = 0.0;
    double* con_vals = (double*)np_cons.data();
    for (int i = 0; i < ncons; i++) {
      if (con_vals[i] > infeas) {
        infeas = con_vals[i];
      }
    }

    // Print out
    if (iter % 10 == 0) {
      PetscCall(PetscFPrintf(comm, fp, "\n%6s%20s%20s%20s%20s%20s\n", "iter",
                             "obj", "KKT_l2", "KKT_linf", "|x|_1", "infeas"));
    }
    PetscCall(PetscFPrintf(comm, fp, "%6d%20.10e%20.10e%20.10e%20.10e%20.10e\n",
                           iter, obj, kkterr_l2, kkterr_linf, x_l1, infeas));
    fflush(fp);

    iter++;
  }

  PetscCall(VecDestroy(&lb_temp));
  PetscCall(VecDestroy(&ub_temp));

  return 0;
}

ndarray_t Optimizer::getOptimizedDesign() { return np_x; }