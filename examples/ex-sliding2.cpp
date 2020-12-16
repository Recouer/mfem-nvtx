// Demonstrate a sliding boundary condition in elasticity using the
// ConstrainedSolver framework

// use a sliding trapezoid

// boundary attribute 4: fixed left side
// boundary attribute 2: force applied on right side
// boundary attribute 5, 6, 7, 8: sliding on internal circle

// 1 is bottom, 2 is right side, 4 is left side

#include "mfem.hpp"
#include <fstream>
#include <iostream>

using namespace std;
using namespace mfem;

/*
   Given a vector space fespace, and the array constrained_att that
   includes the boundary *attributes* that are constrained to have normal
   component zero, this returns a SparseMatrix representing the
   constraints that need to be imposed.
*/
HypreParMatrix * BuildNormalConstraints(ParFiniteElementSpace& fespace,
                                        Array<int> constrained_att)
{
   int rank, size;
   MPI_Comm_rank(fespace.GetComm(), &rank);
   MPI_Comm_size(fespace.GetComm(), &size);
   int dim = fespace.GetVDim();

   std::set<int> constrained_tdofs; // local tdofs
   for (int i = 0; i < fespace.GetNBE(); ++i)
   {
      int att = fespace.GetBdrAttribute(i);
      if (constrained_att.FindSorted(att) != -1)
      {
         Array<int> dofs;
         fespace.GetBdrElementDofs(i, dofs);
         for (auto k : dofs)
         {
            int vdof = fespace.DofToVDof(k, 0);
            int tdof = fespace.GetLocalTDofNumber(vdof);
            if (tdof >= 0) { constrained_tdofs.insert(tdof); }
         }
      }
   }

   std::map<int, int> dof_constraint;
   int n_constraints = 0;
   for (auto k : constrained_tdofs)
   {
      dof_constraint[k] = n_constraints++;
   }
   SparseMatrix * out = new SparseMatrix(n_constraints, fespace.GetTrueVSize());

   int constraint_running_total = 0;
   MPI_Scan(&n_constraints, &constraint_running_total, 1, MPI_INT,
            MPI_SUM, fespace.GetComm());
   int global_constraints = 0;
   if (rank == size - 1) global_constraints = constraint_running_total;
   MPI_Bcast(&global_constraints, 1, MPI_INT, size - 1, fespace.GetComm());

   Vector nor(dim);
   for (int i = 0; i < fespace.GetNBE(); ++i)
   {
      int att = fespace.GetBdrAttribute(i);
      if (constrained_att.FindSorted(att) != -1)
      {
         ElementTransformation * Tr = fespace.GetBdrElementTransformation(i);
         const FiniteElement * fe = fespace.GetBE(i);
         const IntegrationRule& nodes = fe->GetNodes();

         Array<int> dofs;
         fespace.GetBdrElementDofs(i, dofs);
         MFEM_VERIFY(dofs.Size() == nodes.Size(),
                     "Something wrong in finite element space!");

         for (int j = 0; j < dofs.Size(); ++j)
         {
            Tr->SetIntPoint(&nodes[j]);
            // the normal returned in the next line is scaled by h, which
            // is probably what we want in this application
            CalcOrtho(Tr->Jacobian(), nor);

            // next line assumes nodes and dofs are ordered the same, which
            // seems to be true
            int k = dofs[j];
            int vdof = fespace.DofToVDof(k, 0);
            int truek = fespace.GetLocalTDofNumber(vdof);
            if (truek >= 0)
            {
               int constraint = dof_constraint[truek];
               for (int d = 0; d < dim; ++d)
               {
                  int vdof = fespace.DofToVDof(k, d);
                  int truek = fespace.GetLocalTDofNumber(vdof);
                  out->Set(constraint, truek, nor[d]);
                  // d_dofs[d]->Append(vdof);
               }
            }
         }
      }
   }

   out->Finalize();

   // cols are same as for fespace; rows are built here
   HYPRE_Int glob_num_rows = global_constraints;
   HYPRE_Int glob_num_cols = fespace.GlobalTrueVSize();
   HYPRE_Int row_starts[2] = {constraint_running_total - n_constraints,
                              constraint_running_total};
   HYPRE_Int * col_starts = fespace.GetTrueDofOffsets();
   HypreParMatrix * h_out = new HypreParMatrix(fespace.GetComm(), glob_num_rows,
                                               glob_num_cols, row_starts,
                                               col_starts, out);
   h_out->CopyRowStarts();
   h_out->CopyColStarts();

   return h_out;
}

