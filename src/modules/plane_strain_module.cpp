
#include "modules/plane_strain_module.hpp"

namespace neon
{
plane_strain_module::plane_strain_module(basic_mesh const& mesh,
                                         json const& material,
                                         json const& simulation)
    : fem_mesh(mesh,
               material,
               simulation["meshes"].front(),
               simulation["time"]["increments"]["initial"]),
      fem_matrix(fem_mesh, simulation)
{
}
}
