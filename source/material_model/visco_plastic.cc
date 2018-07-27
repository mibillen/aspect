/*
  Copyright (C) 2011 - 2018 by the authors of the ASPECT code.

  This file is part of ASPECT.

  ASPECT is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2, or (at your option)
  any later version.

  ASPECT is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with ASPECT; see the file LICENSE.  If not see
  <http://www.gnu.org/licenses/>.
*/

#include <aspect/material_model/visco_plastic.h>
#include <aspect/utilities.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/base/signaling_nan.h>
#include <aspect/newton.h>
#include <aspect/adiabatic_conditions/interface.h>

namespace aspect
{
  namespace MaterialModel
  {

    namespace
    {
      std::vector<std::string> make_plastic_additional_outputs_names()
      {
        std::vector<std::string> names;
        names.emplace_back("current_cohesions");
        names.emplace_back("current_friction_angles");
        names.emplace_back("plastic_yielding");
        return names;
      }
    }

    template <int dim>
    PlasticAdditionalOutputs<dim>::PlasticAdditionalOutputs (const unsigned int n_points)
      :
      NamedAdditionalMaterialOutputs<dim>(make_plastic_additional_outputs_names()),
      cohesions(n_points, numbers::signaling_nan<double>()),
      friction_angles(n_points, numbers::signaling_nan<double>()),
      yielding(n_points, numbers::signaling_nan<double>())
    {}

    template <int dim>
    std::vector<double>
    PlasticAdditionalOutputs<dim>::get_nth_output(const unsigned int idx) const
    {
      AssertIndexRange (idx, 3);
      switch (idx)
        {
          case 0:
            return cohesions;

          case 1:
            return friction_angles;

          case 2:
            return yielding;

          default:
            AssertThrow(false, ExcInternalError());
        }
      // We will never get here, so just return something
      return cohesions;
    }


    template <int dim>
    double
    ViscoPlastic<dim>::
    average_value ( const std::vector<double> &volume_fractions,
                    const std::vector<double> &parameter_values,
                    const enum averaging_scheme &average_type) const
    {
      double averaged_parameter = 0.0;

      switch (average_type)
        {
          case arithmetic:
          {
            for (unsigned int i=0; i< volume_fractions.size(); ++i)
              averaged_parameter += volume_fractions[i]*parameter_values[i];
            break;
          }
          case harmonic:
          {
            for (unsigned int i=0; i< volume_fractions.size(); ++i)
              averaged_parameter += volume_fractions[i]/(parameter_values[i]);
            averaged_parameter = 1.0/averaged_parameter;
            break;
          }
          case geometric:
          {
            for (unsigned int i=0; i < volume_fractions.size(); ++i)
              averaged_parameter += volume_fractions[i]*std::log(parameter_values[i]);
            averaged_parameter = std::exp(averaged_parameter);
            break;
          }
          case maximum_composition:
          {
            const unsigned int i = (unsigned int)(std::max_element( volume_fractions.begin(),
                                                                    volume_fractions.end() )
                                                  - volume_fractions.begin());
            averaged_parameter = parameter_values[i];
            break;
          }
          default:
          {
            AssertThrow( false, ExcNotImplemented() );
            break;
          }
        }
      return averaged_parameter;
    }


    template <int dim>
    std::pair<std::vector<double>, std::vector<double> >
    ViscoPlastic<dim>::
    calculate_isostrain_viscosities ( const std::vector<double> &volume_fractions,
                                      const double &pressure,
                                      const double &temperature,
                                      const std::vector<double> &composition,
                                      const SymmetricTensor<2,dim> &strain_rate,
                                      const ViscosityScheme &viscous_type,
                                      const YieldScheme &yield_type) const
    {
      // This function calculates viscosities assuming that all the compositional fields
      // experience the same strain rate (isostrain).

      // Calculate the square root of the second moment invariant for the deviatoric strain rate tensor.
      // The first time this function is called (first iteration of first time step)
      // a specified "reference" strain rate is used as the returned value would
      // otherwise be zero.
      const double edot_ii = ( (this->get_timestep_number() == 0 && strain_rate.norm() <= std::numeric_limits<double>::min())
                               ?
                               ref_strain_rate
                               :
                               std::max(std::sqrt(std::fabs(second_invariant(deviator(strain_rate)))),
                                        min_strain_rate) );

      // Calculate viscosities for each of the individual compositional phases
      std::vector<double> composition_viscosities(volume_fractions.size());
      std::vector<double> composition_yielding(volume_fractions.size());
      for (unsigned int j=0; j < volume_fractions.size(); ++j)
        {
          // Power law creep equation
          //    viscosity = 0.5 * A^(-1/n) * edot_ii^((1-n)/n) * d^(m/n) * exp((E + P*V)/(nRT))
          // A: prefactor, edot_ii: square root of second invariant of deviatoric strain rate tensor,
          // d: grain size, m: grain size exponent, E: activation energy, P: pressure,
          // V; activation volume, n: stress exponent, R: gas constant, T: temperature.
          // Note: values of A, d, m, E, V and n are distinct for diffusion & dislocation creep

          // Diffusion creep: viscosity is grain size dependent (m!=0) and strain-rate independent (n=1)
          double viscosity_diffusion = 0.5 / prefactors_diffusion[j] *
                                       std::exp((activation_energies_diffusion[j] + pressure*activation_volumes_diffusion[j])/
                                                (constants::gas_constant*temperature)) *
                                       std::pow(grain_size, grain_size_exponents_diffusion[j]);

          // For dislocation creep, viscosity is grain size independent (m=0) and strain-rate dependent (n>1)
          double viscosity_dislocation = 0.5 * std::pow(prefactors_dislocation[j],-1/stress_exponents_dislocation[j]) *
                                         std::exp((activation_energies_dislocation[j] + pressure*activation_volumes_dislocation[j])/
                                                  (constants::gas_constant*temperature*stress_exponents_dislocation[j])) *
                                         std::pow(edot_ii,((1. - stress_exponents_dislocation[j])/stress_exponents_dislocation[j]));

          // Composite viscosity
          double viscosity_composite = (viscosity_diffusion * viscosity_dislocation)/(viscosity_diffusion + viscosity_dislocation);

          // Select what form of viscosity to use (diffusion, dislocation or composite)
          double viscosity_pre_yield = 0.0;
          switch (viscous_type)
            {
              case diffusion:
              {
                viscosity_pre_yield = viscosity_diffusion;
                break;
              }
              case dislocation:
              {
                viscosity_pre_yield = viscosity_dislocation;
                break;
              }
              case composite:
              {
                viscosity_pre_yield = viscosity_composite;
                break;
              }
              default:
              {
                AssertThrow( false, ExcNotImplemented() );
                break;
              }
            }

          double phi = angles_internal_friction[j];

          // Passing cohesions to a new variable
          double coh = cohesions[j];

          // Viscous weakening
          double viscous_weakening = 1.;

          // Strain weakening
          double strain_ii = 0.;
          if (use_strain_weakening == true)
            {
              // Calculate and/or constrain the strain invariant of the previous timestep
              if ( use_finite_strain_tensor == true )
                {
                  // Calculate second invariant of left stretching tensor "L"
                  Tensor<2,dim> strain;
                  for (unsigned int q = 0; q < Tensor<2,dim>::n_independent_components ; ++q)
                    strain[Tensor<2,dim>::unrolled_to_component_indices(q)] = composition[q];
                  const SymmetricTensor<2,dim> L = symmetrize( strain * transpose(strain) );
                  strain_ii = std::fabs(second_invariant(L));
                }
              // Use the plastic or total strain
              // Here the compositional field already contains the finite strain invariant magnitude
              else if (use_plastic_strain_weakening)
                strain_ii = composition[this->introspection().compositional_index_for_name("plastic_strain")];
              else if (use_viscous_strain_weakening == false)
                strain_ii = composition[this->introspection().compositional_index_for_name("total_strain")];

              // Compute the weakened cohesions and friction angles for the current compositional field
              std::pair<double, double> weakening = calculate_plastic_weakening(strain_ii, j);
              coh = weakening.first;
              phi = weakening.second;

              // Compute the weakening of the diffusion and dislocation prefactors
              // using the viscous strain or the already set total strain
              if (use_viscous_strain_weakening == true)
                strain_ii = composition[this->introspection().compositional_index_for_name("viscous_strain")];

              viscous_weakening = calculate_viscous_weakening(strain_ii, j);
            }


          // Apply strain weakening of the viscous viscosity
          viscosity_pre_yield *= viscous_weakening;

          // For composition spcrust, change from fixed *maximum* viscosity to flow-law viscosity
          // over specified pressure range (strain weakening could make this lower)
          if (use_fixed_spcrust_viscosity == true)
            {
              if (j == (this->introspection().compositional_index_for_name("spcrust") + 1))
                {
                  if (pressure <= spcrust_viscosity_minimum_pressure)
                    {
                      viscosity_pre_yield = std::min(maximum_spcrust_viscosity,viscosity_pre_yield);
                    }
                  else if ((pressure > spcrust_viscosity_minimum_pressure)
                           && (pressure < spcrust_viscosity_maximum_pressure))
                    {
                      viscosity_pre_yield = std::min(maximum_spcrust_viscosity*
                                                     std::pow(10,(pressure-spcrust_viscosity_minimum_pressure)*
                                                              (std::log10(max_visc)-std::log10(maximum_spcrust_viscosity))/
                                                              (spcrust_viscosity_maximum_pressure-spcrust_viscosity_minimum_pressure)),
                                                     viscosity_pre_yield);
                    }
                }
            }

          // Calculate viscous stress
          double viscous_stress = 2. * viscosity_pre_yield * edot_ii;

          // Calculate Drucker Prager yield strength (i.e. yield stress)
          double yield_strength = ( (dim==3)
                                    ?
                                    ( 6.0 * coh * std::cos(phi) + 6.0 * std::max(pressure,0.0) * std::sin(phi) )
                                    / ( std::sqrt(3.0) * (3.0 + std::sin(phi) ) )
                                    :
                                    coh * std::cos(phi) + std::max(pressure,0.0) * std::sin(phi) );

          // Use max_yield_strength to limit the yield strength for depths beneath the lithosphere
          yield_strength = std::min(yield_strength, max_yield_strength);

          // If the viscous stress is greater than the yield strength, rescale the viscosity back to yield surface
          // Also, we use a value of 1 to indicate we're in the yielding regime.
          double viscosity_drucker_prager;
          if ( viscous_stress >= yield_strength )
            {
              viscosity_drucker_prager = yield_strength / (2.0 * edot_ii);
              composition_yielding[j] = 1.0;
            }
          else
            {
              viscosity_drucker_prager = viscosity_pre_yield;
            }


          // Stress limiter rheology
          double viscosity_limiter;
          viscosity_limiter = yield_strength / (2.0 * ref_strain_rate) *
                              std::pow((edot_ii/ref_strain_rate), 1./exponents_stress_limiter[j] - 1.0);

          // Select if yield viscosity is based on Drucker Prager or stress limiter rheology
          double viscosity_yield;
          switch (yield_type)
            {
              case stress_limiter:
              {
                viscosity_yield = 1. / ( 1./viscosity_limiter + 1./viscosity_pre_yield);
                break;
              }
              case drucker_prager:
              {
                viscosity_yield = viscosity_drucker_prager;
                break;
              }
              default:
              {
                AssertThrow( false, ExcNotImplemented() );
                break;
              }
            }

          // Limit the viscosity with specified minimum and maximum bounds
          composition_viscosities[j] = std::min(std::max(viscosity_yield, min_visc), max_visc);

        }
      return std::make_pair (composition_viscosities, composition_yielding);
    }