Mesh * build_trapezoid_mesh()
{
   // starting with Extrude1D(), which is maybe not the right thing
   //       mesh2d = new Mesh(2, nvt, mesh->GetNE()*ny, mesh->GetNBE()*ny);

   const int dimension = 2;
   const int nvt = 4; // vertices
   const int nbe = 4; // num boundary elements
   Mesh * mesh = new Mesh(dimension, nvt, 1, nbe);

   // vertices
   double vc[dimension];
   vc[0] = 0.0; vc[1] = 0.0;
   mesh->AddVertex(vc);
   vc[0] = 1.0; vc[1] = 0.0;
   mesh->AddVertex(vc);
   vc[0] = 0.3; vc[1] = 1.0;
   mesh->AddVertex(vc);
   vc[0] = 1.0; vc[1] = 1.0;
   mesh->AddVertex(vc);

   // element
   Array<int> vert(4);
   // for (int i = 0; i < 4; ++i) { vert[i] = i; }
   vert[0] = 0; vert[1] = 1; vert[2] = 3; vert[3] = 2;
   mesh->AddQuad(vert, 1);

   // Swap<int>(sv[0], sv[1]);

   // boundary
   Array<int> sv(2);
   sv[0] = 0; sv[1] = 1;
   mesh->AddBdrSegment(sv, 1);
   sv[0] = 1; sv[1] = 3;
   mesh->AddBdrSegment(sv, 2);
   sv[0] = 2; sv[1] = 3;
   mesh->AddBdrSegment(sv, 3);
   sv[0] = 0; sv[1] = 2;
   mesh->AddBdrSegment(sv, 4);

   mesh->FinalizeQuadMesh(1, 0, true);

   return mesh;
}

