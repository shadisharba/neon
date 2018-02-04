
#include "femStaticMatrix.hpp"

#include "Exceptions.hpp"
#include "solver/linear/LinearSolverFactory.hpp"

#include "io/json.hpp"
#include <chrono>
#include <omp.h>

namespace neon::diffusion
{
femStaticMatrix::femStaticMatrix(femMesh& fem_mesh, json const& simulation_data)
    : fem_mesh(fem_mesh),
      f(vector::Zero(fem_mesh.active_dofs())),
      d(vector::Zero(fem_mesh.active_dofs())),
      file_io(simulation_data["Name"].get<std::string>(), simulation_data["Visualisation"], fem_mesh),
      linear_solver(make_linear_solver(simulation_data["LinearSolver"]))
{
}

femStaticMatrix::~femStaticMatrix() = default;

void femStaticMatrix::compute_sparsity_pattern()
{
    std::vector<Doublet<int>> doublets;
    doublets.reserve(fem_mesh.active_dofs());

    K.resize(fem_mesh.active_dofs(), fem_mesh.active_dofs());

    for (auto const& submesh : fem_mesh.meshes())
    {
        // Loop over the elements and add in the non-zero components
        for (auto element = 0; element < submesh.elements(); element++)
        {
            for (auto const& p : submesh.local_dof_list(element))
            {
                for (auto const& q : submesh.local_dof_list(element))
                {
                    doublets.emplace_back(p, q);
                }
            }
        }
    }
    K.setFromTriplets(doublets.begin(), doublets.end());
    is_sparsity_computed = true;
}

void femStaticMatrix::compute_external_force(double const load_factor)
{
    auto const start = std::chrono::high_resolution_clock::now();

    f.setZero();

    for (auto const& [name, surfaces] : fem_mesh.surface_boundaries())
    {
        for (auto const& surface : surfaces)
        {
            for (auto const& mesh : surface.load_interface())
            {
                for (auto element = 0; element < mesh.elements(); ++element)
                {
                    auto const& [dofs, fe] = mesh.external_force(element, load_factor);

                    for (auto a = 0; a < fe.size(); ++a)
                    {
                        f(dofs[a]) += fe(a);
                    }
                }
            }
            for (auto const& mesh : surface.stiffness_load_interface())
            {
                for (auto element = 0; element < mesh.elements(); ++element)
                {
                    auto const& [dofs, fe] = mesh.external_force(element, load_factor);
                    auto const& [_, ke] = mesh.external_stiffness(element, load_factor);

                    for (auto a = 0; a < fe.size(); ++a)
                    {
                        f(dofs[a]) += fe(a);

                        for (auto b = 0; b < fe.size(); ++b)
                        {
                            K.coeffRef(dofs[a], dofs[b]) += ke(a, b);
                        }
                    }
                }
            }
        }
    }
    auto const end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end - start;
    std::cout << std::string(6, ' ') << "External forces assembly took " << elapsed_seconds.count()
              << "s\n";
}

void femStaticMatrix::solve()
{
    std::cout << std::string(4, ' ') << "Linear equation system has " << fem_mesh.active_dofs()
              << " degrees of freedom\n";

    assemble_stiffness();

    compute_external_force();

    apply_dirichlet_conditions(K, d, f);

    linear_solver->solve(K, d, f);

    file_io.write(0, 0.0, d);
}

void femStaticMatrix::assemble_stiffness()
{
    if (!is_sparsity_computed) compute_sparsity_pattern();

    auto const start = std::chrono::high_resolution_clock::now();

    K.coeffs() = 0.0;

    for (auto const& submesh : fem_mesh.meshes())
    {
#pragma omp parallel for
        for (auto element = 0; element < submesh.elements(); ++element)
        {
            // auto const[dofs, ke] = submesh.tangent_stiffness(element);
            auto const& tpl = submesh.tangent_stiffness(element);
            auto const& dofs = std::get<0>(tpl);
            auto const& ke = std::get<1>(tpl);

            for (auto b = 0; b < dofs.size(); b++)
            {
                for (auto a = 0; a < dofs.size(); a++)
                {
#pragma omp atomic
                    K.coeffRef(dofs[a], dofs[b]) += ke(a, b);
                }
            }
        }
    }

    auto const end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end - start;

    std::cout << std::string(6, ' ') << "Stiffness assembly took " << elapsed_seconds.count()
              << "s\n";
}

void femStaticMatrix::apply_dirichlet_conditions(sparse_matrix& A, vector& x, vector& b)
{
    for (auto const& [name, dirichlet_boundaries] : fem_mesh.dirichlet_boundaries())
    {
        for (auto const& dirichlet_boundary : dirichlet_boundaries)
        {
            for (auto const& fixed_dof : dirichlet_boundary.dof_view())
            {
                auto const diagonal_entry = A.coeffRef(fixed_dof, fixed_dof);

                x(fixed_dof) = dirichlet_boundary.value_view();

                std::vector<int> non_zero_visitor;

                for (sparse_matrix::InnerIterator it(A, fixed_dof); it; ++it)
                {
                    if (!A.IsRowMajor) b(it.row()) -= it.valueRef() * x(fixed_dof);

                    it.valueRef() = 0.0;

                    non_zero_visitor.push_back(A.IsRowMajor ? it.col() : it.row());
                }

                for (auto const& non_zero : non_zero_visitor)
                {
                    auto const row = A.IsRowMajor ? non_zero : fixed_dof;
                    auto const col = A.IsRowMajor ? fixed_dof : non_zero;

                    if (A.IsRowMajor) b(row) -= A.coeffRef(row, col) * x(fixed_dof);

                    A.coeffRef(row, col) = 0.0;
                }

                // Reset the diagonal to the same value to preserve condition number
                A.coeffRef(fixed_dof, fixed_dof) = diagonal_entry;
                b(fixed_dof) = diagonal_entry * x(fixed_dof);
            }
        }
    }
}
}