    template <int dim>
    std::pair<double, double>
    ViscoPlastic<dim>::
    calculate_plastic_weakening(const double strain_ii,
                                const unsigned int j) const
    {
      // Constrain the second strain invariant of the previous timestep by the strain interval
      const double cut_off_strain_ii = std::max(std::min(strain_ii,end_plastic_strain_weakening_intervals[j]),start_plastic_strain_weakening_intervals[j]);

      // Linear strain weakening of cohesion and internal friction angle between specified strain values
      const double strain_fraction = ( cut_off_strain_ii - start_plastic_strain_weakening_intervals[j] ) /
                                     ( start_plastic_strain_weakening_intervals[j] - end_plastic_strain_weakening_intervals[j] );
      const double current_coh = cohesions[j] + ( cohesions[j] - cohesions[j] * cohesion_strain_weakening_factors[j] ) * strain_fraction;
      const double current_phi = angles_internal_friction[j] + ( angles_internal_friction[j] - angles_internal_friction[j] * friction_strain_weakening_factors[j] ) * strain_fraction;

      return std::make_pair (current_coh, current_phi);
    }

    template <int dim>
    double
    ViscoPlastic<dim>::
    calculate_viscous_weakening(const double strain_ii,
                                const unsigned int j) const
    {
      // Constrain the second strain invariant of the previous timestep by the strain interval
      const double cut_off_strain_ii = std::max(std::min(strain_ii,end_viscous_strain_weakening_intervals[j]),start_viscous_strain_weakening_intervals[j]);

      // Linear strain weakening of cohesion and internal friction angle between specified strain values
      const double strain_fraction = ( cut_off_strain_ii - start_viscous_strain_weakening_intervals[j] ) /
                                     ( start_viscous_strain_weakening_intervals[j] - end_viscous_strain_weakening_intervals[j] );
      const double weakening = 1. + ( 1. - viscous_strain_weakening_factors[j] ) * strain_fraction;

      return weakening;
    }