int main(int argc, char *argv[])
{
   // 1. Initialize MPI.
   int num_procs, myid;
   MPI_Init(&argc, &argv);
   MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
   MPI_Comm_rank(MPI_COMM_WORLD, &myid);

   // 2. Parse command-line options.
   int order = 1;
   bool static_cond = false;
   bool visualization = 1;
   bool amg_elast = 0;
   bool reorder_space = false;

   OptionsParser args(argc, argv);
   args.AddOption(&order, "-o", "--order",
                  "Finite element order (polynomial degree).");
   args.AddOption(&amg_elast, "-elast", "--amg-for-elasticity", "-sys",
                  "--amg-for-systems",
                  "Use the special AMG elasticity solver (GM/LN approaches), "
                  "or standard AMG for systems (unknown approach).");
   args.AddOption(&static_cond, "-sc", "--static-condensation", "-no-sc",
                  "--no-static-condensation", "Enable static condensation.");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.AddOption(&reorder_space, "-nodes", "--by-nodes", "-vdim", "--by-vdim",
                  "Use byNODES ordering of vector space instead of byVDIM");
   args.Parse();
   if (!args.Good())
   {
      if (myid == 0)
      {
         args.PrintUsage(cout);
      }
      MPI_Finalize();
      return 1;
   }
   if (myid == 0)
   {
      args.PrintOptions(cout);
   }

   // 3. Read the (serial) mesh from the given mesh file on all processors.  We
   //    can handle triangular, quadrilateral, tetrahedral, hexahedral, surface
   //    and volume meshes with the same code.
   Mesh *mesh = build_trapezoid_mesh();
   int dim = mesh->Dimension();

   // 5. Refine the serial mesh on all processors to increase the resolution. In
   //    this example we do 'ref_levels' of uniform refinement. We choose
   //    'ref_levels' to be the largest number that gives a final mesh with no
   //    more than 1,000 elements.
   {
      int ref_levels =
         (int)floor(log(1000./mesh->GetNE())/log(2.)/dim);
      for (int l = 0; l < ref_levels; l++)
      {
         mesh->UniformRefinement();
      }
   }

   // 6. Define a parallel mesh by a partitioning of the serial mesh. Refine
   //    this mesh further in parallel to increase the resolution. Once the
   //    parallel mesh is defined, the serial mesh can be deleted.
   ParMesh *pmesh = new ParMesh(MPI_COMM_WORLD, *mesh);
   delete mesh;
   {
      int par_ref_levels = 1;
      for (int l = 0; l < par_ref_levels; l++)
      {
         pmesh->UniformRefinement();
      }
   }

   // 7. Define a parallel finite element space on the parallel mesh. Here we
   //    use vector finite elements, i.e. dim copies of a scalar finite element
   //    space. We use the ordering by vector dimension (the last argument of
   //    the FiniteElementSpace constructor) which is expected in the systems
   //    version of BoomerAMG preconditioner. For NURBS meshes, we use the
   //    (degree elevated) NURBS space associated with the mesh nodes.
   FiniteElementCollection *fec;
   ParFiniteElementSpace *fespace;
   const bool use_nodal_fespace = pmesh->NURBSext && !amg_elast;
   if (use_nodal_fespace)
   {
      fec = NULL;
      fespace = (ParFiniteElementSpace *)pmesh->GetNodes()->FESpace();
   }
   else
   {
      fec = new H1_FECollection(order, dim);
      if (reorder_space)
      {
         fespace = new ParFiniteElementSpace(pmesh, fec, dim, Ordering::byNODES);
      }
      else
      {
         fespace = new ParFiniteElementSpace(pmesh, fec, dim, Ordering::byVDIM);
      }
   }
   HYPRE_Int size = fespace->GlobalTrueVSize();
   if (myid == 0)
   {
      cout << "Number of finite element unknowns: " << size << endl
           << "Assembling: " << flush;
   }

   // 8. Determine the list of true (i.e. parallel conforming) essential
   //    boundary dofs. In this example, the boundary conditions are defined by
   //    marking only boundary attribute 1 from the mesh as essential and
   //    converting it to a list of true dofs.
   Array<int> ess_tdof_list, ess_bdr(pmesh->bdr_attributes.Max());
   ess_bdr = 0;
   ess_bdr[0] = 1; // bottom
   // ess_bdr[3] = 1; // left side
   fespace->GetEssentialTrueDofs(ess_bdr, ess_tdof_list);

   // 9. Set up the parallel linear form b(.) which corresponds to the
   //    right-hand side of the FEM linear system. In this case, b_i equals the
   //    boundary integral of f*phi_i where f represents a "pull down" force on
   //    the Neumann part of the boundary and phi_i are the basis functions in
   //    the finite element fespace. The force is defined by the object f, which
   //    is a vector of Coefficient objects. The fact that f is non-zero on
   //    boundary attribute 2 is indicated by the use of piece-wise constants
   //    coefficient for its last component.
   VectorArrayCoefficient f(dim);
   for (int i = 0; i < dim-1; i++)
   {
      f.Set(i, new ConstantCoefficient(0.0));
   }

   // try to put a leftward force on the right side of the trapezoid
   {
      Vector pull_force(pmesh->bdr_attributes.Max());
      pull_force = 0.0;
      pull_force(1) = -5.0e-2; // index 1 attribute 2
      f.Set(0, new PWConstCoefficient(pull_force));
   }

   // 10. Set up constraint matrix to constrain normal displacement (but
   //     allow tangential displacement) on specified boundaries.
   Array<int> constraint_atts(1);
   constraint_atts[0] = 4;  // attribute 4 left side
   HypreParMatrix * hconstraints = BuildNormalConstraints(*fespace,
                                                          constraint_atts);

   ParLinearForm *b = new ParLinearForm(fespace);
   b->AddBoundaryIntegrator(new VectorBoundaryLFIntegrator(f));
   if (myid == 0)
   {
      cout << "r.h.s. ... " << flush;
   }
   b->Assemble();

   // 11. Define the solution vector x as a parallel finite element grid
   //     function corresponding to fespace. Initialize x with initial guess of
   //     zero, which satisfies the boundary conditions.
   ParGridFunction x(fespace);
   x = 0.0;

   // 12. Set up the parallel bilinear form a(.,.) on the finite element space
   //     corresponding to the linear elasticity integrator with piece-wise
   //     constants coefficient lambda and mu.
   Vector lambda(pmesh->attributes.Max());
   lambda = 1.0;
   // lambda(0) = lambda(1)*50;
   PWConstCoefficient lambda_func(lambda);
   Vector mu(pmesh->attributes.Max());
   mu = 1.0;
   // mu(0) = mu(1)*50;
   PWConstCoefficient mu_func(mu);

   ParBilinearForm *a = new ParBilinearForm(fespace);
   a->AddDomainIntegrator(new ElasticityIntegrator(lambda_func, mu_func));

   // 12. Assemble the parallel bilinear form and the corresponding linear
   //     system, applying any necessary transformations such as: parallel
   //     assembly, eliminating boundary conditions, applying conforming
   //     constraints for non-conforming AMR, static condensation, etc.
   if (myid == 0) { cout << "matrix ... " << flush; }
   if (static_cond) { a->EnableStaticCondensation(); }
   a->Assemble();

   HypreParMatrix A;
   Vector B, X;
   a->FormLinearSystem(ess_tdof_list, x, *b, A, X, B);
   if (myid == 0)
   {
      cout << "done." << endl;
      cout << "Size of linear system: " << A.GetGlobalNumRows() << endl;
   }

   // 13. Define and apply a parallel PCG solver for A X = B with the BoomerAMG
   //     preconditioner from hypre.
   SparseMatrix local_constraints;
   hconstraints->GetDiag(local_constraints);
   {
      std::ofstream out("localconstraints.sparsematrix");
      local_constraints.Print(out, 1);
   }
   Array<int> lagrange_rowstarts(local_constraints.Height() + 1);
   for (int k = 0; k < local_constraints.Height() + 1; ++k)
   {
      lagrange_rowstarts[k] = k;
   }
   EliminationCGSolver * solver = new EliminationCGSolver(A, local_constraints,
                                                          lagrange_rowstarts, dim,
                                                          false);
   solver->SetRelTol(1e-8);
   solver->SetMaxIter(500);
   solver->SetPrintLevel(1);
   solver->Mult(B, X);

   // 14. Recover the parallel grid function corresponding to X. This is the
   //     local finite element solution on each processor.
   a->RecoverFEMSolution(X, *b, x);

   // 15. For non-NURBS meshes, make the mesh curved based on the finite element
   //     space. This means that we define the mesh elements through a fespace
   //     based transformation of the reference element.  This allows us to save
   //     the displaced mesh as a curved mesh when using high-order finite
   //     element displacement field. We assume that the initial mesh (read from
   //     the file) is not higher order curved mesh compared to the chosen FE
   //     space.
   if (!use_nodal_fespace)
   {
      pmesh->SetNodalFESpace(fespace);
   }

   // 16. Save in parallel the displaced mesh and the inverted solution (which
   //     gives the backward displacements to the original grid). This output
   //     can be viewed later using GLVis: "glvis -np <np> -m mesh -g sol".
   {
      GridFunction *nodes = pmesh->GetNodes();
      *nodes += x;
      x *= -1;

      ostringstream mesh_name, sol_name;
      mesh_name << "mesh." << setfill('0') << setw(6) << myid;
      sol_name << "sol." << setfill('0') << setw(6) << myid;

      ofstream mesh_ofs(mesh_name.str().c_str());
      mesh_ofs.precision(8);
      pmesh->Print(mesh_ofs);

      ofstream sol_ofs(sol_name.str().c_str());
      sol_ofs.precision(8);
      x.Save(sol_ofs);
   }

   // 18. Save the refined mesh and the solution in VisIt format.
   {
      // todo: might make more sense to .SetCycle() than to append boundary_attribute to name
      std::stringstream visitname;
      visitname << "trapezoid"; // could add a number?
      VisItDataCollection visit_dc(MPI_COMM_WORLD, visitname.str(), pmesh);
      visit_dc.SetLevelsOfDetail(4);
      visit_dc.RegisterField("displacement", &x); // do we need to do (or undo) the += stuff from above?
      // visit_dc.SetCycle(boundary_attribute);
      visit_dc.Save();
   }

   // 19. Send the above data by socket to a GLVis server.  Use the "n" and "b"
   //     keys in GLVis to visualize the displacements.
   if (visualization)
   {
      char vishost[] = "localhost";
      int  visport   = 19916;
      socketstream sol_sock(vishost, visport);
      sol_sock << "parallel " << num_procs << " " << myid << "\n";
      sol_sock.precision(8);
      sol_sock << "solution\n" << *pmesh << x << flush;
   }

   // 18. Free the used memory.
   delete solver;
   // delete amg;
   delete a;
   delete b;
   if (fec)
   {
      delete fespace;
      delete fec;
   }
   delete pmesh;

   MPI_Finalize();

   return 0;
}
