
#include "small_strain_J2_plasticity.hpp"

#include "constitutive/internal_variables.hpp"
#include "exceptions.hpp"
#include "numeric/mechanics"

#include <range/v3/view/transform.hpp>
#include <tbb/parallel_for.h>

#include <iostream>

namespace neon::mechanics::solid
{
small_strain_J2_plasticity::small_strain_J2_plasticity(std::shared_ptr<internal_variables_t>& variables,
                                                       json const& material_data)
    : isotropic_linear_elasticity(variables, material_data), material(material_data)
{
    variables->add(variable::second::linearised_plastic_strain);
    variables->add(variable::scalar::effective_plastic_strain);

    names.emplace("linearised_plastic_strain");
    names.emplace("effective_plastic_strain");

    variables->commit();
}

small_strain_J2_plasticity::~small_strain_J2_plasticity() = default;

void small_strain_J2_plasticity::update_internal_variables(double)
{
    auto const shear_modulus = material.shear_modulus();

    // Extract the internal variables
    auto& plastic_strains = variables->get(variable::second::linearised_plastic_strain);
    auto& strains = variables->get(variable::second::linearised_strain);
    auto& accumulated_plastic_strains = variables->get(variable::scalar::effective_plastic_strain);

    auto& cauchy_stresses = variables->get(variable::second::cauchy_stress);
    auto& von_mises_stresses = variables->get(variable::scalar::von_mises_stress);

    auto& tangent_operators = variables->get(variable::fourth::tangent_operator);

    // Compute the linear strain gradient from the displacement gradient
    strains = variables->get(variable::second::displacement_gradient)
              | ranges::view::transform([](auto const& H) { return 0.5 * (H + H.transpose()); });

    // Perform the update algorithm for each quadrature point
    tbb::parallel_for(std::size_t{0}, strains.size(), [&](auto const index) {
        auto const& strain = strains[index];
        auto& plastic_strain = plastic_strains[index];
        auto& cauchy_stress = cauchy_stresses[index];
        auto& accumulated_plastic_strain = accumulated_plastic_strains[index];
        auto& von_mises = von_mises_stresses[index];

        // Elastic stress predictor
        cauchy_stress = compute_cauchy_stress(material.shear_modulus(),
                                              material.lambda(),
                                              strain - plastic_strain);

        // Trial von Mises stress
        von_mises = von_mises_stress(cauchy_stress);

        // If this quadrature point is elastic, then set the tangent to the
        // elastic modulus and continue to the next quadrature point
        if (evaluate_yield_function(von_mises, accumulated_plastic_strain) <= 0.0)
        {
            tangent_operators[index] = C_e;
            return;
        }

        auto const von_mises_trial = von_mises;

        // Compute the normal direction to the yield surface which remains
        // constant throughout the radial return method
        matrix3 const normal = deviatoric(cauchy_stress) / deviatoric(cauchy_stress).norm();

        auto const plastic_increment = perform_radial_return(von_mises, accumulated_plastic_strain);

        plastic_strain += plastic_increment * std::sqrt(3.0 / 2.0) * normal;

        cauchy_stress -= 2.0 * shear_modulus * plastic_increment * std::sqrt(3.0 / 2.0) * normal;

        von_mises = von_mises_stress(cauchy_stress);

        accumulated_plastic_strain += plastic_increment;

        tangent_operators[index] = algorithmic_tangent(plastic_increment,
                                                       accumulated_plastic_strain,
                                                       von_mises_trial,
                                                       normal);
    });
}

matrix6 small_strain_J2_plasticity::algorithmic_tangent(double const plastic_increment,
                                                        double const accumulated_plastic_strain,
                                                        double const von_mises,
                                                        matrix3 const& normal) const
{
    auto const G = material.shear_modulus();
    auto const H = material.hardening_modulus(accumulated_plastic_strain);

    return C_e - plastic_increment * 6.0 * std::pow(G, 2) / von_mises * I_dev
           + 6.0 * std::pow(G, 2) * (plastic_increment / von_mises - 1.0 / (3.0 * G + H))
                 * outer_product(normal, normal);
}

double small_strain_J2_plasticity::perform_radial_return(double const von_mises,
                                                         double const accumulated_plastic_strain) const
{
    auto const shear_modulus = material.shear_modulus();

    auto plastic_increment{0.0};

    auto f = evaluate_yield_function(von_mises, accumulated_plastic_strain);

    // Perform the non-linear hardening solve
    int iterations{0};
    auto constexpr max_iterations{50};
    while (f > 1.0e-6 && iterations < max_iterations)
    {
        auto const H = material.hardening_modulus(accumulated_plastic_strain + plastic_increment);

        auto const plastic_increment_delta = f / (3.0 * shear_modulus + H);

        plastic_increment += plastic_increment_delta;

        f = evaluate_yield_function(von_mises, accumulated_plastic_strain, plastic_increment);

        iterations++;
    }
    if (iterations == max_iterations)
    {
        std::cout << "\n";
        std::cout << std::string(8, ' ') << "Plastic increment : " << plastic_increment << "\n";

        std::cout << std::string(8, ' ')
                  << "Accumulated plastic strain : " << accumulated_plastic_strain << "\n";

        std::cout << std::string(8, ' ') << "Hardening modulus : "
                  << material.hardening_modulus(accumulated_plastic_strain + plastic_increment)
                  << "\n";

        std::cout << std::string(8, ' ') << "Shear modulus : " << shear_modulus << "\n";

        std::cout << std::string(8, ' ') << "Yield function after mapping : " << f << "\n";

        std::cout << std::string(8, ' ')
                  << "Current yield stress : " << material.yield_stress(accumulated_plastic_strain)
                  << "\n";

        throw computational_error("Non-convergence in radial return method.");
    }
    return plastic_increment;
}

double small_strain_J2_plasticity::evaluate_yield_function(double const von_mises,
                                                           double const accumulated_plastic_strain,
                                                           double const plastic_increment) const
{
    return (von_mises - 3.0 * material.shear_modulus() * plastic_increment)
           - material.yield_stress(accumulated_plastic_strain + plastic_increment);
}
}