    template <int dim>
    void
    ViscoPlastic<dim>::
    evaluate(const MaterialModel::MaterialModelInputs<dim> &in,
             MaterialModel::MaterialModelOutputs<dim> &out) const
    {
      // set up additional output for the derivatives
      MaterialModel::MaterialModelDerivatives<dim> *derivatives;
      derivatives = out.template get_additional_output<MaterialModel::MaterialModelDerivatives<dim> >();

      // Store which components to exclude during volume fraction computation.
      ComponentMask composition_mask(this->n_compositional_fields(),true);
      if (use_strain_weakening == true)
        {
          if (use_plastic_strain_weakening)
            composition_mask.set(this->introspection().compositional_index_for_name("plastic_strain"),false);

          if (use_viscous_strain_weakening)
            composition_mask.set(this->introspection().compositional_index_for_name("viscous_strain"),false);

          if (!use_plastic_strain_weakening && !use_viscous_strain_weakening && !use_finite_strain_tensor)
            composition_mask.set(this->introspection().compositional_index_for_name("total_strain"),false);

          if (use_finite_strain_tensor)
            {
              const unsigned int n_start = this->introspection().compositional_index_for_name("s11");
              for (unsigned int i = n_start; i < n_start + Tensor<2,dim>::n_independent_components ; ++i)
                composition_mask.set(i,false);
            }
        }

      // Loop through points
      for (unsigned int i=0; i < in.temperature.size(); ++i)
        {
          const double temperature = in.temperature[i];
          const double pressure = in.pressure[i];
          const std::vector<double> &composition = in.composition[i];
          const std::vector<double> volume_fractions = compute_volume_fractions(composition, composition_mask);
          const SymmetricTensor<2,dim> strain_rate = in.strain_rate[i];

          // Averaging composition-field dependent properties

          // densities
          double density = 0.0;
          for (unsigned int j=0; j < volume_fractions.size(); ++j)
            {
              double delta_crust_density = 0.0;
              if (use_spcrust_density_change)
                {
                  if (j == (this->introspection().compositional_index_for_name("spcrust") + 1))
                    {
                      if ((pressure > spcrust_density_minimum_pressure) && (pressure < spcrust_density_maximum_pressure))
                        {
                          delta_crust_density = (pressure-spcrust_density_minimum_pressure)*(spcrust_density_change)/
                                                (spcrust_density_maximum_pressure-spcrust_density_minimum_pressure);
                        }
                      else if (pressure >= spcrust_density_maximum_pressure)
                        {
                          delta_crust_density = spcrust_density_change;
                        }
                    }
                }
              // not strictly correct if thermal expansivities are different, since we are interpreting
              // these compositions as volume fractions, but the error introduced should not be too bad.
              const double temperature_factor = (1.0 - thermal_expansivities[j] * (temperature - reference_T));
              density += volume_fractions[j] * (densities[j] + delta_crust_density) * temperature_factor;
            }

          // thermal expansivities
          double thermal_expansivity = 0.0;
          for (unsigned int j=0; j < volume_fractions.size(); ++j)
            thermal_expansivity += volume_fractions[j] * thermal_expansivities[j];

          // heat capacities
          double heat_capacity = 0.0;
          for (unsigned int j=0; j < volume_fractions.size(); ++j)
            heat_capacity += volume_fractions[j] * heat_capacities[j];

          // thermal diffusivities
          double thermal_diffusivity = 0.0;
          for (unsigned int j=0; j < volume_fractions.size(); ++j)
            thermal_diffusivity += volume_fractions[j] * thermal_diffusivities[j];

          // calculate effective viscosity
          // and retrieve whether the material is plastically yielding
          bool plastic_yielding = false;
          if (in.strain_rate.size())
            {
              // Currently, the viscosities for each of the compositional fields are calculated assuming
              // isostrain amongst all compositions, allowing calculation of the viscosity ratio.
              // TODO: This is only consistent with viscosity averaging if the arithmetic averaging
              // scheme is chosen. It would be useful to have a function to calculate isostress viscosities.
              const std::pair<std::vector<double>, std::vector<double> > calculate_viscosities =
                calculate_isostrain_viscosities(volume_fractions, pressure, temperature, composition, strain_rate,viscous_flow_law,yield_mechanism);
              const std::vector<double> composition_viscosities = calculate_viscosities.first;
              const std::vector<double> composition_yielding = calculate_viscosities.second;

              // The isostrain condition implies that the viscosity averaging should be arithmetic (see above).
              // We have given the user freedom to apply alternative bounds, because in diffusion-dominated
              // creep (where n_diff=1) viscosities are stress and strain-rate independent, so the calculation
              // of compositional field viscosities is consistent with any averaging scheme.
              out.viscosities[i] = average_value(volume_fractions, composition_viscosities, viscosity_averaging);

              // Take the infinity norm to compute plastic yielding in this point.
              // This avoids for example division by zero for harmonic averaging (as compositional_yielding
              // holds values that are either 0 or 1), but might not be consistent with the viscosity
              // averaging chosen.
              plastic_yielding   = average_value(volume_fractions, composition_yielding, maximum_composition);

              // compute derivatives if necessary
              std::vector<SymmetricTensor<2,dim> > composition_viscosities_derivatives(volume_fractions.size());
              std::vector<double> composition_dviscosities_dpressure(volume_fractions.size());
              if (derivatives != NULL)
                {
                  const double finite_difference_accuracy = 1e-7;

                  // For each independent component, compute the derivative.
                  for (unsigned int component = 0; component < SymmetricTensor<2,dim>::n_independent_components; ++component)
                    {
                      const TableIndices<2> strain_rate_indices = SymmetricTensor<2,dim>::unrolled_to_component_indices (component);

                      const SymmetricTensor<2,dim> strain_rate_difference = strain_rate
                                                                            + std::max(std::fabs(strain_rate[strain_rate_indices]), min_strain_rate)
                                                                            * finite_difference_accuracy
                                                                            * Utilities::nth_basis_for_symmetric_tensors<dim>(component);
                      std::vector<double> eta_component =
                        calculate_isostrain_viscosities(volume_fractions, pressure,
                                                        temperature, composition,
                                                        strain_rate_difference,
                                                        viscous_flow_law,yield_mechanism).first;

                      // For each composition of the independent component, compute the derivative.
                      for (unsigned int composition_index = 0; composition_index < eta_component.size(); ++composition_index)
                        {
                          // compute the difference between the viscosity with and without the strain-rate difference.
                          double viscosity_derivative = eta_component[composition_index] - composition_viscosities[composition_index];
                          if (viscosity_derivative != 0)
                            {
                              // when the difference is non-zero, divide by the difference.
                              viscosity_derivative /= std::max(std::fabs(strain_rate_difference[strain_rate_indices]), min_strain_rate)
                                                      * finite_difference_accuracy;
                            }
                          composition_viscosities_derivatives[composition_index][strain_rate_indices] = viscosity_derivative;
                        }
                    }

                  /**
                   * Now compute the derivative of the viscosity to the pressure
                   */
                  const double pressure_difference = in.pressure[i] + (std::fabs(in.pressure[i]) * finite_difference_accuracy);

                  const std::vector<double> viscosity_difference =
                    calculate_isostrain_viscosities(volume_fractions, pressure_difference,
                                                    temperature, composition, strain_rate,
                                                    viscous_flow_law, yield_mechanism).first;


                  for (unsigned int composition_index = 0; composition_index < viscosity_difference.size(); ++composition_index)
                    {
                      double viscosity_derivative = viscosity_difference[composition_index] - composition_viscosities[composition_index];
                      if (viscosity_difference[composition_index] != 0)
                        {
                          if (in.pressure[i] != 0)
                            {
                              viscosity_derivative /= std::fabs(in.pressure[i]) * finite_difference_accuracy;
                            }
                          else
                            {
                              viscosity_derivative = 0;
                            }
                        }
                      composition_dviscosities_dpressure[composition_index] = viscosity_derivative;
                    }

                  double viscosity_averaging_p = 0; // Geometric
                  if (viscosity_averaging == harmonic)
                    viscosity_averaging_p = -1;
                  if (viscosity_averaging == arithmetic)
                    viscosity_averaging_p = 1;
                  if (viscosity_averaging == maximum_composition)
                    viscosity_averaging_p = 1000;


                  derivatives->viscosity_derivative_wrt_strain_rate[i] =
                    Utilities::derivative_of_weighted_p_norm_average(out.viscosities[i],
                                                                     volume_fractions,
                                                                     composition_viscosities,
                                                                     composition_viscosities_derivatives,
                                                                     viscosity_averaging_p);
                  derivatives->viscosity_derivative_wrt_pressure[i] =
                    Utilities::derivative_of_weighted_p_norm_average(out.viscosities[i],
                                                                     volume_fractions,
                                                                     composition_viscosities,
                                                                     composition_dviscosities_dpressure,
                                                                     viscosity_averaging_p);

                }
            }

          out.densities[i] = density;
          out.thermal_expansion_coefficients[i] = thermal_expansivity;
          // Specific heat at the given positions.
          out.specific_heat[i] = heat_capacity;
          // Thermal conductivity at the given positions. If the temperature equation uses
          // the reference density profile formulation, use the reference density to
          // calculate thermal conductivity. Otherwise, use the real density. If the adiabatic
          // conditions are not yet initialized, the real density will still be used.
          if (this->get_parameters().formulation_temperature_equation ==
              Parameters<dim>::Formulation::TemperatureEquation::reference_density_profile &&
              this->get_adiabatic_conditions().is_initialized())
            out.thermal_conductivities[i] = thermal_diffusivity * heat_capacity *
                                            this->get_adiabatic_conditions().density(in.position[i]);
          else
            out.thermal_conductivities[i] = thermal_diffusivity * heat_capacity * density;
          // Compressibility at the given positions.
          // The compressibility is given as
          // $\frac 1\rho \frac{\partial\rho}{\partial p}$.
          out.compressibilities[i] = 0.0;
          // Pressure derivative of entropy at the given positions.
          out.entropy_derivative_pressure[i] = 0.0;
          // Temperature derivative of entropy at the given positions.
          out.entropy_derivative_temperature[i] = 0.0;
          // Change in composition due to chemical reactions at the
          // given positions. The term reaction_terms[i][c] is the
          // change in compositional field c at point i.
          for (unsigned int c=0; c<in.composition[i].size(); ++c)
            out.reaction_terms[i][c] = 0.0;
          // If strain weakening is used, overwrite the first reaction term,
          // which represents the second invariant of the (plastic) strain tensor.
          // If plastic strain is tracked (so not the total strain), only overwrite
          // when plastically yielding.
          // If viscous strain is also tracked, overwrite the second rection term as well.
          double edot_ii = 0.;
          double e_ii = 0.;
          if  (use_strain_weakening == true && use_finite_strain_tensor == false && this->get_timestep_number() > 0 && in.strain_rate.size())
            {
              edot_ii = std::max(sqrt(std::fabs(second_invariant(deviator(strain_rate)))),min_strain_rate);
              e_ii = edot_ii*this->get_timestep();
              if (use_plastic_strain_weakening == true && plastic_yielding == true)
                out.reaction_terms[i][this->introspection().compositional_index_for_name("plastic_strain")] = e_ii;
              if (use_viscous_strain_weakening == true && plastic_yielding == false)
                out.reaction_terms[i][this->introspection().compositional_index_for_name("viscous_strain")] = e_ii;
              if (use_plastic_strain_weakening == false && use_viscous_strain_weakening == false)
                out.reaction_terms[i][this->introspection().compositional_index_for_name("total_strain")] = e_ii;
            }

          // fill plastic outputs if they exist
          if (PlasticAdditionalOutputs<dim> *plastic_out = out.template get_additional_output<PlasticAdditionalOutputs<dim> >())
            {
              double C = 0.;
              double phi = 0.;
              // set to weakened values, or unweakened values when strain weakening is not used
              for (unsigned int j=0; j < volume_fractions.size(); ++j)
                {
                  // the first compositional field contains the total strain or the plastic strain or, in case only viscous strain
                  // weakening is applied, the viscous strain.
                  if (use_strain_weakening == true )
                    {
                      double strain_invariant = 0.;
                      if (use_plastic_strain_weakening)
                        strain_invariant = composition[this->introspection().compositional_index_for_name("plastic_strain")];
                      else if (!use_viscous_strain_weakening && !use_finite_strain_tensor)
                        strain_invariant = composition[this->introspection().compositional_index_for_name("total_strain")];
                      else if (use_finite_strain_tensor)
                        {
                          // Calculate second invariant of left stretching tensor "L"
                          Tensor<2,dim> strain;
                          const unsigned int n_first = this->introspection().compositional_index_for_name("s11");
                          for (unsigned int q = n_first; q < n_first + Tensor<2,dim>::n_independent_components ; ++q)
                            strain[Tensor<2,dim>::unrolled_to_component_indices(q)] = composition[q];
                          const SymmetricTensor<2,dim> L = symmetrize( strain * transpose(strain) );
                          strain_invariant = std::fabs(second_invariant(L));
                        }

                      std::pair<double, double> weakening = calculate_plastic_weakening(strain_invariant, j);
                      C   += volume_fractions[j] * weakening.first;
                      phi += volume_fractions[j] * weakening.second;
                    }
                  else
                    {
                      C   += volume_fractions[j] * cohesions[j];
                      phi += volume_fractions[j] * angles_internal_friction[j];
                    }
                }
              plastic_out->cohesions[i] = C;
              // convert radians to degrees
              plastic_out->friction_angles[i] = phi * 180. / numbers::PI;
              plastic_out->yielding[i] = plastic_yielding ? 1 : 0;
            }
        }

      // We need the velocity gradient for the finite strain (they are not included in material model inputs),
      // so we get them from the finite element.
      if (in.current_cell.state() == IteratorState::valid && use_strain_weakening == true
          && use_finite_strain_tensor == true && this->get_timestep_number() > 0 && in.strain_rate.size())
        {

          std::vector<Point<dim> > quadrature_positions(in.position.size());
          for (unsigned int i=0; i < in.position.size(); ++i)
            quadrature_positions[i] = this->get_mapping().transform_real_to_unit_cell(in.current_cell, in.position[i]);

          FEValues<dim> fe_values (this->get_mapping(),
                                   this->get_fe(),
                                   Quadrature<dim>(quadrature_positions),
                                   update_gradients);

          std::vector<Tensor<2,dim> > velocity_gradients (quadrature_positions.size(), Tensor<2,dim>());

          fe_values.reinit (in.current_cell);
          fe_values[this->introspection().extractors.velocities].get_function_gradients (this->get_solution(),
                                                                                         velocity_gradients);

          // Assign the strain components to the compositional fields reaction terms.
          // If there are too many fields, we simply fill only the first fields with the
          // existing strain tensor components.
          for (unsigned int q=0; q < in.position.size(); ++q)
            {
              // Convert the compositional fields into the tensor quantity they represent.
              Tensor<2,dim> strain;
              const unsigned int n_first = this->introspection().compositional_index_for_name("s11");
              for (unsigned int i = n_first; i < n_first + Tensor<2,dim>::n_independent_components ; ++i)
                {
                  strain[Tensor<2,dim>::unrolled_to_component_indices(i)] = in.composition[q][i];
                }

              // Compute the strain accumulated in this timestep.
              const Tensor<2,dim> strain_increment = this->get_timestep() * (velocity_gradients[q] * strain);

              // Output the strain increment component-wise to its respective compositional field's reaction terms.
              for (unsigned int i = n_first; i < n_first + Tensor<2,dim>::n_independent_components ; ++i)
                {
                  out.reaction_terms[q][i] = strain_increment[Tensor<2,dim>::unrolled_to_component_indices(i)];
                }
            }

        }
    }

