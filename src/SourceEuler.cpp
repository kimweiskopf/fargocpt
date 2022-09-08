/**
	\file SourceEuler.c

	Contains routines used by the hydrodynamical loop. More specifically, it
   contains the main loop itself and all the source term substeps (with the
   exception of the evaluation of the viscous force). The transport substep is
   treated elsewhere.
*/

#include <cassert>
#include <cfloat>
#include <climits>


#include <vector>

#include "LowTasks.h"
#include "Pframeforce.h"
#include "SideEuler.h"
#include "SourceEuler.h"
#include "Theo.h"
#include "TransportEuler.h"
#include "accretion.h"
#include "boundary_conditions.h"
#include "commbound.h"
#include "constants.h"
#include "gas_torques.h"
#include "global.h"
#include "logging.h"
#include "nongnu.h"
#include "opacity.h"
#include "output.h"
#include "parameters.h"
#include "particles/particles.h"
#include "pvte_law.h"
#include "quantities.h"
#include "selfgravity.h"
#include "stress.h"
#include "sts.h"
#include "units.h"
#include "util.h"
#include "viscosity.h"
#include "simulation.h"
#include "frame_of_reference.h"

#include <cstring>

/**
	Checks polargrid for negative entries.

	\param array polargrid to check
	\returns >0 if there are negative entries, 0 otherwise
*/
// static int DetectCrash(t_polargrid *array)
// {
//     unsigned int result = 0;

//     for (unsigned int n_radial = 0; n_radial < array->Nrad; ++n_radial) {
// 	for (unsigned int n_azimuthal = 0; n_azimuthal < array->Nsec;
// 	     ++n_azimuthal) {
// 	    /// since nan < 0 is false and nan > 0 is false
// 	    /// we need to assure that array > 0 to catch bad values
// 	    if (!((*array)(n_radial, n_azimuthal) > 0.0)) {
// 		logging::print(LOG_WARNING "%s negative in cell: (%u,%u)=%g\n",
// 			       array->get_name(), n_radial, n_azimuthal,
// 			       (*array)(n_radial, n_azimuthal));
// 		result += 1;
// 	    }
// 	}
//     }

//     return result;
// }

// static void HandleCrash(t_data &data)
// {
//     if (DetectCrash(&data[t_data::SIGMA])) {
// 	logging::print(LOG_ERROR "DetectCrash: Density < 0\n");
// 	PersonalExit(1);
//     }

//     if (parameters::Adiabatic) {
// 	if (DetectCrash(&data[t_data::ENERGY])) {
// 	    logging::print(LOG_ERROR "DetectCrash: Energy < 0\n");
// 	    PersonalExit(1);
// 	}
//     }
// }

void ComputeViscousStressTensor(t_data &data)
{
    if ((parameters::artificial_viscosity ==
	 parameters::artificial_viscosity_TW) &&
	(parameters::artificial_viscosity_dissipation)) {
	viscosity::compute_viscous_terms(data, true);
    } else {
	viscosity::compute_viscous_terms(data, false);
    }
}

void SetTemperatureFloorCeilValues(t_data &data, std::string filename, int line)
{
    if (assure_temperature_range(data)) {
	logging::print(
	    LOG_DEBUG
	    "Found temperature outside the valid range of %g to %g %s in %s: %d.\n",
	    parameters::minimum_temperature, parameters::maximum_temperature,
	    units::temperature.get_cgs_symbol(), filename.c_str(), line);
    }
}

void CalculateMonitorQuantitiesAfterHydroStep(t_data &data,
						     int nTimeStep, double dt)
{
    if (data[t_data::ADVECTION_TORQUE].get_write()) {
	gas_torques::calculate_advection_torque(data, dt / parameters::DT);
    }
    if (data[t_data::VISCOUS_TORQUE].get_write()) {
	gas_torques::calculate_viscous_torque(data, dt / parameters::DT);
    }
    if (data[t_data::GRAVITATIONAL_TORQUE_NOT_INTEGRATED].get_write()) {
	gas_torques::calculate_gravitational_torque(data, dt / parameters::DT);
    }

    if (data[t_data::ALPHA_GRAV_MEAN].get_write()) {
	quantities::calculate_alpha_grav_mean_sumup(data, nTimeStep, dt / parameters::DT);
    }
    if (data[t_data::ALPHA_REYNOLDS_MEAN].get_write()) {
	quantities::calculate_alpha_reynolds_mean_sumup(data, nTimeStep,
							dt / parameters::DT);
    }
}

/**
	Assures miminum value in each cell.

	\param dst polar grid
	\param minimum_value minimum value
*/
bool assure_minimum_value(t_polargrid &dst, double minimum_value)
{
    bool found = false;
    bool is_dens = strcmp(dst.get_name(), "Sigma") == 0;

    for (unsigned int n_radial = 0; n_radial < dst.get_size_radial();
	 ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal < dst.get_size_azimuthal(); ++n_azimuthal) {
	    if (dst(n_radial, n_azimuthal) < minimum_value) {
		if (is_dens) {
		    double mass_delta =
			(minimum_value - dst(n_radial, n_azimuthal)) *
			Surf[n_radial];
		    sum_without_ghost_cells(MassDelta.FloorPositive, mass_delta,
					    n_radial);
		}
		dst(n_radial, n_azimuthal) = minimum_value;
#ifndef NDEBUG
		logging::print(LOG_DEBUG
			       "assure_minimum_value: %s(%u,%u)=%g < %g\n",
			       dst.get_name(), n_radial, n_azimuthal,
			       dst(n_radial, n_azimuthal), minimum_value);
#endif
		found = true;
	    }
	}
    }

    return found;
}

bool assure_temperature_range(t_data &data)
{
    bool found = false;

    t_polargrid &energy = data[t_data::ENERGY];
    t_polargrid &density = data[t_data::SIGMA];

    const double Tmin = parameters::minimum_temperature;

    const double Tmax = parameters::maximum_temperature;

    for (unsigned int n_radial = 0; n_radial < energy.get_size_radial();
	 ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal < energy.get_size_azimuthal(); ++n_azimuthal) {

	    const double mu = pvte::get_mu(data, n_radial, n_azimuthal);
	    const double gamma_eff =
		pvte::get_gamma_eff(data, n_radial, n_azimuthal);

	    const double minimum_energy = Tmin *
					  density(n_radial, n_azimuthal) / mu *
					  constants::R / (gamma_eff - 1.0);

	    const double maximum_energy = Tmax *
					  density(n_radial, n_azimuthal) / mu *
					  constants::R / (gamma_eff - 1.0);

	    if (!(energy(n_radial, n_azimuthal) > minimum_energy)) {
#ifndef NDEBUG
		logging::print(
		    LOG_DEBUG "assure_minimum_temperature: (%u,%u)=%g<%g\n",
		    n_radial, n_azimuthal,
		    energy(n_radial, n_azimuthal) *
			units::temperature.get_cgs_factor() /
			density(n_radial, n_azimuthal) * mu / constants::R *
			(gamma_eff - 1.0),
		    Tmin * units::temperature.get_cgs_factor(), Tmin);
#endif
		energy(n_radial, n_azimuthal) =
		    Tmin * density(n_radial, n_azimuthal) / mu * constants::R /
		    (gamma_eff - 1.0);
		found = true;
	    }

	    if (!(energy(n_radial, n_azimuthal) < maximum_energy)) {
#ifndef NDEBUG
		logging::print(
		    LOG_DEBUG "assure_maximum_temperature: (%u,%u)=%g>%g\n",
		    n_radial, n_azimuthal,
		    energy(n_radial, n_azimuthal) *
			units::temperature.get_cgs_factor() /
			density(n_radial, n_azimuthal) * mu / constants::R *
			(gamma_eff - 1.0),
		    Tmax * units::temperature.get_cgs_factor(), Tmax);
#endif
		energy(n_radial, n_azimuthal) =
		    Tmax * density(n_radial, n_azimuthal) / mu * constants::R /
		    (gamma_eff - 1.0);
		found = true;
	    }
	}
    }

    return found;
}

void recalculate_derived_disk_quantities(t_data &data, bool force_update)
{

    if (parameters::Locally_Isothermal) {
	if (parameters::ASPECTRATIO_MODE > 0) {
	    compute_sound_speed(data, force_update);
	    compute_pressure(data, force_update);
	    compute_temperature(data, force_update);
	    compute_scale_height(data, force_update);
	} else {
	    compute_pressure(data, force_update);
	}
    }
    if (parameters::Adiabatic || parameters::Polytropic) {
	if (parameters::variableGamma) {
	    pvte::compute_gamma_mu(data);
	}
	compute_temperature(data, force_update);
	compute_sound_speed(data, force_update);
	compute_scale_height(data, force_update);
	compute_pressure(data, force_update);
    }

    viscosity::update_viscosity(data);
}

void init_euler(t_data &data)
{
    InitCellCenterCoordinates();
    InitTransport();

    if (parameters::Locally_Isothermal) {
	compute_sound_speed(data, true);
	compute_pressure(data, true);
	compute_temperature(data, true);
	compute_scale_height(data, true);
    }

    if (parameters::Adiabatic || parameters::Polytropic) {
	if (parameters::variableGamma) {
	    compute_sound_speed(data, true);
	    compute_scale_height(data, true);
	    pvte::compute_gamma_mu(data);
	}
	compute_temperature(data, true);
	compute_sound_speed(data, true);
	compute_scale_height(data, true);
	compute_pressure(data, true);
    }

    viscosity::update_viscosity(data);
    compute_heating_cooling_for_CFL(data);
}

/**

*/
void FreeEuler()
{
    FreeTransport();
    FreeCellCenterCoordinates();
}

/**
	copy one polar grid into another

	\param dst destination polar grid
	\param src source polar grid
*/
void copy_polargrid(t_polargrid &dst, const t_polargrid &src)
{
    assert((dst.get_size_radial() == src.get_size_radial()) &&
	   (dst.get_size_azimuthal() == src.get_size_azimuthal()));

    std::memcpy(dst.Field, src.Field,
		dst.get_size_radial() * dst.get_size_azimuthal() *
		    sizeof(*dst.Field));
}

/**
	switch one polar grid with another

	\param dst destination polar grid
	\param src source polar grid
	switches polar grids
*/
void move_polargrid(t_polargrid &dst, t_polargrid &src)
{
    assert((dst.get_size_radial() == src.get_size_radial()) &&
	   (dst.get_size_azimuthal() == src.get_size_azimuthal()));

    std::swap(dst.Field, src.Field);
}

