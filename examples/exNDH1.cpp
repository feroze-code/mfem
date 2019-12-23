//                       MFEM Example 3 - Parallel Version
//
// Compile with: make exNDH1
//
// Sample runs:  mpirun -np 4 exNDH1 -m ../data/star.mesh
//               mpirun -np 4 exNDH1 -m ../data/square-disc.mesh -o 2
//               mpirun -np 4 exNDH1 -m ../data/beam-tet.mesh
//               mpirun -np 4 exNDH1 -m ../data/beam-hex.mesh
//               mpirun -np 4 exNDH1 -m ../data/escher.mesh
//               mpirun -np 4 exNDH1 -m ../data/escher.mesh -o 2
//               mpirun -np 4 exNDH1 -m ../data/fichera.mesh
//               mpirun -np 4 exNDH1 -m ../data/fichera-q2.vtk
//               mpirun -np 4 exNDH1 -m ../data/fichera-q3.mesh
//               mpirun -np 4 exNDH1 -m ../data/square-disc-nurbs.mesh
//               mpirun -np 4 exNDH1 -m ../data/beam-hex-nurbs.mesh
//               mpirun -np 4 exNDH1 -m ../data/amr-quad.mesh -o 2
//               mpirun -np 4 exNDH1 -m ../data/amr-hex.mesh
//               mpirun -np 4 exNDH1 -m ../data/star-surf.mesh -o 2
//               mpirun -np 4 exNDH1 -m ../data/mobius-strip.mesh -o 2 -f 0.1
//               mpirun -np 4 exNDH1 -m ../data/klein-bottle.mesh -o 2 -f 0.1
//
// Description:  This example code solves a projection of a gradient of a
//               function in H^1 to H(curl).
//
//               We recommend viewing examples 1-3 before viewing this example.

#include "mfem.hpp"
#include <fstream>
#include <iostream>

using namespace std;
using namespace mfem;

double p_exact(const Vector &x);
void gradp_exact(const Vector &, Vector &);

int dim;