    template <int dim>
    double
    ViscoPlastic<dim>::
    reference_viscosity () const
    {
      return ref_visc;
    }

    template <int dim>
    bool
    ViscoPlastic<dim>::
    is_compressible () const
    {
      return false;
    }

    template <int dim>
    double ViscoPlastic<dim>::
    get_min_strain_rate () const
    {
      return min_strain_rate;
    }

    template <int dim>
    void
    ViscoPlastic<dim>::declare_parameters (ParameterHandler &prm)
    {
      prm.enter_subsection("Material model");
      {
        prm.enter_subsection ("Visco Plastic");
        {
          // Reference and minimum/maximum values
          prm.declare_entry ("Reference temperature", "293", Patterns::Double(0),
                             "For calculating density by thermal expansivity. Units: $K$");
          prm.declare_entry ("Minimum strain rate", "1.0e-20", Patterns::Double(0),
                             "Stabilizes strain dependent viscosity. Units: $1 / s$");
          prm.declare_entry ("Reference strain rate","1.0e-15",Patterns::Double(0),
                             "Reference strain rate for first time step. Units: $1 / s$");
          prm.declare_entry ("Minimum viscosity", "1e17", Patterns::Double(0),
                             "Lower cutoff for effective viscosity. Units: $Pa \\, s$");
          prm.declare_entry ("Maximum viscosity", "1e28", Patterns::Double(0),
                             "Upper cutoff for effective viscosity. Units: $Pa \\, s$");
          prm.declare_entry ("Reference viscosity", "1e22", Patterns::Double(0),
                             "Reference viscosity for nondimensionalization. "
                             "To understand how pressure scaling works, take a look at "
                             "\\cite{KHB12}. In particular, the value of this parameter "
                             "would not affect the solution computed by \\aspect{} if "
                             "we could do arithmetic exactly; however, computers do "
                             "arithmetic in finite precision, and consequently we need to "
                             "scale quantities in ways so that their magnitudes are "
                             "roughly the same. As explained in \\cite{KHB12}, we scale "
                             "the pressure during some computations (never visible by "
                             "users) by a factor that involves a reference viscosity. This "
                             "parameter describes this reference viscosity."
                             "\n\n"
                             "For problems with a constant viscosity, you will generally want "
                             "to choose the reference viscosity equal to the actual viscosity. "
                             "For problems with a variable viscosity, the reference viscosity "
                             "should be a value that adequately represents the order of "
                             "magnitude of the viscosities that appear, such as an average "
                             "value or the value one would use to compute a Rayleigh number."
                             "\n\n"
                             "Units: $Pa \\, s$");

          // Equation of state parameters
          prm.declare_entry ("Thermal diffusivities", "0.8e-6",
                             Patterns::List(Patterns::Double(0)),
                             "List of thermal diffusivities, for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value.  Units: $m^2/s$");
          prm.declare_entry ("Heat capacities", "1.25e3",
                             Patterns::List(Patterns::Double(0)),
                             "List of heat capacities $C_p$, for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value.  Units: $J/kg/K$");
          prm.declare_entry ("Densities", "3300.",
                             Patterns::List(Patterns::Double(0)),
                             "List of densities, $\\rho$, for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value.  Units: $kg / m^3$");
          prm.declare_entry ("Thermal expansivities", "3.5e-5",
                             Patterns::List(Patterns::Double(0)),
                             "List of thermal expansivities for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value.  Units: $1 / K$");

          // Strain weakening parameters
          prm.declare_entry ("Use strain weakening", "false",
                             Patterns::Bool (),
                             "Apply strain weakening to viscosity, cohesion and internal angle "
                             "of friction based on accumulated finite strain.  Units: None");
          prm.declare_entry ("Use plastic strain weakening", "false",
                             Patterns::Bool (),
                             "Apply strain weakening to cohesion and internal angle "
                             "of friction based on accumulated finite plastic strain only.  Units: None");
          prm.declare_entry ("Use viscous strain weakening", "false",
                             Patterns::Bool (),
                             "Apply strain weakening to diffusion and dislocation viscosity prefactors "
                             "based on accumulated finite viscous strain only.  Units: None");
          prm.declare_entry ("Use finite strain tensor", "false",
                             Patterns::Bool (),
                             "Track and use the full finite strain tensor for strain weakening. "
                             "Units: None");
          prm.declare_entry ("Start plasticity strain weakening intervals", "0.",
                             Patterns::List(Patterns::Double(0)),
                             "List of strain weakening interval initial strains "
                             "for the cohesion and friction angle parameters of the "
                             "background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value.  Units: None");
          prm.declare_entry ("End plasticity strain weakening intervals", "1.",
                             Patterns::List(Patterns::Double(0)),
                             "List of strain weakening interval final strains "
                             "for the cohesion and friction angle parameters of the "
                             "background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value.  Units: None");
          prm.declare_entry ("Start prefactor strain weakening intervals", "0.",
                             Patterns::List(Patterns::Double(0)),
                             "List of strain weakening interval initial strains "
                             "for the diffusion and dislocation prefactor parameters of the "
                             "background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value.  Units: None");
          prm.declare_entry ("End prefactor strain weakening intervals", "1.",
                             Patterns::List(Patterns::Double(0)),
                             "List of strain weakening interval final strains "
                             "for the diffusion and dislocation prefactor parameters of the "
                             "background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value.  Units: None");
          prm.declare_entry ("Prefactor strain weakening factors", "1.",
                             Patterns::List(Patterns::Double(0,1)),
                             "List of viscous strain weakening factors "
                             "for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value.  Units: None");
          prm.declare_entry ("Cohesion strain weakening factors", "1.",
                             Patterns::List(Patterns::Double(0)),
                             "List of cohesion strain weakening factors "
                             "for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value.  Units: None");
          prm.declare_entry ("Friction strain weakening factors", "1.",
                             Patterns::List(Patterns::Double(0)),
                             "List of friction strain weakening factors "
                             "for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value.  Units: None");

          // Rheological parameters
          prm.declare_entry ("Grain size", "1e-3", Patterns::Double(0), "Units: $m$");
          prm.declare_entry ("Viscosity averaging scheme", "harmonic",
                             Patterns::Selection("arithmetic|harmonic|geometric|maximum composition"),
                             "When more than one compositional field is present at a point "
                             "with different viscosities, we need to come up with an average "
                             "viscosity at that point.  Select a weighted harmonic, arithmetic, "
                             "geometric, or maximum composition.");
          prm.declare_entry ("Viscous flow law", "composite",
                             Patterns::Selection("diffusion|dislocation|composite"),
                             "Select what type of viscosity law to use between diffusion, "
                             "dislocation and composite options. Soon there will be an option "
                             "to select a specific flow law for each assigned composition ");
          prm.declare_entry ("Yield mechanism", "drucker",
                             Patterns::Selection("drucker|limiter"),
                             "Select what type of yield mechanism to use between Drucker Prager "
                             "and stress limiter options.");

          // Diffusion creep parameters
          prm.declare_entry ("Prefactors for diffusion creep", "1.5e-15",
                             Patterns::List(Patterns::Double(0)),
                             "List of viscosity prefactors, $A$, for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value. "
                             "Units: $Pa^{-1} m^{m_{\\text{diffusion}}} s^{-1}$");
          prm.declare_entry ("Stress exponents for diffusion creep", "1",
                             Patterns::List(Patterns::Double(0)),
                             "List of stress exponents, $n_{\\text{diffusion}}$, for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value.  Units: None");
          prm.declare_entry ("Grain size exponents for diffusion creep", "3",
                             Patterns::List(Patterns::Double(0)),
                             "List of grain size exponents, $m_{\\text{diffusion}}$, for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value. Units: None");
          prm.declare_entry ("Activation energies for diffusion creep", "375e3",
                             Patterns::List(Patterns::Double(0)),
                             "List of activation energies, $E_a$, for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value.  Units: $J / mol$");
          prm.declare_entry ("Activation volumes for diffusion creep", "6e-6",
                             Patterns::List(Patterns::Double(0)),
                             "List of activation volumes, $V_a$, for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value.  Units: $m^3 / mol$");

          // Dislocation creep parameters
          prm.declare_entry ("Prefactors for dislocation creep", "1.1e-16",
                             Patterns::List(Patterns::Double(0)),
                             "List of viscosity prefactors, $A$, for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value. "
                             "Units: $Pa^{-n_{\\text{dislocation}}} s^{-1}$");
          prm.declare_entry ("Stress exponents for dislocation creep", "3.5",
                             Patterns::List(Patterns::Double(0)),
                             "List of stress exponents, $n_{\\text{dislocation}}$, for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value.  Units: None");
          prm.declare_entry ("Activation energies for dislocation creep", "530e3",
                             Patterns::List(Patterns::Double(0)),
                             "List of activation energies, $E_a$, for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value.  Units: $J / mol$");
          prm.declare_entry ("Activation volumes for dislocation creep", "1.4e-5",
                             Patterns::List(Patterns::Double(0)),
                             "List of activation volumes, $V_a$, for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "If only one value is given, then all use the same value.  Units: $m^3 / mol$");


          // Plasticity parameters
          prm.declare_entry ("Angles of internal friction", "0",
                             Patterns::List(Patterns::Double(0)),
                             "List of angles of internal friction, $\\phi$, for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "For a value of zero, in 2D the von Mises criterion is retrieved. "
                             "Angles higher than 30 degrees are harder to solve numerically. Units: degrees.");
          prm.declare_entry ("Cohesions", "1e20",
                             Patterns::List(Patterns::Double(0)),
                             "List of cohesions, $C$, for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "The extremely large default cohesion value (1e20 Pa) prevents the viscous stress from "
                             "exceeding the yield stress. Units: $Pa$.");

          // Stress limiter parameters
          prm.declare_entry ("Stress limiter exponents", "1.0",
                             Patterns::List(Patterns::Double(0)),
                             "List of stress limiter exponents, $n_{\\text{lim}}$, "
                             "for background material and compositional fields, "
                             "for a total of N+1 values, where N is the number of compositional fields. "
                             "Units: none.");

          // Limit maximum value of the drucker-prager yield stress
          prm.declare_entry ("Maximum yield stress", "1e12", Patterns::Double(0),
                             "Limits the maximum value of the yield stress determined by the "
                             "drucker-prager plasticity parameters. Default value is chosen so this "
                             "is not automatically used. Values of 100e6--1000e6 $Pa$ have been used "
                             "in previous models. Units: $Pa$");

          // Transition from maximum spcrust viscosity to flow-law defined value
          // over specified pressure range
          prm.declare_entry ("Use fixed spcrust viscosity", "false", Patterns::Bool (),
                             "Transition the viscosity of a compositional field called spcrust "
                             "from a constant value to the value determined by the flow law parameters. "
                             "Units: None");
          prm.declare_entry ("Maximum spcrust viscosity", "1e28", Patterns::Double(0),
                             "Maximum viscosity for the composition called spcrust. Using a value of 1e20 $Pa \\, s$"
                             "would create a weak layer that smoothly increases to the viscosity "
                             "determined by the flow law parameters (if these predict a higher "
                             "value). Units: $Pa \\, s$");
          prm.declare_entry ("Minimum transition pressure spcrust viscosity", "0.0", Patterns::Double(0),
                             "Pressure at which to start the smooth transition from "
                             "the maximum spcrust viscosity to the viscosity determined by"
                             "the flow law. A value of 2.0e9 $Pa$ would correspond to a "
                             "depth of about 60 km. Units: $Pa$");
          prm.declare_entry ("Maximum transition pressure spcrust viscosity", "0.0", Patterns::Double(0),
                             "Pressure at which to end smooth transition from "
                             "the maximum spcrust viscosity to the viscosity determined by"
                             "the flow law. A value of 3.9e9 $Pa$ would correspond to a "
                             "depth of about 120 km. Units: $Pa$");

          // Transition the spcrust density from defined value by a delta-rho
          // given by spcrust_density_change over specified pressure range
          prm.declare_entry ("Use spcrust density change", "false", Patterns::Bool (),
                             "Change the density of a compositional field called spcrust"
                             "over a specified pressure range.  Units: None");
          prm.declare_entry ("Density change from spcrust", "0.0", Patterns::Double(0),
                             "Density change for spcrust composition density to new value. "
                             "Density changes smoothly from minimum to maximum transition pressure for spcrust."
                             "For basalt density of 3000 and eclogite density of 3540, use a value of 540 $kg/m^3"
                             "Units: $kg/m^3$");
          prm.declare_entry ("Minimum transition pressure spcrust density", "0.0", Patterns::Double(0),
                             "Pressure at which to start the smooth transition from  in density."
                             "A value of 2.0e9 $Pa$ would correspond to a depth of about 60 km. Units: $Pa$");
          prm.declare_entry ("Maximum transition pressure spcrust density", "0.0", Patterns::Double(0),
                             "Pressure at which to end smooth transition in density. "
                             "A value of 3.9e9 $Pa$ would correspond to a depth of about 120 km. Units: $Pa$");

        }
        prm.leave_subsection();
      }
      prm.leave_subsection();
    }



    template <int dim>
    void
    ViscoPlastic<dim>::parse_parameters (ParameterHandler &prm)
    {
      // increment by one for background:
      const unsigned int n_fields = this->n_compositional_fields() + 1;

      // number of required compositional fields for full finite strain tensor
      const unsigned int s = Tensor<2,dim>::n_independent_components;

      prm.enter_subsection("Material model");
      {
        prm.enter_subsection ("Visco Plastic");
        {
          // Reference and minimum/maximum values
          reference_T = prm.get_double("Reference temperature");
          min_strain_rate = prm.get_double("Minimum strain rate");
          ref_strain_rate = prm.get_double("Reference strain rate");
          min_visc = prm.get_double ("Minimum viscosity");
          max_visc = prm.get_double ("Maximum viscosity");
          ref_visc = prm.get_double ("Reference viscosity");

          // Equation of state parameters
          thermal_diffusivities = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Thermal diffusivities"))),
                                                                          n_fields,
                                                                          "Thermal diffusivities");
          heat_capacities = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Heat capacities"))),
                                                                    n_fields,
                                                                    "Heat capacities");

          // ---- Compositional parameters
          grain_size = prm.get_double("Grain size");
          densities = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Densities"))),
                                                              n_fields,
                                                              "Densities");
          thermal_expansivities = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Thermal expansivities"))),
                                                                          n_fields,
                                                                          "Thermal expansivities");