/**
	In this substep we take into account the source part of Euler equations.
   We evolve velocities with pressure gradients, gravitational forces and
   curvature terms
*/
void update_with_sourceterms(t_data &data, const double dt)
{
    double supp_torque = 0.0; // for imposed disk drift

    if (parameters::Adiabatic) {
	for (unsigned int n_radial = 0;
	     n_radial <= data[t_data::ENERGY].get_max_radial(); ++n_radial) {
	    for (unsigned int n_azimuthal = 0;
		 n_azimuthal <= data[t_data::ENERGY].get_max_azimuthal();
		 ++n_azimuthal) {
		// div(v) = 1/r d(r*v_r)/dr + 1/r d(v_phi)/dphi
		const double DIV_V =
		    (data[t_data::V_RADIAL](n_radial + 1, n_azimuthal) *
			 Ra[n_radial + 1] -
		     data[t_data::V_RADIAL](n_radial, n_azimuthal) *
			 Ra[n_radial]) *
			InvDiffRsup[n_radial] * InvRb[n_radial] +
		    (data[t_data::V_AZIMUTHAL](
			 n_radial,
			 n_azimuthal == data[t_data::ENERGY].get_max_azimuthal()
			     ? 0
			     : n_azimuthal + 1) -
		     data[t_data::V_AZIMUTHAL](n_radial, n_azimuthal)) *
			invdphi * InvRb[n_radial];

		const double gamma =
		    pvte::get_gamma_eff(data, n_radial, n_azimuthal);

		/*
		// Like D'Angelo et al. 2003 eq. 25
		const double P = (gamma - 1.0) * data[t_data::ENERGY](n_radial,
		n_azimuthal); const double dE = dt * (-P*DIV_V + 0.5*(gamma
		- 1.0) * P * dt * std::pow(DIV_V, 2)); const double energy_old =
		data[t_data::ENERGY](n_radial, n_azimuthal); const double
		energy_new = energy_old + dE; data[t_data::ENERGY](n_radial,
		n_azimuthal) = energy_new;
		*/

		// Like D'Angelo et al. 2003 eq. 24
		const double energy_old =
		    data[t_data::ENERGY](n_radial, n_azimuthal);
		const double energy_new =
		    energy_old * std::exp(-(gamma - 1.0) * dt * DIV_V);
		data[t_data::ENERGY](n_radial, n_azimuthal) = energy_new;

		/*
		// Zeus2D like, see Stone & Norman 1992
		// produces poor results with shock tube test
		const double P = (gamma - 1.0);
		const double energy_old = data[t_data::ENERGY](n_radial,
		n_azimuthal); const double energy_new = energy_old*(1.0 -
		0.5*dt*P*DIV_V)/(1.0 + 0.5*dt*P*DIV_V);
		data[t_data::ENERGY](n_radial, n_azimuthal) = energy_new;
		*/
	    }
	}
    }

    // update v_radial with source terms
    for (unsigned int n_radial = 1;
	 n_radial <= data[t_data::V_RADIAL].get_max_radial() - 1; ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= data[t_data::V_RADIAL].get_max_azimuthal();
	     ++n_azimuthal) {
	    // 1/Sigma * dP/dr : Sigma is calculated as a mean value between the
	    // neightbour cells
	    const double gradp =
		2.0 /
		(data[t_data::SIGMA](n_radial, n_azimuthal) +
		 data[t_data::SIGMA](n_radial - 1, n_azimuthal)) *
		(data[t_data::PRESSURE](n_radial, n_azimuthal) -
		 data[t_data::PRESSURE](n_radial - 1, n_azimuthal)) *
		InvDiffRmed[n_radial];

	    // dPhi/dr
	    double gradphi;
	    if (parameters::body_force_from_potential) {
		gradphi = (data[t_data::POTENTIAL](n_radial, n_azimuthal) -
			   data[t_data::POTENTIAL](n_radial - 1, n_azimuthal)) *
			  InvDiffRmed[n_radial];
	    } else {
		gradphi = -data[t_data::ACCEL_RADIAL](n_radial, n_azimuthal);
	    }

	    // v_phi^2/r : v_phi^2 is calculated by a mean in both directions
	    double vt2 =
		data[t_data::V_AZIMUTHAL](n_radial, n_azimuthal) +
		data[t_data::V_AZIMUTHAL](
		    n_radial,
		    n_azimuthal == data[t_data::V_AZIMUTHAL].get_max_azimuthal()
			? 0
			: n_azimuthal + 1) +
		data[t_data::V_AZIMUTHAL](n_radial - 1, n_azimuthal) +
		data[t_data::V_AZIMUTHAL](
		    n_radial - 1,
		    n_azimuthal == data[t_data::V_AZIMUTHAL].get_max_azimuthal()
			? 0
			: n_azimuthal + 1);
	    vt2 = 0.25 * vt2 + Rinf[n_radial] * refframe::OmegaFrame;
	    vt2 = vt2 * vt2;

		const double InvR = 2.0 / (Rmed[n_radial] + Rmed[n_radial-1]);

	    // add all terms to new v_radial: v_radial_new = v_radial +
	    // dt*(source terms)
	    data[t_data::V_RADIAL](n_radial, n_azimuthal) =
		data[t_data::V_RADIAL](n_radial, n_azimuthal) +
		dt * (-gradp - gradphi + vt2 * InvR);

	}
    }

    // update v_azimuthal with source terms
    for (unsigned int n_radial = 0;
	 n_radial <= data[t_data::V_AZIMUTHAL].get_max_radial(); ++n_radial) {
	if (parameters::IMPOSEDDISKDRIFT != 0.0) {
	    supp_torque = parameters::IMPOSEDDISKDRIFT * 0.5 *
			  std::pow(Rmed[n_radial], -2.5 + parameters::SIGMASLOPE);
	}
	//const double invdxtheta = 1.0 / (dphi * Rmed[n_radial]);
	const double invdxtheta = 2.0 / (dphi * (Rsup[n_radial]+Rinf[n_radial]));

	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= data[t_data::V_AZIMUTHAL].get_max_azimuthal();
	     ++n_azimuthal) {

	    const double n_az_minus =
		(n_azimuthal == 0 ? data[t_data::PRESSURE].get_max_azimuthal()
				  : n_azimuthal - 1);
	    // 1/Sigma 1/r dP/dphi
	    const double gradp =
		2.0 /
		(data[t_data::SIGMA](n_radial, n_azimuthal) +
		 data[t_data::SIGMA](
		     n_radial, n_azimuthal == 0
				   ? data[t_data::SIGMA].get_max_azimuthal()
				   : n_azimuthal - 1)) *
		(data[t_data::PRESSURE](n_radial, n_azimuthal) -
		 data[t_data::PRESSURE](n_radial, n_az_minus)) *
		invdxtheta;

	    // 1/r dPhi/dphi
	    double gradphi;
	    if (parameters::body_force_from_potential) {
		gradphi = (data[t_data::POTENTIAL](n_radial, n_azimuthal) -
			   data[t_data::POTENTIAL](n_radial, n_az_minus)) *
			  invdxtheta;
	    } else {
		gradphi = -data[t_data::ACCEL_AZIMUTHAL](n_radial, n_azimuthal);
	    }

	    // add all terms to new v_azimuthal: v_azimuthal_new = v_azimuthal +
	    // dt*(source terms)
	    data[t_data::V_AZIMUTHAL](n_radial, n_azimuthal) =
		data[t_data::V_AZIMUTHAL](n_radial, n_azimuthal) +
		dt * (-gradp - gradphi);

	    if (parameters::IMPOSEDDISKDRIFT != 0.0) {
		// add term for imposed disk drift
		data[t_data::V_AZIMUTHAL](n_radial, n_azimuthal) +=
		    dt * supp_torque;
	    }
	}
    }

    if (parameters::self_gravity) {
	selfgravity::compute(data, dt, true);
    }
}

