
#pragma once

#include "constitutive/InternalVariables.hpp"
#include "mesh/NodeOrderingAdapter.hpp"
#include "mesh/diffusion/femMesh.hpp"
#include "mesh/mechanical/solid/femMesh.hpp"

#include <map>
#include <string>
#include <unordered_set>

#include "vtkIdList.h"
#include "vtkSmartPointer.h"

#include <json/value.h>

#include "vtkDoubleArray.h"
#include "vtkIdTypeArray.h"
#include "vtkPointData.h"
#include "vtkUnstructuredGrid.h"
#include "vtkXMLUnstructuredGridWriter.h"

namespace neon
{
class FileIO
{
public:
    explicit FileIO(std::string file_name, Json::Value const& visualisation_data);

    virtual ~FileIO();

    FileIO(FileIO&&) = default;

protected:
    virtual void write_to_file(int const time_step, double const total_time);

    /** Write out the field to a vtk file */
    void add_field(std::string const& name, vector const& data, int const components);

protected:
    std::string const directory_name = "visualisation";
    std::string file_name;

    vtkSmartPointer<vtkUnstructuredGrid> unstructured_mesh;

    std::ofstream pvd_file; //!< Stream for writing time history

    std::unordered_set<std::string> output_set;

    int write_every = 1; //!< Time steps to write out (e.g. two is every second time step)
    bool use_binary_format = true;
};

namespace mechanical
{
template <class femMeshType>
class FileIO : public neon::FileIO
{
public:
    using fem_mesh_type = femMeshType;

    using variable_type = typename fem_mesh_type::internal_variable_type;

    using ScalarMap = std::map<std::string, typename variable_type::Scalar>;
    using TensorMap = std::map<std::string, typename variable_type::Tensor>;

public:
    std::string const primary_field{"Displacement"};

public:
    explicit FileIO(std::string file_name,
                    Json::Value const& visualisation_data,
                    fem_mesh_type const& fem_mesh);

    FileIO(FileIO&&) = default;

    void write(int const time_step, double const total_time);

private:
    void add_mesh();

private:
    fem_mesh_type const& fem_mesh;

    // clang-format off
    ScalarMap const scalar_map{{"AccumulatedPlasticStrain", variable_type::Scalar::EffectivePlasticStrain},
                               {"VonMisesStress", variable_type::Scalar::VonMisesStress},
                               {"Damage", variable_type::Scalar::Damage},
                               {"EnergyReleaseRate", variable_type::Scalar::EnergyReleaseRate}};

    TensorMap const tensor_map{{"CauchyStress", variable_type::Tensor::Cauchy},
                               {"LinearisedStrain", variable_type::Tensor::LinearisedStrain},
                               {"LinearisedPlasticStrain", variable_type::Tensor::LinearisedPlasticStrain},
                               {"DeformationGradient", variable_type::Tensor::DeformationGradient},
                               {"DisplacementGradient", variable_type::Tensor::DisplacementGradient},
                               {"KinematicHardening", variable_type::Tensor::KinematicHardening},
                               {"BackStress", variable_type::Tensor::BackStress}};
    // clang-format on
};

template <class femMeshType>
FileIO<femMeshType>::FileIO(std::string file_name,
                            Json::Value const& visualisation_data,
                            femMeshType const& fem_mesh)
    : neon::FileIO(file_name, visualisation_data), fem_mesh(fem_mesh)
{
    // Check the output set against the known values for this module
    for (auto const& output : output_set)
    {
        if (scalar_map.find(output) == scalar_map.end()
            && tensor_map.find(output) == tensor_map.end() && output != primary_field)
        {
            throw std::runtime_error("Output \"" + output
                                     + "\" is not valid for a solid mechanics simulation\n");
        }
    }
    add_mesh();
}

template <class femMeshType>
void FileIO<femMeshType>::write(int const time_step, double const total_time)
{
    if (time_step % write_every != 0) return;

    vector nodal_averaged_value, insertions;

    // Write out the required fields
    for (auto const& name : output_set)
    {
        if (auto found = tensor_map.find(name); found != tensor_map.end())
        {
            auto const tensor_size = femMeshType::internal_variable_type::tensor_size;

            nodal_averaged_value.resize(fem_mesh.coordinates().size() * tensor_size);

            insertions.resize(fem_mesh.coordinates().size() * tensor_size);

            nodal_averaged_value.setZero();
            insertions.setZero();

            // Add internal variables
            for (auto const& submesh : fem_mesh.meshes())
            {
                if (!submesh.internal_variables().has(found->second))
                {
                    throw std::runtime_error("Internal variable " + name + " does not exist in mesh");
                }

                auto const [value, count] = submesh.nodal_averaged_variable(found->second);

                nodal_averaged_value += value;

                insertions += count;
            }
            nodal_averaged_value = nodal_averaged_value.cwiseQuotient(insertions);

            add_field(found->first, nodal_averaged_value, tensor_size);
        }
        else if (auto found = scalar_map.find(name); found != scalar_map.end())
        {
            nodal_averaged_value.resize(fem_mesh.coordinates().size());

            insertions.resize(fem_mesh.coordinates().size());

            nodal_averaged_value.setZero();

            insertions.setZero();

            for (auto const& submesh : fem_mesh.meshes())
            {
                auto const [value, count] = submesh.nodal_averaged_variable(found->second);
                nodal_averaged_value += value;
                insertions += count;
            }

            nodal_averaged_value = nodal_averaged_value.cwiseQuotient(insertions);

            add_field(found->first, nodal_averaged_value, 1);
        }
        else if (name == primary_field)
        {
            unstructured_mesh->GetPointData()->AddArray(fem_mesh.coordinates().vtk_displacement());
        }
        else
        {
            throw std::runtime_error("Field \"" + name
                                     + "\" was not found in mesh internal variables\n");
        }
    }
    write_to_file(time_step, total_time);
}

template <class femMeshType>
void FileIO<femMeshType>::add_mesh()
{
    // Populate an unstructured grid object
    unstructured_mesh->SetPoints(fem_mesh.coordinates().vtk_coordinates());

    for (auto const& submesh : fem_mesh.meshes())
    {
        auto const vtk_ordered_connectivity = convert_to_vtk(submesh.connectivities(),
                                                             submesh.topology());
        for (auto const& node_list : vtk_ordered_connectivity)
        {
            auto vtk_node_list = vtkSmartPointer<vtkIdList>::New();

            for (auto const& node : node_list)
            {
                vtk_node_list->InsertNextId(static_cast<long>(node));
            }
            unstructured_mesh->InsertNextCell(to_vtk(submesh.topology()), vtk_node_list);
        }
    }
    unstructured_mesh->GetPointData()->AddArray(fem_mesh.coordinates().vtk_displacement());
}
}

namespace diffusion
{
class FileIO : public neon::FileIO
{
public:
    using VectorMap = std::map<std::string, InternalVariables::vector>;

    std::string const primary_field{"Temperature"};

public:
    explicit FileIO(std::string file_name,
                    Json::Value const& visualisation_data,
                    femMesh const& fem_mesh);

    FileIO(FileIO&&) = default;

    void write(int const time_step, double const total_time, vector const& temperature);

private:
    void add_mesh();

private:
    femMesh const& fem_mesh;

    VectorMap const vector_map{{"HeatFlux", InternalVariables::vector::HeatFlux}};
};
}
}