          // Strain weakening parameters
          use_strain_weakening             = prm.get_bool ("Use strain weakening");

          use_plastic_strain_weakening     = prm.get_bool ("Use plastic strain weakening");
          if (use_plastic_strain_weakening)
            {
              AssertThrow(use_strain_weakening,
                          ExcMessage("If plastic strain weakening is to be used, strain weakening should also be set to true. "));
              AssertThrow(this->introspection().compositional_name_exists("plastic_strain"),
                          ExcMessage("Material model visco_plastic with plastic strain weakening only works if there is a "
                                     "compositional field called plastic_strain."));
            }

          use_viscous_strain_weakening     = prm.get_bool ("Use viscous strain weakening");
          if (use_viscous_strain_weakening)
            {
              AssertThrow(use_strain_weakening,
                          ExcMessage("If viscous strain weakening is to be used, strain weakening should also be set to true. "));
              AssertThrow(this->introspection().compositional_name_exists("viscous_strain"),
                          ExcMessage("Material model visco_plastic with viscous strain weakening only works if there is a "
                                     "compositional field called viscous_strain."));
            }


          use_finite_strain_tensor  = prm.get_bool ("Use finite strain tensor");
          if (use_finite_strain_tensor)
            {
              AssertThrow(this->n_compositional_fields() >= s,
                          ExcMessage("There must be enough compositional fields to track all components of the finite strain tensor (4 in 2D, 9 in 3D). "));
              AssertThrow(use_strain_weakening,
                          ExcMessage("If strain weakening using the full tensor is to be used, strain weakening should also be set to true. "));
              AssertThrow(use_plastic_strain_weakening == false && use_viscous_strain_weakening == false,
                          ExcMessage("If strain weakening using the full tensor is to be used, the total strain will be used for weakening. "));
              // Assert that fields exist and that they are in the right order
              const unsigned int n_s11 = this->introspection().compositional_index_for_name("s11");
              const unsigned int n_s12 = this->introspection().compositional_index_for_name("s12");
              const unsigned int n_s21 = this->introspection().compositional_index_for_name("s21");
              const unsigned int n_s22 = this->introspection().compositional_index_for_name("s22");
              AssertThrow(n_s12>n_s11 && n_s21>n_s12 && n_s22 > n_s21,
                          ExcMessage("Material model visco_plastic with strain weakening using the full strain tensor only works if there "
                                     "are compositional fields called sij, with i=1,..,dim and j=1,...,dim in the order s11,s12,s21 etc."));
              if (dim==3)
                {
                  const unsigned int n_s13 = this->introspection().compositional_index_for_name("s13");
                  const unsigned int n_s23 = this->introspection().compositional_index_for_name("s23");
                  const unsigned int n_s31 = this->introspection().compositional_index_for_name("s31");
                  const unsigned int n_s32 = this->introspection().compositional_index_for_name("s32");
                  const unsigned int n_s33 = this->introspection().compositional_index_for_name("s33");
                  AssertThrow(n_s23 > n_s13 && n_s31 > n_s23 && n_s32 > n_s31 && n_s33 > n_s32,
                              ExcMessage("Material model visco_plastic with strain weakening using the full strain tensor only works if there "
                                         "are compositional fields called sij, with i=1,..,dim and j=1,...,dim in the order s11,s12,s21 etc."));
                  AssertThrow(n_s33 == n_s13+s-1, ExcMessage("The strain tensor components should be represented by consecutive fields."));
                }
            }