/**
	In this substep we add the articifial viscous pressure source terms.
	Shocks are spread over CVNR zones: von Neumann-Richtmyer viscosity
   constant; Beware of misprint in Stone and Norman's paper : use C2^2 instead
   of C2
*/
void update_with_artificial_viscosity(t_data &data, const double dt)
{

    /// Do not Apply sub keplerian boundary for boundary conditions that set
    /// Vphi themselves
    const bool add_kep_inner =
	(parameters::boundary_inner !=
	 parameters::boundary_condition_evanescent) &&
	(parameters::boundary_inner !=
	 parameters::boundary_condition_boundary_layer) &&
	(parameters::boundary_inner !=
	 parameters::boundary_condition_precribed_time_variable) &&
	(!parameters::domegadr_zero);

    if (add_kep_inner) {
	ApplySubKeplerianBoundaryInner(data[t_data::V_AZIMUTHAL]);
    }

    if ((parameters::boundary_outer !=
	 parameters::boundary_condition_center_of_mass_initial) &&
	(parameters::boundary_outer !=
	 parameters::boundary_condition_zero_gradient) &&
	(parameters::boundary_outer !=
	 parameters::boundary_condition_evanescent) &&
	(parameters::boundary_outer !=
	 parameters::boundary_condition_boundary_layer) &&
	(parameters::boundary_outer !=
	 parameters::boundary_condition_precribed_time_variable) &&
	(!parameters::massoverflow) && (!parameters::domegadr_zero)) {
	ApplySubKeplerianBoundaryOuter(data[t_data::V_AZIMUTHAL],
				       add_kep_inner);
    }

    if (parameters::artificial_viscosity ==
	    parameters::artificial_viscosity_SN &&
	parameters::EXPLICIT_VISCOSITY) {

	// calculate q_r and q_phi
	for (unsigned int n_radial = 0;
	     n_radial <= data[t_data::Q_R].get_max_radial(); ++n_radial) {
	    for (unsigned int n_azimuthal = 0;
		 n_azimuthal <= data[t_data::Q_R].get_max_azimuthal();
		 ++n_azimuthal) {
		double dv_r =
		    data[t_data::V_RADIAL](n_radial + 1, n_azimuthal) -
		    data[t_data::V_RADIAL](n_radial, n_azimuthal);
		if (dv_r < 0.0) {
		    data[t_data::Q_R](n_radial, n_azimuthal) =
			std::pow(parameters::artificial_viscosity_factor, 2) *
			data[t_data::SIGMA](n_radial, n_azimuthal) *
			std::pow(dv_r, 2);
		} else {
		    data[t_data::Q_R](n_radial, n_azimuthal) = 0.0;
		}

		double dv_phi =
		    data[t_data::V_AZIMUTHAL](
			n_radial,
			n_azimuthal ==
				data[t_data::V_AZIMUTHAL].get_max_azimuthal()
			    ? 0
			    : n_azimuthal + 1) -
		    data[t_data::V_AZIMUTHAL](n_radial, n_azimuthal);
		if (dv_phi < 0.0) {
		    data[t_data::Q_PHI](n_radial, n_azimuthal) =
			std::pow(parameters::artificial_viscosity_factor, 2) *
			data[t_data::SIGMA](n_radial, n_azimuthal) *
			std::pow(dv_phi, 2);
		} else {
		    data[t_data::Q_PHI](n_radial, n_azimuthal) = 0.0;
		}
	    }
	}

	// If gas disk is adiabatic, we add artificial viscosity as a source
	// term for advection of thermal energy polargrid
	// perform this update before the velocities are updated
	if (parameters::Adiabatic) {
	    if (parameters::artificial_viscosity_dissipation) {
		for (unsigned int n_radial = 0;
		     n_radial <= data[t_data::ENERGY].get_max_radial();
		     ++n_radial) {
		    const double dxtheta = dphi * Rmed[n_radial];
		    const double invdxtheta = 1.0 / dxtheta;
		    for (unsigned int n_azimuthal = 0;
			 n_azimuthal <=
			 data[t_data::ENERGY].get_max_azimuthal();
			 ++n_azimuthal) {
			data[t_data::ENERGY](n_radial, n_azimuthal) =
			    data[t_data::ENERGY](n_radial, n_azimuthal) -
			    dt * data[t_data::Q_R](n_radial, n_azimuthal) *
				(data[t_data::V_RADIAL](n_radial + 1,
							n_azimuthal) -
				 data[t_data::V_RADIAL](n_radial,
							n_azimuthal)) *
				InvDiffRsup[n_radial] -
			    dt * data[t_data::Q_PHI](n_radial, n_azimuthal) *
				(data[t_data::V_AZIMUTHAL](
				     n_radial,
				     n_azimuthal == data[t_data::V_AZIMUTHAL]
							.get_max_azimuthal()
					 ? 0
					 : n_azimuthal + 1) -
				 data[t_data::V_AZIMUTHAL](n_radial,
							   n_azimuthal)) *
				invdxtheta;
		    }
		}
	    }
	}

	// add artificial viscous pressure source term to v_radial
	for (unsigned int n_radial = 1;
	     n_radial <= data[t_data::V_RADIAL].get_max_radial() - 1;
	     ++n_radial) {
	    for (unsigned int n_azimuthal = 0;
		 n_azimuthal <= data[t_data::V_RADIAL].get_max_azimuthal();
		 ++n_azimuthal) {
		// 1/Sigma dq_r/dr : Sigma is calculated as a mean value between
		// the neightbour cells
		data[t_data::V_RADIAL](n_radial, n_azimuthal) =
		    data[t_data::V_RADIAL](n_radial, n_azimuthal) -
		    dt * 2.0 /
			(data[t_data::SIGMA](n_radial, n_azimuthal) +
			 data[t_data::SIGMA](n_radial - 1, n_azimuthal)) *
			(data[t_data::Q_R](n_radial, n_azimuthal) -
			 data[t_data::Q_R](n_radial - 1, n_azimuthal)) *
			InvDiffRmed[n_radial];
	    }
	}

	// add artificial viscous pressure source term to v_azimuthal
	for (unsigned int n_radial = 0;
	     n_radial <= data[t_data::V_AZIMUTHAL].get_max_radial();
	     ++n_radial) {
	    const double dxtheta = dphi * Rmed[n_radial];
	    const double invdxtheta = 1.0 / dxtheta;
	    for (unsigned int n_azimuthal = 0;
		 n_azimuthal <= data[t_data::V_AZIMUTHAL].get_max_azimuthal();
		 ++n_azimuthal) {
		// 1/Sigma 1/r dq_phi/dphi : Sigma is calculated as a mean value
		// between the neightbour cells
		data[t_data::V_AZIMUTHAL](n_radial, n_azimuthal) =
		    data[t_data::V_AZIMUTHAL](n_radial, n_azimuthal) -
		    dt * 2.0 /
			(data[t_data::SIGMA](n_radial, n_azimuthal) +
			 data[t_data::SIGMA](
			     n_radial,
			     n_azimuthal == 0
				 ? data[t_data::SIGMA].get_max_azimuthal()
				 : n_azimuthal - 1)) *
			(data[t_data::Q_PHI](n_radial, n_azimuthal) -
			 data[t_data::Q_PHI](
			     n_radial,
			     n_azimuthal == 0
				 ? data[t_data::Q_PHI].get_max_azimuthal()
				 : n_azimuthal - 1)) *
			invdxtheta;
	    }
	}
    }
}

void irradiation(t_data &data) {

	const auto & plsys = data.get_planetary_system();
	const unsigned int Npl = plsys.get_number_of_planets();

	for (unsigned int npl=0; npl < Npl; npl ++) { 
		const auto& planet = plsys.get_planet(npl);
		if (planet.get_irradiate()) {
			irradiation_single(data, planet);
		}
	}
}

void irradiation_single(t_data &data, const t_planet &planet) {

	const double rampup_time = planet.get_irradiation_rampuptime();

	double ramping = 1.0;
	
	if (PhysicalTime < rampup_time) {
	    ramping =
		1.0 -
		std::pow(std::cos(PhysicalTime * M_PI / 2.0 / rampup_time), 2);
	}

	const double x = planet.get_x();
	const double y = planet.get_y();
	const double radius = planet.get_planet_radial_extend();
	const double temperature = planet.get_temperature();

	const unsigned int Nrad = data[t_data::QPLUS].get_max_radial();
	const unsigned int Naz = data[t_data::QPLUS].get_max_azimuthal();
	// Simple star heating (see Masterthesis Alexandros Ziampras)
	for (unsigned int nrad = 1; nrad < Nrad; ++nrad) {
	for (unsigned int naz = 0; naz <= Naz; ++naz) {
		const unsigned int ncell = nrad * data[t_data::SIGMA].get_size_azimuthal() + naz;
		const double xc = CellCenterX->Field[ncell];
		const double yc = CellCenterY->Field[ncell];
		const double distance = std::sqrt(std::pow(x - xc, 2) + std::pow(y - yc, 2));
		const double HoverR = data[t_data::SCALE_HEIGHT](nrad, naz) * InvRmed[nrad];
		const double sigma = constants::sigma.get_code_value();
		const double tau_eff = data[t_data::TAU_EFF](nrad, naz);
		const double eps = 0.5; // TODO: add a parameter
		// choose according to Chiang & Goldreich (1997)
		const double dlogH_dlogr = 9.0 / 7.0;
		
		// irradiation contribution near and far from the star
		// see D'Angelo & Marzari 2012
		const double roverd = distance < radius ? 1.0 : radius/distance;
		const double W_G = 0.4 * roverd + HoverR * (dlogH_dlogr - 1.0);

		// use eq. 7 from Menou & Goodman (2004) (rearranged), Qirr
		// = 2*(1-eps)*L_star/(4 pi r^2)*(dlogH/dlogr - 1) * H/r *
		// 1/Tau_eff here we use (1-eps) =
		// parameters::heating_star_factor L_star = 4 pi R_star^2
		// sigma_sb T_star^4
		// with modifications from D'Angelo & Marzari 2012
		// for near and far field
		double qplus = 2 * (1 - eps);
		qplus *= sigma * std::pow(temperature, 4) * std::pow(roverd, 2); // *L_star/(4 pi r^2)
		qplus *= W_G;
		qplus /= tau_eff;			// * 1/Tau_eff
		data[t_data::QPLUS](nrad, naz) += ramping * qplus;
	}
	}
	
}

void viscous_heating(t_data &data) {

	/* We calculate the heating source term Qplus from i=1 to max-1 */
	for (unsigned int n_radial = 1;
		n_radial < data[t_data::QPLUS].get_max_radial(); ++n_radial) {
	for (unsigned int n_azimuthal = 0;
		n_azimuthal <= data[t_data::QPLUS].get_max_azimuthal();
		++n_azimuthal) {
		if (data[t_data::VISCOSITY](n_radial, n_azimuthal) != 0.0) {
			// average tau_r_phi over 4 cells
			double tau_r_phi =
			0.25 *
			(data[t_data::TAU_R_PHI](n_radial, n_azimuthal) +
				data[t_data::TAU_R_PHI](n_radial + 1, n_azimuthal) +
				data[t_data::TAU_R_PHI](
					n_radial,
					n_azimuthal ==
						data[t_data::TAU_R_PHI].get_max_azimuthal()
					? 0
					: n_azimuthal + 1) +
				data[t_data::TAU_R_PHI](
					n_radial + 1,
					n_azimuthal ==
						data[t_data::TAU_R_PHI].get_max_azimuthal()
					? 0
					: n_azimuthal + 1));

			double qplus =
			1.0 /
			(2.0 * data[t_data::VISCOSITY](n_radial, n_azimuthal) *
				data[t_data::SIGMA](n_radial, n_azimuthal)) *
			(std::pow(data[t_data::TAU_R_R](n_radial, n_azimuthal),
					2) +
				2 * std::pow(tau_r_phi, 2) +
				std::pow(
					data[t_data::TAU_PHI_PHI](n_radial, n_azimuthal),
					2));
			qplus +=
			(2.0 / 9.0) *
			data[t_data::VISCOSITY](n_radial, n_azimuthal) *
			data[t_data::SIGMA](n_radial, n_azimuthal) *
			std::pow(data[t_data::DIV_V](n_radial, n_azimuthal), 2);

			qplus *= parameters::heating_viscous_factor;
			data[t_data::QPLUS](n_radial, n_azimuthal) += qplus;
		}
	}}
}

void calculate_qplus(t_data &data)
{

	// clear up all Qplus terms
	data[t_data::QPLUS].clear();

    if (parameters::heating_viscous_enabled && parameters::EXPLICIT_VISCOSITY) {
		viscous_heating(data);
    }
    if (parameters::heating_star_enabled) {
		if (!parameters::cooling_radiative_enabled) {
			// TODO: make it properly!
			die("Need to calulate Tau_eff first!\n"); 
		}
		irradiation(data);
	}    
}