int main(int argc, char *argv[])
{
   // 1. Initialize MPI.
   int num_procs, myid;
   MPI_Init(&argc, &argv);
   MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
   MPI_Comm_rank(MPI_COMM_WORLD, &myid);

   // 2. Parse command-line options.
   const char *mesh_file = "../data/beam-hex.mesh";
   int order = 1;
   bool static_cond = false;
   bool pa = false;
   bool visualization = 1;

   OptionsParser args(argc, argv);
   args.AddOption(&mesh_file, "-m", "--mesh",
                  "Mesh file to use.");
   args.AddOption(&order, "-o", "--order",
                  "Finite element order (polynomial degree).");
   args.AddOption(&static_cond, "-sc", "--static-condensation", "-no-sc",
                  "--no-static-condensation", "Enable static condensation.");
   args.AddOption(&pa, "-pa", "--partial-assembly", "-no-pa",
                  "--no-partial-assembly", "Enable Partial Assembly.");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");

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
   Mesh *mesh = new Mesh(mesh_file, 1, 1);
   dim = mesh->Dimension();
   int sdim = mesh->SpaceDimension();

   // 4. Refine the serial mesh on all processors to increase the resolution. In
   //    this example we do 'ref_levels' of uniform refinement. We choose
   //    'ref_levels' to be the largest number that gives a final mesh with no
   //    more than 1,000 elements.
   {
      int ref_levels = (int)floor(log(1000./mesh->GetNE())/log(2.)/dim);
      for (int l = 0; l < ref_levels; l++)
      {
         mesh->UniformRefinement();
      }
   }

   // 5. Define a parallel mesh by a partitioning of the serial mesh. Refine
   //    this mesh further in parallel to increase the resolution. Once the
   //    parallel mesh is defined, the serial mesh can be deleted. Tetrahedral
   //    meshes need to be reoriented before we can define high-order Nedelec
   //    spaces on them.
   ParMesh *pmesh = new ParMesh(MPI_COMM_WORLD, *mesh);
   delete mesh;
   {
      int par_ref_levels = 1;
      for (int l = 0; l < par_ref_levels; l++)
      {
         pmesh->UniformRefinement();
      }
   }
   pmesh->ReorientTetMesh();

   // 6. Define a parallel finite element space on the parallel mesh. Here we
   //    use the Nedelec finite elements of the specified order.
   FiniteElementCollection *fec = new ND_FECollection(order, dim);
   FiniteElementCollection *H1fec = new H1_FECollection(order, dim);
   ParFiniteElementSpace *fespace = new ParFiniteElementSpace(pmesh, fec);
   ParFiniteElementSpace *H1fespace = new ParFiniteElementSpace(pmesh, H1fec);
   HYPRE_Int size = fespace->GlobalTrueVSize();
   if (myid == 0)
   {
      cout << "Number of finite element unknowns: " << size << endl;
   }

   // 7. Define the solution vector x as a parallel finite element grid function
   //    corresponding to fespace. Initialize x by projecting the exact
   //    solution. Note that only values from the boundary edges will be used
   //    when eliminating the non-homogeneous boundary condition to modify the
   //    r.h.s. vector b.
   ParGridFunction x(fespace);
   FunctionCoefficient p_coef(p_exact);
   ParGridFunction p(H1fespace);
   p.ProjectCoefficient(p_coef);

   VectorFunctionCoefficient gradp_coef(sdim, gradp_exact);

   // 8. Set up the parallel bilinear form corresponding to the EM diffusion
   //    operator curl muinv curl + sigma I, by adding the curl-curl and the
   //    mass domain integrators.
   Coefficient *muinv = new ConstantCoefficient(1.0);
   Coefficient *sigma = new ConstantCoefficient(1.0);
   ParBilinearForm *a = new ParBilinearForm(fespace);
   ParMixedBilinearForm *a_NDH1 = new ParMixedBilinearForm(H1fespace, fespace);
   if (pa)
   {
      a->SetAssemblyLevel(AssemblyLevel::PARTIAL);
      a_NDH1->SetAssemblyLevel(AssemblyLevel::PARTIAL);
   }

   a->AddDomainIntegrator(new VectorFEMassIntegrator(*sigma));

   a_NDH1->AddDomainIntegrator(new MixedVectorGradientIntegrator(*muinv));

   // 9. Assemble the parallel bilinear form and the corresponding linear
   //    system, applying any necessary transformations such as: parallel
   //    assembly, eliminating boundary conditions, applying conforming
   //    constraints for non-conforming AMR, static condensation, etc.
   if (static_cond) { a->EnableStaticCondensation(); }

   a->Assemble();
   if (!pa) { a->Finalize(); }

   a_NDH1->Assemble();
   if (!pa) { a_NDH1->Finalize(); }

   Vector B(fespace->GetTrueVSize());
   Vector X(fespace->GetTrueVSize());

   if (pa)
   {
      a_NDH1->Mult(p, x);
      x.GetTrueDofs(B);
   }
   else
   {
      HypreParMatrix *NDH1 = a_NDH1->ParallelAssemble();

      Vector P(H1fespace->GetTrueVSize());
      p.GetTrueDofs(P);

      NDH1->Mult(P,B);

      delete NDH1;
   }

   // 10. Define and apply a parallel PCG solver for AX=B with the AMS
   //     preconditioner from hypre, in the full assembly case.
   //     With partial assembly, use Jacobi preconditioner, for now.

   if (pa)
   {
      ParGridFunction diag_pa(fespace);
      diag_pa = 0.0;
      a->AssembleDiagonal(diag_pa);

      Vector tdiag_pa(fespace->GetTrueVSize());
      diag_pa.GetTrueDofs(tdiag_pa);

      Array<int> ess_tdof_list;
      OperatorJacobiSmoother Jacobi(diag_pa, ess_tdof_list, 1.0);

      CGSolver cg(MPI_COMM_WORLD);
      cg.SetRelTol(1e-12);
      cg.SetMaxIter(1000);
      cg.SetPrintLevel(1);
      cg.SetOperator(*a);
      cg.SetPreconditioner(Jacobi);

      ParGridFunction rhs(fespace);
      rhs = x;
      cg.Mult(rhs, x);
   }
   else
   {
      HypreParMatrix *Amat = a->ParallelAssemble();
      HyprePCG *pcg = new HyprePCG(*Amat);
      pcg->SetTol(1e-12);
      pcg->SetMaxIter(500);
      pcg->SetPrintLevel(2);
      pcg->Mult(B, X);

      delete pcg;
      delete Amat;

      x.SetFromTrueDofs(X);
   }

   // 11. Compute and print the L^2 norm of the error.
   {
      double err = x.ComputeL2Error(gradp_coef);
      if (myid == 0)
      {
         cout << "\n|| E_h - E ||_{L^2} = " << err << '\n' << endl;
      }
   }

   // 12. Save the refined mesh and the solution in parallel. This output can
   //     be viewed later using GLVis: "glvis -np <np> -m mesh -g sol".
   {
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

   // 13. Send the solution by socket to a GLVis server.
   if (visualization)
   {
      char vishost[] = "localhost";
      int  visport   = 19916;
      socketstream sol_sock(vishost, visport);
      sol_sock << "parallel " << num_procs << " " << myid << "\n";
      sol_sock.precision(8);
      sol_sock << "solution\n" << *pmesh << x << flush;
   }

   // 14. Free the used memory.
   delete a;
   delete a_NDH1;
   delete sigma;
   delete muinv;
   delete fespace;
   delete H1fespace;
   delete fec;
   delete H1fec;
   delete pmesh;

   MPI_Finalize();

   return 0;
}

double p_exact(const Vector &x)
{
   if (dim == 3)
   {
      return sin(x(0)) * sin(x(1)) * sin(x(2));
   }
   else if (dim == 2)
   {
      return sin(x(0)) * sin(x(1));
   }

   return 0.0;
}

void gradp_exact(const Vector &x, Vector &f)
{
   if (dim == 3)
   {
      f(0) = cos(x(0)) * sin(x(1)) * sin(x(2));
      f(1) = sin(x(0)) * cos(x(1)) * sin(x(2));
      f(2) = sin(x(0)) * sin(x(1)) * cos(x(2));
   }
   else
   {
      f(0) = cos(x(0)) * sin(x(1));
      f(1) = sin(x(0)) * cos(x(1));
      if (x.Size() == 3) { f(2) = 0.0; }
   }
}