          if (use_strain_weakening)
            {
              if (!use_plastic_strain_weakening && !use_viscous_strain_weakening && !use_finite_strain_tensor)
                {
                  AssertThrow(this->introspection().compositional_name_exists("total_strain"),
                              ExcMessage("Material model visco_plastic with total strain weakening only works if there is a "
                                         "compositional field called total_strain."));
                }
            }

          start_plastic_strain_weakening_intervals = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Start plasticity strain weakening intervals"))),
                                                     n_fields,
                                                     "Start plasticity strain weakening intervals");
          end_plastic_strain_weakening_intervals = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("End plasticity strain weakening intervals"))),
                                                   n_fields,
                                                   "End plasticity strain weakening intervals");
          start_viscous_strain_weakening_intervals = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Start prefactor strain weakening intervals"))),
                                                     n_fields,
                                                     "Start prefactor strain weakening intervals");
          end_viscous_strain_weakening_intervals = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("End prefactor strain weakening intervals"))),
                                                   n_fields,
                                                   "End prefactor strain weakening intervals");
          viscous_strain_weakening_factors = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Prefactor strain weakening factors"))),
                                                                                     n_fields,
                                                                                     "Prefactor strain weakening factors");
          cohesion_strain_weakening_factors = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Cohesion strain weakening factors"))),
                                                                                      n_fields,
                                                                                      "Cohesion strain weakening factors");
          friction_strain_weakening_factors = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Friction strain weakening factors"))),
                                                                                      n_fields,
                                                                                      "Friction strain weakening factors");

          // Rheological parameters
          if (prm.get ("Viscosity averaging scheme") == "harmonic")
            viscosity_averaging = harmonic;
          else if (prm.get ("Viscosity averaging scheme") == "arithmetic")
            viscosity_averaging = arithmetic;
          else if (prm.get ("Viscosity averaging scheme") == "geometric")
            viscosity_averaging = geometric;
          else if (prm.get ("Viscosity averaging scheme") == "maximum composition")
            viscosity_averaging = maximum_composition;
          else
            AssertThrow(false, ExcMessage("Not a valid viscosity averaging scheme"));

          // Rheological parameters
          if (prm.get ("Viscous flow law") == "composite")
            viscous_flow_law = composite;
          else if (prm.get ("Viscous flow law") == "diffusion")
            viscous_flow_law = diffusion;
          else if (prm.get ("Viscous flow law") == "dislocation")
            viscous_flow_law = dislocation;
          else
            AssertThrow(false, ExcMessage("Not a valid viscous flow law"));

          // Rheological parameters
          if (prm.get ("Yield mechanism") == "drucker")
            yield_mechanism = drucker_prager;
          else if (prm.get ("Yield mechanism") == "limiter")
            yield_mechanism = stress_limiter;
          else
            AssertThrow(false, ExcMessage("Not a valid yield mechanism."));

          // Rheological parameters
          // Diffusion creep parameters (Stress exponents often but not always 1)
          prefactors_diffusion = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Prefactors for diffusion creep"))),
                                                                         n_fields,
                                                                         "Prefactors for diffusion creep");
          grain_size_exponents_diffusion = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Grain size exponents for diffusion creep"))),
                                                                                   n_fields,
                                                                                   "Grain size exponents for diffusion creep");
          activation_energies_diffusion = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Activation energies for diffusion creep"))),
                                                                                  n_fields,
                                                                                  "Activation energies for diffusion creep");
          activation_volumes_diffusion = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Activation volumes for diffusion creep"))),
                                                                                 n_fields,
                                                                                 "Activation volumes for diffusion creep");
          // Dislocation creep parameters (Note the lack of grain size exponents)
          prefactors_dislocation = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Prefactors for dislocation creep"))),
                                                                           n_fields,
                                                                           "Prefactors for dislocation creep");
          stress_exponents_dislocation = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Stress exponents for dislocation creep"))),
                                                                                 n_fields,
                                                                                 "Stress exponents for dislocation creep");
          activation_energies_dislocation = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Activation energies for dislocation creep"))),
                                                                                    n_fields,
                                                                                    "Activation energies for dislocation creep");
          activation_volumes_dislocation = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Activation volumes for dislocation creep"))),
                                                                                   n_fields,
                                                                                   "Activation volumes for dislocation creep");
          // Plasticity parameters
          angles_internal_friction = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Angles of internal friction"))),
                                                                             n_fields,
                                                                             "Angles of internal friction");
          // Convert angles from degrees to radians
          for (unsigned int i = 0; i<n_fields; ++i)
            angles_internal_friction[i] *= numbers::PI/180.0;
          cohesions = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Cohesions"))),
                                                              n_fields,
                                                              "Cohesions");
          // Stress limiter parameter
          exponents_stress_limiter  = Utilities::possibly_extend_from_1_to_N (Utilities::string_to_double(Utilities::split_string_list(prm.get("Stress limiter exponents"))),
                                                                              n_fields,
                                                                              "Stress limiter exponents");

          // Limit maximum value of the drucker-prager yield stress
          max_yield_strength = prm.get_double("Maximum yield stress");

          // spcrust composition with fixed viscosity that transitions over pressure range to flow-law rheology
          use_fixed_spcrust_viscosity = prm.get_bool ("Use fixed spcrust viscosity");
          if (use_fixed_spcrust_viscosity)
            AssertThrow(this->introspection().compositional_name_exists("spcrust"),
                        ExcMessage("There must be a compositional field called spcrust."));
          maximum_spcrust_viscosity = prm.get_double("Maximum spcrust viscosity");
          spcrust_viscosity_minimum_pressure = prm.get_double("Minimum transition pressure spcrust viscosity");
          spcrust_viscosity_maximum_pressure = prm.get_double("Maximum transition pressure spcrust viscosity");

          // spcrust composition with density change over pressure range
          use_spcrust_density_change = prm.get_bool ("Use spcrust density change");
          if (use_spcrust_density_change)
            AssertThrow(this->introspection().compositional_name_exists("spcrust"),
                        ExcMessage("There must be a compositional field called spcrust."));
          spcrust_density_change = prm.get_double("Density change from spcrust");
          spcrust_density_minimum_pressure = prm.get_double("Minimum transition pressure spcrust density");
          spcrust_density_maximum_pressure = prm.get_double("Maximum transition pressure spcrust density");

        }
        prm.leave_subsection();
      }
      prm.leave_subsection();

      // Declare dependencies on solution variables
      this->model_dependence.viscosity = NonlinearDependence::temperature | NonlinearDependence::pressure | NonlinearDependence::strain_rate | NonlinearDependence::compositional_fields;
      this->model_dependence.density = NonlinearDependence::temperature | NonlinearDependence::pressure | NonlinearDependence::compositional_fields;
      this->model_dependence.compressibility = NonlinearDependence::none;
      this->model_dependence.specific_heat = NonlinearDependence::none;
      this->model_dependence.thermal_conductivity = NonlinearDependence::temperature | NonlinearDependence::pressure | NonlinearDependence::compositional_fields;
    }

    template <int dim>
    void
    ViscoPlastic<dim>::create_additional_named_outputs (MaterialModel::MaterialModelOutputs<dim> &out) const
    {
      if (out.template get_additional_output<PlasticAdditionalOutputs<dim> >() == NULL)
        {
          const unsigned int n_points = out.viscosities.size();
          out.additional_outputs.push_back(
            std::shared_ptr<MaterialModel::AdditionalMaterialOutputs<dim> >
            (new MaterialModel::PlasticAdditionalOutputs<dim> (n_points)));
        }
    }

  }
}