void calculate_qminus(t_data &data)
{
    // clear up all Qminus terms
    data[t_data::QMINUS].clear();

    // beta cooling
    if (parameters::cooling_beta_enabled) {
	for (unsigned int n_radial = 1;
	     n_radial < data[t_data::QMINUS].get_max_radial(); ++n_radial) {
	    for (unsigned int n_azimuthal = 0;
		 n_azimuthal <= data[t_data::QMINUS].get_max_azimuthal();
		 ++n_azimuthal) {
		// Q- = E Omega/beta
		const double r = Rmed[n_radial];
		const double omega_k = calculate_omega_kepler(r);
		const double E = data[t_data::ENERGY](n_radial, n_azimuthal);
		const double t_ramp_up = parameters::cooling_beta_ramp_up;

		double beta_inv = 1 / parameters::cooling_beta;
		if (t_ramp_up > 0.0) {
		    const double t = PhysicalTime;
		    double ramp_factor =
			1 - std::exp(-std::pow(2 * t / t_ramp_up, 2));
		    beta_inv = beta_inv * ramp_factor;
		}

		double delta_E = E;
		if (parameters::cooling_beta_initial) {
		    const double sigma =
			data[t_data::SIGMA](n_radial, n_azimuthal);
		    const double sigma0 =
			data[t_data::SIGMA0](n_radial, n_azimuthal);
		    const double E0 =
			data[t_data::ENERGY0](n_radial, n_azimuthal);
		    delta_E -= E0 / sigma0 * sigma;
		}
		if (parameters::cooling_beta_aspect_ratio) {
		    const double sigma =
			data[t_data::SIGMA](n_radial, n_azimuthal);
		    const double E0 =
			1.0 / (parameters::ADIABATICINDEX - 1.0) *
			std::pow(parameters::ASPECTRATIO_REF, 2) *
			std::pow(Rmed[n_radial], 2.0 * parameters::FLARINGINDEX - 1.0) *
			constants::G * hydro_center_mass * sigma;
		    delta_E -= E0;
		}
		const double qminus = delta_E * omega_k * beta_inv;


		data[t_data::QMINUS](n_radial, n_azimuthal) += qminus;
	    }
	}
    }

    // local radiative cooling
    if (parameters::cooling_radiative_enabled) {

	for (unsigned int n_radial = 1;
	     n_radial < data[t_data::QMINUS].get_max_radial(); ++n_radial) {
	    for (unsigned int n_azimuthal = 0;
		 n_azimuthal <= data[t_data::QMINUS].get_max_azimuthal();
		 ++n_azimuthal) {
		// calculate Rosseland mean opacity kappa. opaclin needs values
		// in cgs units
		const double temperatureCGS =
		    data[t_data::TEMPERATURE](n_radial, n_azimuthal) *
		    units::temperature;

		const double H =
		    data[t_data::SCALE_HEIGHT](n_radial, n_azimuthal);

		const double densityCGS =
		    data[t_data::SIGMA](n_radial, n_azimuthal) /
		    (parameters::density_factor * H) * units::density;

		const double kappaCGS =
		    opacity::opacity(densityCGS, temperatureCGS);

		data[t_data::KAPPA](n_radial, n_azimuthal) =
		    parameters::kappa_factor * kappaCGS *
		    units::opacity.get_inverse_cgs_factor();

		// mean vertical optical depth: tau = 1/2 kappa Sigma
		data[t_data::TAU](n_radial, n_azimuthal) =
		    parameters::tau_factor *
		    (1.0 / parameters::density_factor) *
		    data[t_data::KAPPA](n_radial, n_azimuthal) *
		    data[t_data::SIGMA](n_radial, n_azimuthal);


		if(parameters::heating_star_enabled){
		//  irradiated disk tau_eff = 3/8 tau + 1/2 + 1/(4*tau+tau_min)
		//  compare D'Angelo & Marzari 2012
		data[t_data::TAU_EFF](n_radial, n_azimuthal) =
		    3.0 / 8.0 * data[t_data::TAU](n_radial, n_azimuthal) +
			0.5 +
		    1.0 /
			(4.0 * data[t_data::TAU](n_radial, n_azimuthal) + parameters::tau_min);
		} else {
			//  non irradiated disk tau_eff = 3/8 tau + sqrt(3)/4 + 1/(4*tau+tau_min)
			data[t_data::TAU_EFF](n_radial, n_azimuthal) =
				3.0 / 8.0 * data[t_data::TAU](n_radial, n_azimuthal) +
				std::sqrt(3.0) / 4.0 +
				1.0 /
				(4.0 * data[t_data::TAU](n_radial, n_azimuthal) + parameters::tau_min);
		}

		if (parameters::opacity ==
		    parameters::opacity_simple) { // Compare D'Angelo et. al
						  // 2003 eq.(28)
		    data[t_data::TAU_EFF](n_radial, n_azimuthal) =
			3.0 / 8.0 * data[t_data::TAU](n_radial, n_azimuthal);
		}
		// Q = factor 2 sigma_sb T^4 / tau_eff

		const double factor = parameters::cooling_radiative_factor;
		const double sigma_sb = constants::sigma.get_code_value();
		const double T4 = std::pow(
		    data[t_data::TEMPERATURE](n_radial, n_azimuthal), 4);
		const double tau_eff =
		    data[t_data::TAU_EFF](n_radial, n_azimuthal);
		const double Tmin4 =
		    std::pow(parameters::minimum_temperature *
				 units::temperature.get_inverse_cgs_factor(),
			     4);

		const double qminus =
		    factor * 2 * sigma_sb * (T4 - Tmin4) / tau_eff;

		data[t_data::QMINUS](n_radial, n_azimuthal) += qminus;
	    }
	}
    }
}

/**
	In this substep we take into account the source part of energy equation.
   We evolve internal energy with compression/dilatation and heating terms
*/
void SubStep3(t_data &data, const double dt)
{
    calculate_qminus(data); // first to calculate teff
    calculate_qplus(data);

    // calculate tau_cool if needed for output
    if (data[t_data::TAU_COOL].get_write_1D() ||
	data[t_data::TAU_COOL].get_write_2D()) {
	for (unsigned int n_radial = 0;
	     n_radial <= data[t_data::TAU_COOL].get_max_radial(); ++n_radial) {
	    for (unsigned int n_azimuthal = 0;
		 n_azimuthal <= data[t_data::TAU_COOL].get_max_azimuthal();
		 ++n_azimuthal) {
		data[t_data::TAU_COOL](n_radial, n_azimuthal) =
		    data[t_data::ENERGY](n_radial, n_azimuthal) /
		    data[t_data::QMINUS](n_radial, n_azimuthal);
	    }
	}
    }

    // calculate pDV for write out
    if (data[t_data::P_DIVV].get_write_1D() ||
	data[t_data::P_DIVV].get_write_2D() ||
	parameters::radiative_diffusion_enabled) {
	data.pdivv_total = 0;
	for (unsigned int n_radial = 0;
	     n_radial <= data[t_data::ENERGY].get_max_radial(); ++n_radial) {
	    for (unsigned int n_azimuthal = 0;
		 n_azimuthal <= data[t_data::ENERGY].get_max_azimuthal();
		 ++n_azimuthal) {
		double pdivv =
		    (pvte::get_gamma_eff(data, n_radial, n_azimuthal) - 1.0) *
		    dt * data[t_data::DIV_V](n_radial, n_azimuthal) *
		    data[t_data::ENERGY](n_radial, n_azimuthal);
		data[t_data::P_DIVV](n_radial, n_azimuthal) = pdivv;

		sum_without_ghost_cells(data.pdivv_total, pdivv, n_radial);
	    }
	}
    }

    // Now we can update energy with source terms
    for (unsigned int n_radial = 1;
	 n_radial < data[t_data::ENERGY].get_max_radial(); ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= data[t_data::ENERGY].get_max_azimuthal();
	     ++n_azimuthal) {

	    const double sigma_sb = constants::sigma;
	    const double c = constants::c;
	    const double mu = pvte::get_mu(data, n_radial, n_azimuthal);
	    const double gamma =
		pvte::get_gamma_eff(data, n_radial, n_azimuthal);

	    const double Rgas = constants::R;

	    const double H = data[t_data::SCALE_HEIGHT](n_radial, n_azimuthal);

	    const double sigma = data[t_data::SIGMA](n_radial, n_azimuthal);
	    const double energy = data[t_data::ENERGY](n_radial, n_azimuthal);

	    const double inv_pow4 =
		std::pow(mu * (gamma - 1.0) / (Rgas * sigma), 4);
	    double alpha = 1.0 + 2.0 * H * 4.0 * sigma_sb / c * inv_pow4 *
				     std::pow(energy, 3);

	    data[t_data::QPLUS](n_radial, n_azimuthal) /= alpha;
	    data[t_data::QMINUS](n_radial, n_azimuthal) /= alpha;
	    const double Qplus = data[t_data::QPLUS](n_radial, n_azimuthal);
	    const double Qminus = data[t_data::QMINUS](n_radial, n_azimuthal);

	    double energy_new = energy + dt * (Qplus - Qminus);

	    const double SigmaFloor =
		10.0 * parameters::sigma0 * parameters::sigma_floor;
	    // If the cell is too close to the density floor
	    // we set energy to equilibrium energy
	    if ((sigma < SigmaFloor)) {
		const double tau_eff =
		    data[t_data::TAU_EFF](n_radial, n_azimuthal);
		const double e4 = Qplus * tau_eff / (2.0 * sigma_sb);
		const double constant = (Rgas / mu * sigma / (gamma - 1.0));
		// energy, where current heating cooling rate are in equilibirum
		const double eq_energy = std::pow(e4, 1.0 / 4.0) * constant;

		data[t_data::QMINUS](n_radial, n_azimuthal) = Qplus;
		energy_new = eq_energy;
	    }

	    data[t_data::ENERGY](n_radial, n_azimuthal) = energy_new;
	}
    }

	SetTemperatureFloorCeilValues(data, __FILE__, __LINE__);
}

static inline double flux_limiter(const double R)
{
    // flux limiter
    if (R <= 2) {
	return 2.0 / (3 + std::sqrt(9 + 10 * std::pow(R, 2)));
    } else {
	return 10.0 / (10 * R + 9 + std::sqrt(180 * R + 81));
    }
}