// explicit instantiations
namespace aspect
{
  namespace MaterialModel
  {
    ASPECT_REGISTER_MATERIAL_MODEL(ViscoPlastic,
                                   "visco plastic",
                                   "An implementation of a visco-plastic rheology with options for "
                                   "selecting dislocation creep, diffusion creep or composite "
                                   "viscous flow laws.  Plasticity limits viscous stresses through "
                                   "a Drucker Prager yield criterion. The model is incompressible. "
                                   "Note that this material model is based heavily on the "
                                   "DiffusionDislocation (Bob Myhill) and DruckerPrager "
                                   "(Anne Glerum) material models. "
                                   "\n\n "
                                   "The viscosity for dislocation or diffusion creep is defined as "
                                   "\\[v = \\frac 12 A^{-\\frac{1}{n}} d^{\\frac{m}{n}} "
                                   "\\dot{\\varepsilon}_{ii}^{\\frac{1-n}{n}} "
                                   "\\exp\\left(\\frac{E + PV}{nRT}\\right)\\] "
                                   "where $A$ is the prefactor, $n$ is the stress exponent, "
                                   "$\\dot{\\varepsilon}_{ii}$ is the square root of the deviatoric "
                                   "strain rate tensor second invariant, $d$ is grain size, "
                                   "$m$ is the grain size exponent, $E$ is activation energy, "
                                   "$V$ is activation volume, $P$ is pressure, $R$ is the gas "
                                   "exponent and $T$ is temperature. "
                                   "This form of the viscosity equation is commonly used in "
                                   "geodynamic simulations. See, for example, Billen and Hirth "
                                   "(2007), G3, 8, Q08012. Significantly, other studies may use "
                                   "slightly different forms of the viscosity equation leading to "
                                   "variations in how specific terms are defined or combined. For "
                                   "example, the grain size exponent should always be positive in "
                                   "the diffusion viscosity equation used here, while other studies "
                                   "place the grain size term in the denominator and invert the sign "
                                   "of the grain size exponent. When examining previous work, one "
                                   "should carefully check how the viscous prefactor and grain size "
                                   "terms are defined. "
                                   "\n\n "
                                   "One may select to use the diffusion ($v_{\\text{diff}}$; $n=1$, $m!=0$), "
                                   "dislocation ($v_{\\text{disl}}$, $n>1$, $m=0$) or composite "
                                   "$\\frac{v_{\\text{diff}} v_{\\text{disl}}}{v_{\\text{diff}}+v_{\\text{disl}}}$ equation form. "
                                   "\n\n "
                                   "The diffusion and dislocation prefactors can be weakened with a factor "
                                   "between 0 and 1 according to the total or the viscous strain only. "
                                   "\n\n "
                                   "Viscosity is limited through one of two different `yielding' mechanisms. "
                                   "\n\n"
                                   "Plasticity limits viscous stress through a Drucker Prager "
                                   "yield criterion, where the yield stress in 3D is  "
                                   "$\\sigma_y = \\frac{6C\\cos(\\phi) + 2P\\sin(\\phi)} "
                                   "{\\sqrt(3)(3+\\sin(\\phi))}$ "
                                   "and "
                                   "$\\sigma_y = C\\cos(\\phi) + P\\sin(\\phi)$ "
                                   "in 2D. Above, $C$ is cohesion and $\\phi$  is the angle of "
                                   "internal friction.  Note that the 2D form is equivalent to the "
                                   "Mohr Coulomb yield surface.  If $\\phi$ is 0, the yield stress "
                                   "is fixed and equal to the cohesion (Von Mises yield criterion). "
                                   "When the viscous stress ($2v{\\varepsilon}_{ii}$) exceeds "
                                   "the yield stress, the viscosity is rescaled back to the yield "
                                   "surface: $v_{y}=\\sigma_{y}/(2{\\varepsilon}_{ii})$. "
                                   "This form of plasticity is commonly used in geodynamic models. "
                                   "See, for example, Thieulot, C. (2011), PEPI 188, pp. 47-68. "
                                   "\n\n"
                                   "The user has the option to linearly reduce the cohesion and "
                                   "internal friction angle as a function of the finite strain magnitude. "
                                   "The finite strain invariant or full strain tensor is calculated through "
                                   "compositional fields within the material model. This implementation is "
                                   "identical to the compositional field finite strain plugin and cookbook "
                                   "described in the manual (author: Gassmoeller, Dannberg). If the user selects to track "
                                   "the finite strain invariant ($e_{ii}$), a single compositional field tracks "
                                   "the value derived from $e_{ii}^t = (e_{ii})^{(t-1)} + \\dot{e}_{ii}\\; dt$, where $t$ and $t-1$ "
                                   "are the current and prior time steps, $\\dot{e}_{ii}$ is the second invariant of the "
                                   "strain rate tensor and $dt$ is the time step size. In the case of the "
                                   "full strain tensor $F$, the finite strain magnitude is derived from the "
                                   "second invariant of the symmetric stretching tensor $L$, where "
                                   "$L = F [F]^T$. The user must specify a single compositional "
                                   "field for the finite strain invariant or multiple fields (4 in 2D, 9 in 3D) "
                                   "for the finite strain tensor. These field(s) must be the first listed "
                                   "compositional fields in the parameter file. Note that one or more of the finite strain "
                                   "tensor components must be assigned a non-zero value initially. This value can be "
                                   "be quite small (e.g., 1.e-8), but still non-zero. While the option to track and use "
                                   "the full finite strain tensor exists, tracking the associated compositional fields "
                                   "is computationally expensive in 3D. Similarly, the finite strain magnitudes "
                                   "may in fact decrease if the orientation of the deformation field switches "
                                   "through time. Consequently, the ideal solution is track the finite strain "
                                   "invariant (single compositional) field within the material and track "
                                   "the full finite strain tensor through particles."
                                   "When only the second invariant of the strain is tracked, one has the option to "
                                   "track the full strain or only the plastic strain. In the latter case, strain is only tracked "
                                   "in case the material is plastically yielding, i.e. the viscous stess > yield strength. "
                                   ""
                                   "\n\n"
                                   "Viscous stress may also be limited by a non-linear stress limiter "
                                   "that has a form similar to the Peierls creep mechanism. "
                                   "This stress limiter assigns an effective viscosity "
                                   "$\\sigma_{\\text{eff}} = \\frac{\\tau_y}{2\\varepsilon_y} "
                                   "{\\frac{\\varepsilon_{ii}}{\\varepsilon_y}}^{\\frac{1}{n_y}-1}$ "
                                   "Above $\\tau_y$ is a yield stress, $\\varepsilon_y$ is the "
                                   "reference strain rate, $\\varepsilon_{ii}$ is the strain rate "
                                   "and $n_y$ is the stress limiter exponent.  The yield stress, "
                                   "$\\tau_y$, is defined through the Drucker Prager yield criterion "
                                   "formulation. This method of limiting viscous stress has been used "
                                   "in various forms within the geodynamic literature, including "
                                   "Christensen (1992), JGR, 97(B2), pp. 2015-2036; "
                                   "Cizkova and Bina (2013), EPSL, 379, pp. 95-103; "
                                   "Cizkova and Bina (2015), EPSL, 430, pp. 408-415. "
                                   "When $n_y$ is 1, it essentially becomes a linear viscosity model, "
                                   "and in the limit $n_y\\rightarrow \\infty$ it converges to the "
                                   "standard viscosity rescaling method (concretely, values $n_y>20$ "
                                   "are large enough)."
                                   "\n\n "
                                   "Compositional fields can each be assigned individual values of "
                                   "thermal diffusivity, heat capacity, density, thermal "
                                   "expansivity and rheological parameters. "
                                   "\n\n "
                                   "If more than one compositional field is present at a given "
                                   "point, viscosities are averaged with an arithmetic, geometric "
                                   "harmonic (default) or maximum composition scheme. "
                                   "\n\n "
                                   "The value for the components of this formula and additional "
                                   "parameters are read from the parameter file in subsection "
                                   " 'Material model/Visco Plastic'.")
  }
}