void radiative_diffusion(t_data &data, const double dt)
{
    static bool grids_allocated = false;
    static t_polargrid Ka, Kb;
    static t_polargrid A, B, C, D, E;
    static t_polargrid Told;
    static double *SendInnerBoundary, *SendOuterBoundary, *RecvInnerBoundary,
	*RecvOuterBoundary;

    if (!grids_allocated) {
	Ka.set_vector(true);
	Ka.set_size(data.get_n_radial(), data.get_n_azimuthal());
	Kb.set_scalar(true);
	Kb.set_size(data.get_n_radial(), data.get_n_azimuthal());

	A.set_scalar(true);
	A.set_size(data.get_n_radial(), data.get_n_azimuthal());
	B.set_scalar(true);
	B.set_size(data.get_n_radial(), data.get_n_azimuthal());
	C.set_scalar(true);
	C.set_size(data.get_n_radial(), data.get_n_azimuthal());
	D.set_scalar(true);
	D.set_size(data.get_n_radial(), data.get_n_azimuthal());
	E.set_scalar(true);
	E.set_size(data.get_n_radial(), data.get_n_azimuthal());

	Told.set_scalar(true);
	Told.set_size(data.get_n_radial(), data.get_n_azimuthal());

	// create arrays for communcation
	SendInnerBoundary =
	    (double *)malloc(NAzimuthal * CPUOVERLAP * sizeof(double));
	SendOuterBoundary =
	    (double *)malloc(NAzimuthal * CPUOVERLAP * sizeof(double));
	RecvInnerBoundary =
	    (double *)malloc(NAzimuthal * CPUOVERLAP * sizeof(double));
	RecvOuterBoundary =
	    (double *)malloc(NAzimuthal * CPUOVERLAP * sizeof(double));

	grids_allocated = true;
    }

    auto &Temperature = data[t_data::TEMPERATURE];
    auto &Sigma = data[t_data::SIGMA];
    auto &Energy = data[t_data::ENERGY];
    auto &Scale_height = data[t_data::SCALE_HEIGHT];

    // We set minimum Temperature here such that H (thru Cs) is also computed
    // with the minimum Temperature
    for (unsigned int naz = 0; naz < Energy.get_size_azimuthal(); ++naz) {
	const unsigned int nr_max = Energy.get_max_radial();

	if (CPU_Rank == 0 &&
	    parameters::boundary_inner == parameters::boundary_condition_open) {
	    const double Tmin = parameters::minimum_temperature;

	    const double mu = pvte::get_mu(data, 1, naz);
	    const double gamma_eff = pvte::get_gamma_eff(data, 1, naz);
	    Sigma(0, naz) = Sigma(1, naz);

	    const double minimum_energy =
		Tmin * Sigma(1, naz) / mu * constants::R / (gamma_eff - 1.0);

	    Energy(0, naz) = minimum_energy;
	}

	if (CPU_Rank == CPU_Highest &&
	    parameters::boundary_outer == parameters::boundary_condition_open) {
	    const double Tmin = parameters::minimum_temperature; 

	    const double mu = pvte::get_mu(data, nr_max - 1, naz);
	    const double gamma_eff = pvte::get_gamma_eff(data, nr_max - 1, naz);
	    Sigma(nr_max, naz) = Sigma(nr_max - 1, naz);

	    const double minimum_energy = Tmin * Sigma(nr_max - 1, naz) / mu *
					  constants::R / (gamma_eff - 1.0);

	    Energy(nr_max, naz) = minimum_energy;
	}
    }

    // update temperature, soundspeed and aspect ratio
    compute_temperature(data, true);
    compute_sound_speed(data, true);
    compute_scale_height(data, true);

    // calcuate Ka for K(i/2,j)
    for (unsigned int nr = 1; nr < Ka.get_size_radial() - 1; ++nr) {
	for (unsigned int naz = 0; naz < Ka.get_size_azimuthal(); ++naz) {
	    const unsigned int n_azimuthal_plus =
		(naz == Ka.get_max_azimuthal() ? 0 : naz + 1);
	    const unsigned int n_azimuthal_minus =
		(naz == 0 ? Ka.get_max_azimuthal() : naz - 1);

	    // average temperature radially
	    const double temperature =
		0.5 * (Temperature(nr - 1, naz) + Temperature(nr, naz));
	    const double density = 0.5 * (Sigma(nr - 1, naz) + Sigma(nr, naz));
	    const double scale_height =
		0.5 * (Scale_height(nr - 1, naz) + Scale_height(nr, naz));

	    const double temperatureCGS = temperature * units::temperature;
	    const double H = scale_height;
	    const double densityCGS =
		density / (parameters::density_factor * H) * units::density;

	    const double kappaCGS =
		opacity::opacity(densityCGS, temperatureCGS);
	    const double kappa = parameters::kappa_factor * kappaCGS *
				 units::opacity.get_inverse_cgs_factor();

	    const double denom = 1.0 / (density * kappa);

	    // Levermore & Pomraning 1981
	    // R = 4 |nabla T\/T * 1/(rho kappa)
	    const double dT_dr =
		(Temperature(nr, naz) - Temperature(nr - 1, naz)) *
		InvDiffRmed[nr];
	    const double dT_dphi =
		InvRinf[nr] *
		(0.5 * (Temperature(nr - 1, n_azimuthal_plus) +
			Temperature(nr, n_azimuthal_plus)) -
		 0.5 * (Temperature(nr - 1, n_azimuthal_minus) +
			Temperature(nr, n_azimuthal_minus))) /
		(2.0 * dphi);

	    const double nabla_T =
		std::sqrt(std::pow(dT_dr, 2) + std::pow(dT_dphi, 2));

	    const double R = 4.0 * nabla_T / temperature * denom * H *
			     parameters::density_factor;

	    const double lambda = flux_limiter(R);

	    Ka(nr, naz) = 8.0 * 4.0 * constants::sigma.get_code_value() *
			  lambda * H * H * std::pow(temperature, 3) * denom;
	}
    }

    for (unsigned int naz = 0; naz < Ka.get_size_azimuthal(); ++naz) {
	const unsigned int nr_max = Ka.get_max_radial();
	if (CPU_Rank == CPU_Highest &&
	    parameters::boundary_outer ==
		parameters::boundary_condition_reflecting) {
	    Ka(nr_max - 1, naz) = 0.0;
	}

	if (CPU_Rank == 0 && parameters::boundary_inner ==
				 parameters::boundary_condition_reflecting) {
	    Ka(1, naz) = 0.0;
	}
    }

    // Similar to Tobi's original implementation
    for (unsigned int naz = 0; naz < Ka.get_size_azimuthal(); ++naz) {
	const unsigned int nr_max = Ka.get_max_radial();
	if (CPU_Rank == CPU_Highest &&
	    !(parameters::boundary_outer ==
		  parameters::boundary_condition_reflecting ||
	      parameters::boundary_outer ==
		  parameters::boundary_condition_open)) {
	    Ka(nr_max - 1, naz) = Ka(nr_max - 2, naz);
	}

	if (CPU_Rank == 0 && !(parameters::boundary_inner ==
				   parameters::boundary_condition_reflecting ||
			       parameters::boundary_inner ==
				   parameters::boundary_condition_open)) {
	    Ka(1, naz) = Ka(2, naz);
	}
    }

    // calcuate Kb for K(i,j/2)
    for (unsigned int nr = 1; nr < Kb.get_size_radial() - 1; ++nr) {
	for (unsigned int naz = 0; naz < Kb.get_size_azimuthal(); ++naz) {
	    // unsigned int n_azimuthal_plus = (n_azimuthal ==
	    // Kb.get_max_azimuthal() ? 0 : n_azimuthal + 1);
	    const unsigned int naz_m =
		(naz == 0 ? Kb.get_max_azimuthal() : naz - 1);

	    // average temperature azimuthally
	    const double temperature =
		0.5 * (Temperature(nr, naz_m) + Temperature(nr, naz));
	    const double density = 0.5 * (Sigma(nr, naz_m) + Sigma(nr, naz));
	    const double scale_height =
		0.5 * (Scale_height(nr, naz_m) + Scale_height(nr, naz));

	    const double temperatureCGS = temperature * units::temperature;
	    const double H = scale_height;
	    const double densityCGS =
		density / (parameters::density_factor * H) * units::density;

	    const double kappaCGS =
		opacity::opacity(densityCGS, temperatureCGS);
	    const double kappa = parameters::kappa_factor * kappaCGS *
				 units::opacity.get_inverse_cgs_factor();

	    const double denom = 1.0 / (density * kappa);

	    // Levermore & Pomraning 1981
	    // R = 4 |nabla T\/T * 1/(rho kappa)
	    const double dT_dr =
		(0.5 * (Temperature(nr - 1, naz_m) + Temperature(nr - 1, naz)) -
		 0.5 *
		     (Temperature(nr + 1, naz_m) + Temperature(nr + 1, naz))) /
		(Ra[nr - 1] - Ra[nr + 1]);
	    const double dT_dphi =
		InvRmed[nr] * (Temperature(nr, naz) - Temperature(nr, naz_m)) /
		dphi;

	    const double nabla_T =
		std::sqrt(std::pow(dT_dr, 2) + std::pow(dT_dphi, 2));

	    const double R = 4.0 * nabla_T / temperature * denom * H *
			     parameters::density_factor;

	    const double lambda = flux_limiter(R);
	    /*if (n_radial == 4) {
		    printf("kb:
	    phi=%lg\tR=%lg\tlambda=%lg\tdphi=%lg\tdr=%lg\tnabla=%lg\tT=%lg\tH=%lg\n",
	    dphi*n_azimuthal, R, lambda,dT_dphi,dT_dr,nabla_T,temperature,H);
	    }*/

	    Kb(nr, naz) = 8.0 * 4.0 * constants::sigma.get_code_value() *
			  lambda * H * H * std::pow(temperature, 3) * denom;
	    // Kb(n_radial, n_azimuthal)
	    // = 16.0*parameters::density_factor*constants::sigma.get_code_value()*lambda*H*pow3(temperature)*denom;
	}
    }

    const double c_v = constants::R / (parameters::MU * (parameters::ADIABATICINDEX - 1.0));

    // calculate A,B,C,D,E
    for (unsigned int nr = 1; nr < Temperature.get_size_radial() - 1; ++nr) {
	for (unsigned int naz = 0; naz < Temperature.get_size_azimuthal();
	     ++naz) {
	    const double Sig = Sigma(nr, naz);
	    const double common_factor =
		-dt * parameters::density_factor / (Sig * c_v);

	    // 2/(dR^2)
	    const double common_AC =
		common_factor * 2.0 /
		(std::pow(Ra[nr + 1], 2) - std::pow(Ra[nr], 2));
	    A(nr, naz) = common_AC * Ka(nr, naz) * Ra[nr] * InvDiffRmed[nr];
	    C(nr, naz) =
		common_AC * Ka(nr + 1, naz) * Ra[nr + 1] * InvDiffRmed[nr + 1];

	    // 1/(r^2 dphi^2)
	    const double common_DE =
		common_factor / (std::pow(Rb[nr], 2) * std::pow(dphi, 2));
	    D(nr, naz) = common_DE * Kb(nr, naz);
	    E(nr, naz) =
		common_DE * Kb(nr, naz == Kb.get_max_azimuthal() ? 0 : naz + 1);

	    B(nr, naz) =
		-A(nr, naz) - C(nr, naz) - D(nr, naz) - E(nr, naz) + 1.0;

	    Told(nr, naz) = Temperature(nr, naz);

	    /*double energy_change = dt*data[t_data::QPLUS](n_radial,
	    n_azimuthal)
		- dt*data[t_data::QMINUS](n_radial, n_azimuthal)
		- dt*data[t_data::P_DIVV](n_radial, n_azimuthal);

	    double temperature_change =
		MU/R*(parameters::ADIABATICINDEX-1.0)*energy_change/Sigma(n_radial,n_azimuthal);
	    Told(n_radial, n_azimuthal) += temperature_change;

	    if (Told(n_radial, n_azimuthal) <
	    parameters::minimum_temperature*units::temperature.get_inverse_cgs_factor())
		{ Temperature(n_radial, n_azimuthal) =
	    parameters::minimum_temperature*units::temperature.get_inverse_cgs_factor();
	    }
	    */
	}
    }

    static unsigned int old_iterations =
	parameters::radiative_diffusion_max_iterations;
    static int direction = 1;
    static double omega = parameters::radiative_diffusion_omega;

    unsigned int iterations = 0;
    double absolute_norm = DBL_MAX;
    double norm_change = DBL_MAX;

    const int l = CPUOVERLAP * NAzimuthal;
    const int oo = (Temperature.Nrad - CPUOVERLAP) * NAzimuthal;
    const int o = (Temperature.Nrad - 2 * CPUOVERLAP) * NAzimuthal;

    // do SOR
    while ((norm_change > 1e-12) &&
	   (parameters::radiative_diffusion_max_iterations > iterations)) {
	// if ((CPU_Rank == CPU_Highest) && parameters::boundary_outer ==
	// parameters::boundary_condition_open) {
	// 	// set temperature to T_min in outermost cells
	// 	for (unsigned int n_azimuthal = 0; n_azimuthal <=
	// Temperature.get_max_azimuthal(); ++n_azimuthal) {
	// 		Temperature(Temperature.get_max_radial(),
	// n_azimuthal) =
	// parameters::minimum_temperature*units::temperature.get_inverse_cgs_factor();
	// 	}
	// }

	// if ((CPU_Rank == 0) && parameters::boundary_inner ==
	// parameters::boundary_condition_open) {
	// 	// set temperature to T_min in innermost cells
	// 	for (unsigned int n_azimuthal = 0; n_azimuthal <=
	// Temperature.get_max_azimuthal(); ++n_azimuthal) {
	// 		Temperature(0, n_azimuthal) =
	// parameters::minimum_temperature*units::temperature.get_inverse_cgs_factor();
	// 	}
	// }
	boundary_conditions::apply_boundary_condition(data, 0.0, false);

	norm_change = absolute_norm;
	absolute_norm = 0.0;

	for (unsigned int nr = 1; nr < Temperature.get_size_radial() - 1;
	     ++nr) {
	    for (unsigned int naz = 0; naz < Temperature.get_size_azimuthal();
		 ++naz) {
		const double old_value = Temperature(nr, naz);
		const unsigned int naz_p =
		    (naz == Temperature.get_max_azimuthal() ? 0 : naz + 1);
		const unsigned int naz_m =
		    (naz == 0 ? Temperature.get_max_azimuthal() : naz - 1);

		Temperature(nr, naz) =
		    (1.0 - omega) * Temperature(nr, naz) -
		    omega / B(nr, naz) *
			(A(nr, naz) * Temperature(nr - 1, naz) +
			 C(nr, naz) * Temperature(nr + 1, naz) +
			 D(nr, naz) * Temperature(nr, naz_m) +
			 E(nr, naz) * Temperature(nr, naz_p) - Told(nr, naz));

		// only non ghostcells to norm and don't count overlap cell's
		// twice
		const bool isnot_ghostcell_rank_0 =
		    nr > ((CPU_Rank == 0) ? GHOSTCELLS_B : CPUOVERLAP);
		const bool isnot_ghostcell_rank_highest =
		    (nr <
		     (Temperature.get_max_radial() -
		      ((CPU_Rank == CPU_Highest) ? GHOSTCELLS_B : CPUOVERLAP)));

		if (isnot_ghostcell_rank_0 && isnot_ghostcell_rank_highest) {
		    absolute_norm +=
			std::pow(old_value - Temperature(nr, naz), 2);
		}
	    }
	}

	double tmp = absolute_norm;
	MPI_Allreduce(&tmp, &absolute_norm, 1, MPI_DOUBLE, MPI_SUM,
		      MPI_COMM_WORLD);
	absolute_norm = std::sqrt(absolute_norm) / (GlobalNRadial * NAzimuthal);

	norm_change = fabs(absolute_norm - norm_change);
	iterations++;

	// communicate with other nodes
	memcpy(SendInnerBoundary, Temperature.Field + l, l * sizeof(double));
	memcpy(SendOuterBoundary, Temperature.Field + o, l * sizeof(double));

	MPI_Request req1, req2, req3, req4;

	if (CPU_Rank % 2 == 0) {
	    if (CPU_Rank != 0) {
		MPI_Isend(SendInnerBoundary, NAzimuthal * CPUOVERLAP,
			  MPI_DOUBLE, CPU_Prev, 0, MPI_COMM_WORLD, &req1);
		MPI_Irecv(RecvInnerBoundary, NAzimuthal * CPUOVERLAP,
			  MPI_DOUBLE, CPU_Prev, 0, MPI_COMM_WORLD, &req2);
	    }
	    if (CPU_Rank != CPU_Highest) {
		MPI_Isend(SendOuterBoundary, NAzimuthal * CPUOVERLAP,
			  MPI_DOUBLE, CPU_Next, 0, MPI_COMM_WORLD, &req3);
		MPI_Irecv(RecvOuterBoundary, NAzimuthal * CPUOVERLAP,
			  MPI_DOUBLE, CPU_Next, 0, MPI_COMM_WORLD, &req4);
	    }
	} else {
	    if (CPU_Rank != CPU_Highest) {
		MPI_Irecv(RecvOuterBoundary, NAzimuthal * CPUOVERLAP,
			  MPI_DOUBLE, CPU_Next, 0, MPI_COMM_WORLD, &req3);
		MPI_Isend(SendOuterBoundary, NAzimuthal * CPUOVERLAP,
			  MPI_DOUBLE, CPU_Next, 0, MPI_COMM_WORLD, &req4);
	    }
	    if (CPU_Rank != 0) {
		MPI_Irecv(RecvInnerBoundary, NAzimuthal * CPUOVERLAP,
			  MPI_DOUBLE, CPU_Prev, 0, MPI_COMM_WORLD, &req1);
		MPI_Isend(SendInnerBoundary, NAzimuthal * CPUOVERLAP,
			  MPI_DOUBLE, CPU_Prev, 0, MPI_COMM_WORLD, &req2);
	    }
	}

	if (CPU_Rank != 0) {
	    MPI_Wait(&req1, &global_MPI_Status);
	    MPI_Wait(&req2, &global_MPI_Status);
	    memcpy(Temperature.Field, RecvInnerBoundary, l * sizeof(double));
	}

	if (CPU_Rank != CPU_Highest) {
	    MPI_Wait(&req3, &global_MPI_Status);
	    MPI_Wait(&req4, &global_MPI_Status);
	    memcpy(Temperature.Field + oo, RecvOuterBoundary,
		   l * sizeof(double));
	}
    }

    if (iterations == parameters::radiative_diffusion_max_iterations) {
	logging::print_master(
	    LOG_WARNING
	    "Maximum iterations (%u) reached in radiative_diffusion (omega = %lg). Norm is %lg with a last change of %lg.\n",
	    parameters::radiative_diffusion_max_iterations, omega,
	    absolute_norm, norm_change);
    }

    // adapt omega
    if (old_iterations < iterations) {
	direction *= -1;
    }

    if (parameters::radiative_diffusion_omega_auto_enabled) {
	omega += direction * 0.01;
    }

    if (omega >= 2.0) {
	omega = 1.99;
	direction = -1;
    }

    if (omega <= 1.0) {
	omega = 1.0;
	direction = 1;
    }

    old_iterations = iterations;

    logging::print_master(LOG_VERBOSE "%u iterations, omega=%lf\n", iterations,
			  omega);

    // compute energy from temperature
    for (unsigned int n_radial = 1; n_radial < Energy.get_size_radial() - 1;
	 ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal < Energy.get_size_azimuthal(); ++n_azimuthal) {
	    Energy(n_radial, n_azimuthal) = Temperature(n_radial, n_azimuthal) *
					    Sigma(n_radial, n_azimuthal) /
					    (parameters::ADIABATICINDEX - 1.0) /
					    parameters::MU * constants::R;
	}
    }

	SetTemperatureFloorCeilValues(data, __FILE__, __LINE__);
}


static void compute_sound_speed_normal(t_data &data, bool force_update)
{
    static double last_physicaltime_calculated = -1;

    if ((!force_update) && (last_physicaltime_calculated == PhysicalTime)) {
	return;
    }
    last_physicaltime_calculated = PhysicalTime;

    for (unsigned int n_radial = 0;
	 n_radial <= data[t_data::SOUNDSPEED].get_max_radial(); ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= data[t_data::SOUNDSPEED].get_max_azimuthal();
	     ++n_azimuthal) {
	    if (parameters::Adiabatic) {
		const double gamma_eff =
		    pvte::get_gamma_eff(data, n_radial, n_azimuthal);
		const double gamma1 =
		    pvte::get_gamma1(data, n_radial, n_azimuthal);

		data[t_data::SOUNDSPEED](n_radial, n_azimuthal) =
		    std::sqrt(gamma1 * (gamma_eff - 1.0) *
			      data[t_data::ENERGY](n_radial, n_azimuthal) /
			      data[t_data::SIGMA](n_radial, n_azimuthal));

	    } else if (parameters::Polytropic) {
		const double gamma_eff =
		    pvte::get_gamma_eff(data, n_radial, n_azimuthal);
		data[t_data::SOUNDSPEED](n_radial, n_azimuthal) =
		    std::sqrt(gamma_eff * constants::R / parameters::MU *
			      data[t_data::TEMPERATURE](n_radial, n_azimuthal));
	    } else { // isothermal
		// This follows from: cs/v_Kepler = H/r
		data[t_data::SOUNDSPEED](n_radial, n_azimuthal) =
		    parameters::ASPECTRATIO_REF *
		    std::sqrt(constants::G * hydro_center_mass / Rb[n_radial]) *
		    std::pow(Rb[n_radial], parameters::FLARINGINDEX);
	    }
	}
    }
}

static void compute_iso_sound_speed_center_of_mass(t_data &data,
						   const bool force_update)
{
    static double last_physicaltime_calculated = -1;

    if ((!force_update) && (last_physicaltime_calculated == PhysicalTime)) {
	return;
    }
    last_physicaltime_calculated = PhysicalTime;

    const Pair r_cm = data.get_planetary_system().get_center_of_mass();
    const double m_cm = data.get_planetary_system().get_mass();

    // Cs^2 = h^2 * vk * r ^ 2*Flaring
    for (unsigned int n_rad = 0;
	 n_rad <= data[t_data::SOUNDSPEED].get_max_radial(); ++n_rad) {
	for (unsigned int n_az = 0;
	     n_az <= data[t_data::SOUNDSPEED].get_max_azimuthal(); ++n_az) {

	    const int cell = get_cell_id(n_rad, n_az);
	    const double x = CellCenterX->Field[cell];
	    const double y = CellCenterY->Field[cell];

	    /// since the mass is distributed homogeniously distributed
	    /// inside the cell, we assume that the planet is always at
	    /// least cell_size / 2 plus planet radius away from the gas
	    /// this is an rough estimate without explanation
	    /// alternatively you can think about it yourself
	    const double min_dist =
		0.5 * std::max(Rsup[n_rad] - Rinf[n_rad], Rmed[n_rad] * dphi);

	    const double dx = x - r_cm.x;
	    const double dy = y - r_cm.y;

	    const double dist = std::max(
		std::sqrt(std::pow(dx, 2) + std::pow(dy, 2)), min_dist);

	    const double Cs2 = constants::G * m_cm / dist;

	    const double Cs =
		parameters::ASPECTRATIO_REF * std::pow(dist, parameters::FLARINGINDEX) * std::sqrt(Cs2);
	    data[t_data::SOUNDSPEED](n_rad, n_az) = Cs;
	}
    }
}

static void compute_iso_sound_speed_nbody(t_data &data, const bool force_update)
{

    static double last_physicaltime_calculated = -1;

    if ((!force_update) && (last_physicaltime_calculated == PhysicalTime)) {
	return;
    }
    last_physicaltime_calculated = PhysicalTime;

    static const unsigned int N_planets =
	data.get_planetary_system().get_number_of_planets();
    static std::vector<double> xpl(N_planets);
    static std::vector<double> ypl(N_planets);
    static std::vector<double> mpl(N_planets);
    static std::vector<double> rpl(N_planets);

    // setup planet data
    for (unsigned int k = 0; k < N_planets; k++) {
	t_planet &planet = data.get_planetary_system().get_planet(k);
	mpl[k] = planet.get_rampup_mass();
	xpl[k] = planet.get_x();
	ypl[k] = planet.get_y();
	rpl[k] = planet.get_planet_radial_extend();
    }

    assert(N_planets > 1);

    // Cs^2 = h^2 * vk * r ^ 2*Flaring
    for (unsigned int n_rad = 0;
	 n_rad <= data[t_data::SOUNDSPEED].get_max_radial(); ++n_rad) {
	for (unsigned int n_az = 0;
	     n_az <= data[t_data::SOUNDSPEED].get_max_azimuthal(); ++n_az) {

	    const int cell = get_cell_id(n_rad, n_az);
	    const double x = CellCenterX->Field[cell];
	    const double y = CellCenterY->Field[cell];

		double Cs2 = 0.0;

		for (unsigned int k = 0; k < N_planets; k++) {

			/// since the mass is distributed homogeniously distributed
			/// inside the cell, we assume that the planet is always at
			/// least cell_size / 2 plus planet radius away from the gas
			/// this is an rough estimate without explanation
			/// alternatively you can think about it yourself
			const double min_dist =
				0.5 * std::max(Rsup[n_rad] - Rinf[n_rad],
					   Rmed[n_rad] * dphi) +
				rpl[k];

			const double dx = x - xpl[k];
			const double dy = y - ypl[k];

			const double dist = std::max(
				std::sqrt(std::pow(dx, 2) + std::pow(dy, 2)), min_dist);

		Cs2 += (parameters::ASPECTRATIO_REF * parameters::ASPECTRATIO_REF * std::pow(dist, 2.0*parameters::FLARINGINDEX)
				* constants::G * mpl[k]) / dist;
	    }

		const double Cs = std::sqrt(Cs2);
	    data[t_data::SOUNDSPEED](n_rad, n_az) = Cs;
	}
    }
}

void compute_sound_speed(t_data &data, bool force_update)
{
    if (parameters::Adiabatic || parameters::Polytropic) {
	compute_sound_speed_normal(data, force_update);
    }

    if (parameters::Locally_Isothermal) {
	switch (parameters::ASPECTRATIO_MODE) {
	case 0:
	    compute_sound_speed_normal(data, force_update);
	    break;
	case 1:
	    compute_iso_sound_speed_nbody(data,
					  force_update); // has discontinuities
	    break;
	case 2:
	    compute_iso_sound_speed_center_of_mass(data, force_update);
	    break;
	default:
	    compute_sound_speed_normal(data, force_update);
	}
    }
}

/**
	computes aspect ratio
*/
void compute_scale_height_old(t_data &data, const bool force_update)
{
    static double last_physicaltime_calculated = -1;

    if ((!force_update) && (last_physicaltime_calculated == PhysicalTime)) {
	return;
    }
    last_physicaltime_calculated = PhysicalTime;

    for (unsigned int n_radial = 0;
	 n_radial <= data[t_data::SCALE_HEIGHT].get_max_radial(); ++n_radial) {
	double inv_omega_kepler = 1.0 / calculate_omega_kepler(Rb[n_radial]);

	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= data[t_data::SCALE_HEIGHT].get_max_azimuthal();
	     ++n_azimuthal) {
	    if (parameters::Adiabatic || parameters::Polytropic) {
		// h = H/r = c_s,iso / v_k = c_s/sqrt(gamma) / v_k
		// H = h*r = c_s,iso / W_k = c_s/sqrt(gamma) / W_k
		const double gamma1 =
		    pvte::get_gamma1(data, n_radial, n_azimuthal);
		data[t_data::SCALE_HEIGHT](n_radial, n_azimuthal) =
		    data[t_data::SOUNDSPEED](n_radial, n_azimuthal) /
		    (std::sqrt(gamma1)) * inv_omega_kepler;
	    } else {
		// h = H/r = c_s/v_k
		// H = h*r = c_s/v_k * R = c_s / W_k
		data[t_data::SCALE_HEIGHT](n_radial, n_azimuthal) =
		    data[t_data::SOUNDSPEED](n_radial, n_azimuthal) *
		    inv_omega_kepler;
	    }
		if(parameters::heating_star_enabled){
			const double h = data[t_data::SCALE_HEIGHT](n_radial, n_azimuthal) / Rb[n_radial];
		data[t_data::ASPECTRATIO](n_radial, n_azimuthal) = h;
		}
	}
    }
}

/**
	computes aspect ratio for an entire Nbody system
*/
void compute_scale_height_nbody(t_data &data, const bool force_update)
{
    static double last_physicaltime_calculated = -1;

    if ((!force_update) && (last_physicaltime_calculated == PhysicalTime)) {
	return;
    }
    last_physicaltime_calculated = PhysicalTime;

    static const unsigned int N_planets =
	data.get_planetary_system().get_number_of_planets();
    static std::vector<double> xpl(N_planets);
    static std::vector<double> ypl(N_planets);
    static std::vector<double> mpl(N_planets);
    static std::vector<double> rpl(N_planets);

    // setup planet data
    for (unsigned int k = 0; k < N_planets; k++) {
	const t_planet &planet = data.get_planetary_system().get_planet(k);
	mpl[k] = planet.get_rampup_mass();
	xpl[k] = planet.get_x();
	ypl[k] = planet.get_y();
	rpl[k] = planet.get_planet_radial_extend();
    }

    // h = H/r
    // H = = c_s,iso / (GM/r^3) = c_s/sqrt(gamma) / / (GM/r^3)
    // for an Nbody system, H^-2 = sum_n (H_n)^-2
    // See Günter & Kley 2003 Eq. 8, but beware of wrong extra square.
    // Better see Thun et al. 2017 Eq. 8 instead.
    for (unsigned int n_rad = 0;
	 n_rad <= data[t_data::SCALE_HEIGHT].get_max_radial(); ++n_rad) {
	for (unsigned int n_az = 0;
	     n_az <= data[t_data::SCALE_HEIGHT].get_max_azimuthal(); ++n_az) {

	    const int cell = get_cell_id(n_rad, n_az);
	    const double x = CellCenterX->Field[cell];
	    const double y = CellCenterY->Field[cell];
	    const double cs2 =
		std::pow(data[t_data::SOUNDSPEED](n_rad, n_az), 2);

		double inv_H2 = 0.0; // inverse scale height squared
		double inv_h2 = 0.0; // inverse aspectratio squared

	    for (unsigned int k = 0; k < N_planets; k++) {

		/// since the mass is distributed homogeniously distributed
		/// inside the cell, we assume that the planet is always at
		/// least cell_size / 2 plus planet radius away from the gas
		/// this is an rough estimate without explanation
		/// alternatively you can think about it yourself
		const double min_dist =
		    0.5 * std::max(Rsup[n_rad] - Rinf[n_rad],
				   Rmed[n_rad] * dphi) +
		    rpl[k];

		const double dx = x - xpl[k];
		const double dy = y - ypl[k];

		const double dist = std::max(
			std::sqrt(std::pow(dx, 2) + std::pow(dy, 2)), min_dist);
		const double dist3 = std::pow(dist, 3);

		// H^2 = (GM / dist^3 / Cs_iso^2)^-1
		if (parameters::Adiabatic || parameters::Polytropic) {
		    const double gamma1 = pvte::get_gamma1(data, n_rad, n_az);
			const double tmp_inv_H2 =
			constants::G * mpl[k] * gamma1 / (dist3 * cs2);
		    inv_H2 += tmp_inv_H2;

			if(parameters::heating_star_enabled){
			const double tmp_inv_h2 =
			constants::G * mpl[k] * gamma1 / (dist * cs2);
			inv_h2 += tmp_inv_h2;
			}

		} else {
		    const double tmp_inv_H2 =
			constants::G * mpl[k] / (dist3 * cs2);
		    inv_H2 += tmp_inv_H2;

			if(parameters::heating_star_enabled){
			const double tmp_inv_h2 =
			constants::G * mpl[k] / (dist * cs2);
			inv_h2 += tmp_inv_h2;
			}
		}
	    }

	    const double H = std::sqrt(1.0 / inv_H2);
	    data[t_data::SCALE_HEIGHT](n_rad, n_az) = H;

		if(parameters::heating_star_enabled){
			const double h = std::sqrt(1.0 / inv_h2);
			data[t_data::ASPECTRATIO](n_rad, n_az) = h;
		}
	}
    }
}

/**
	computes aspect ratio with respect to the center of mass
*/
void compute_scale_height_center_of_mass(t_data &data, const bool force_update)
{
    static double last_physicaltime_calculated = -1;

    if ((!force_update) && (last_physicaltime_calculated == PhysicalTime)) {
	return;
    }
    last_physicaltime_calculated = PhysicalTime;

    const Pair r_cm = data.get_planetary_system().get_center_of_mass();
    const double m_cm = data.get_planetary_system().get_mass();

    for (unsigned int n_rad = 0;
	 n_rad <= data[t_data::SCALE_HEIGHT].get_max_radial(); ++n_rad) {
	for (unsigned int n_az = 0;
	     n_az <= data[t_data::SCALE_HEIGHT].get_max_azimuthal(); ++n_az) {

	    const int cell = get_cell_id(n_rad, n_az);
	    const double x = CellCenterX->Field[cell];
	    const double y = CellCenterY->Field[cell];
		const double cs = data[t_data::SOUNDSPEED](n_rad, n_az);

	    // const double min_dist =
	    //	0.5 * std::max(Rsup[n_rad] - Rinf[n_rad],
	    //		   Rmed[n_rad] * dphi);

	    const double dx = x - r_cm.x;
	    const double dy = y - r_cm.y;

	    // const double dist = std::max(
	    //	std::sqrt(std::pow(dx, 2) + std::pow(dy, 2)), min_dist);
	    const double dist = std::sqrt(std::pow(dx, 2) + std::pow(dy, 2));

		// h^2 = Cs_iso / vk = (Cs_iso^2 / (GM / dist))
		// H^2 = Cs_iso / Omegak = (Cs_iso^2 / (GM / dist^3))
		// H = h * dist
	    if (parameters::Adiabatic || parameters::Polytropic) {
		/// Convert sound speed to isothermal sound speed cs,iso = cs /
		/// sqrt(gamma)
		const double gamma1 = pvte::get_gamma1(data, n_rad, n_az);
		const double h = cs * std::sqrt(dist / (constants::G * m_cm * gamma1));

		if(parameters::heating_star_enabled){
		data[t_data::ASPECTRATIO](n_rad, n_az) = h;
		}
		const double H = dist * h;
		data[t_data::SCALE_HEIGHT](n_rad, n_az) = H;

	    } else { // locally isothermal
		const double h = cs * std::sqrt(dist / (constants::G * m_cm));
		if(parameters::heating_star_enabled){
		data[t_data::ASPECTRATIO](n_rad, n_az) = h;
		}
		const double H = dist * h;
		data[t_data::SCALE_HEIGHT](n_rad, n_az) = H;
	    }
	}
    }
}

void compute_scale_height(t_data &data, const bool force_update)
{
    switch (parameters::ASPECTRATIO_MODE) {
    case 0:
	compute_scale_height_old(data, force_update);
	break;
    case 1:
	compute_scale_height_nbody(data, force_update);
	break;
    case 2:
	compute_scale_height_center_of_mass(data, force_update);
	break;
    default:
	compute_scale_height_old(data, force_update);
    }
}

/**
	computes pressure
*/
void compute_pressure(t_data &data, bool force_update)
{
    static double last_physicaltime_calculated = -1;

    if ((!force_update) && (last_physicaltime_calculated == PhysicalTime)) {
	return;
    }
    last_physicaltime_calculated = PhysicalTime;

    for (unsigned int n_radial = 0;
	 n_radial <= data[t_data::PRESSURE].get_max_radial(); ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= data[t_data::PRESSURE].get_max_azimuthal();
	     ++n_azimuthal) {
	    if (parameters::Adiabatic) {
		const double gamma_eff =
		    pvte::get_gamma_eff(data, n_radial, n_azimuthal);
		data[t_data::PRESSURE](n_radial, n_azimuthal) =
		    (gamma_eff - 1.0) *
		    data[t_data::ENERGY](n_radial, n_azimuthal);
	    } else if (parameters::Polytropic) {
		data[t_data::PRESSURE](n_radial, n_azimuthal) =
		    data[t_data::SIGMA](n_radial, n_azimuthal) *
		    std::pow(data[t_data::SOUNDSPEED](n_radial, n_azimuthal),
			     2) /
		    parameters::ADIABATICINDEX;
	    } else { // Isothermal
		// since SoundSpeed is not update from initialization, cs
		// remains axisymmetric
		data[t_data::PRESSURE](n_radial, n_azimuthal) =
		    data[t_data::SIGMA](n_radial, n_azimuthal) *
		    std::pow(data[t_data::SOUNDSPEED](n_radial, n_azimuthal),
			     2);
	    }
	}
    }
}

/**
	computes temperature
*/
void compute_temperature(t_data &data, bool force_update)
{
    static double last_physicaltime_calculated = -1;

    if ((!force_update) && (last_physicaltime_calculated == PhysicalTime)) {
	return;
    }
    last_physicaltime_calculated = PhysicalTime;

    for (unsigned int n_radial = 0;
	 n_radial <= data[t_data::TEMPERATURE].get_max_radial(); ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= data[t_data::TEMPERATURE].get_max_azimuthal();
	     ++n_azimuthal) {
	    if (parameters::Adiabatic) {
		const double mu = pvte::get_mu(data, n_radial, n_azimuthal);
		const double gamma_eff =
		    pvte::get_gamma_eff(data, n_radial, n_azimuthal);

		data[t_data::TEMPERATURE](n_radial, n_azimuthal) =
		    mu / constants::R * (gamma_eff - 1.0) *
		    data[t_data::ENERGY](n_radial, n_azimuthal) /
		    data[t_data::SIGMA](n_radial, n_azimuthal);
	    } else if (parameters::Polytropic) {
		const double mu = pvte::get_mu(data, n_radial, n_azimuthal);
		const double gamma_eff =
		    pvte::get_gamma_eff(data, n_radial, n_azimuthal);
		data[t_data::TEMPERATURE](n_radial, n_azimuthal) =
		    mu / constants::R * parameters::POLYTROPIC_CONSTANT *
		    std::pow(data[t_data::SIGMA](n_radial, n_azimuthal),
			     gamma_eff - 1.0);
	    } else { // Isothermal
		data[t_data::TEMPERATURE](n_radial, n_azimuthal) =
		    parameters::MU / constants::R *
		    data[t_data::PRESSURE](n_radial, n_azimuthal) /
		    data[t_data::SIGMA](n_radial, n_azimuthal);
	    }
	}
    }
}

/**
	computes density rho
*/
void compute_rho(t_data &data, bool force_update)
{
    static double last_physicaltime_calculated = -1;

    if ((!force_update) && (last_physicaltime_calculated == PhysicalTime)) {
	return;
    }
    last_physicaltime_calculated = PhysicalTime;

    compute_scale_height(data, force_update);

    for (unsigned int n_radial = 0;
	 n_radial <= data[t_data::RHO].get_max_radial(); ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= data[t_data::RHO].get_max_azimuthal();
	     ++n_azimuthal) {
	    const double H = data[t_data::SCALE_HEIGHT](n_radial, n_azimuthal);
	    data[t_data::RHO](n_radial, n_azimuthal) =
		data[t_data::SIGMA](n_radial, n_azimuthal) /
		(parameters::density_factor * H);
	}
    }
}

void compute_heating_cooling_for_CFL(t_data &data)
{
    if (parameters::Adiabatic) {

	viscosity::update_viscosity(data);
	ComputeViscousStressTensor(data);

	calculate_qminus(data); // first to calculate teff
	calculate_qplus(data);

	for (unsigned int n_radial = 1;
	     n_radial < data[t_data::ENERGY].get_max_radial(); ++n_radial) {
	    for (unsigned int n_azimuthal = 0;
		 n_azimuthal <= data[t_data::ENERGY].get_max_azimuthal();
		 ++n_azimuthal) {

		const double sigma_sb = constants::sigma;
		const double c = constants::c;
		const double mu = pvte::get_mu(data, n_radial, n_azimuthal);
		const double gamma =
		    pvte::get_gamma_eff(data, n_radial, n_azimuthal);
		const double Rgas = constants::R;

		const double H =
		    data[t_data::SCALE_HEIGHT](n_radial, n_azimuthal);

		const double sigma = data[t_data::SIGMA](n_radial, n_azimuthal);
		const double energy =
		    data[t_data::ENERGY](n_radial, n_azimuthal);

		const double inv_pow4 =
		    std::pow(mu * (gamma - 1.0) / (Rgas * sigma), 4);
		double alpha = 1.0 + 2.0 * H * 4.0 * sigma_sb / c * inv_pow4 *
					 std::pow(energy, 3);

		data[t_data::QPLUS](n_radial, n_azimuthal) /= alpha;
		data[t_data::QMINUS](n_radial, n_azimuthal) /= alpha;
	    }
	}
    }
}
