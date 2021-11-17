/**
	\file SourceEuler.c

	Contains routines used by the hydrodynamical loop. More specifically, it
   contains the main loop itself and all the source term substeps (with the
   exception of the evaluation of the viscous force). The transport substep is
   treated elsewhere.
*/

#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
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
#include "particles.h"
#include "quantities.h"
#include "selfgravity.h"
#include "stress.h"
#include "sts.h"
#include "units.h"
#include "util.h"
#include "viscosity.h"

#include <cstring>
extern boolean Corotating;

extern boolean FastTransport;
Pair IndirectTerm;
Pair IndirectTermDisk;
Pair IndirectTermPlanets;
double dtemp;

/**
	Checks polargrid for negative entries.

	\param array polargrid to check
	\returns >0 if there are negative entries, 0 otherwise
*/
static int DetectCrash(t_polargrid *array)
{
    unsigned int result = 0;

    for (unsigned int n_radial = 0; n_radial < array->Nrad; ++n_radial) {
	for (unsigned int n_azimuthal = 0; n_azimuthal < array->Nsec;
	     ++n_azimuthal) {
	    if ((*array)(n_radial, n_azimuthal) < 0.0) {
		logging::print(LOG_WARNING "%s negative in cell: (%u,%u)=%g\n",
			       array->get_name(), n_radial, n_azimuthal,
			       (*array)(n_radial, n_azimuthal));
		result += 1;
	    }
	}
    }

    return result;
}

static void HandleCrash(t_data &data)
{
    if (DetectCrash(&data[t_data::DENSITY])) {
	logging::print(LOG_ERROR "DetectCrash: Density < 0\n");
	PersonalExit(1);
    }

    if (parameters::Adiabatic) {
	if (DetectCrash(&data[t_data::ENERGY])) {
	    logging::print(LOG_ERROR "DetectCrash: Energy < 0\n");
	    PersonalExit(1);
	}
    }
}

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
    if (assure_minimum_temperature(
	    data[t_data::ENERGY], data[t_data::DENSITY],
	    parameters::minimum_temperature *
		units::temperature.get_inverse_cgs_factor())) {
	logging::print(LOG_DEBUG "Found temperature < %g %s in %s: %d.\n",
		       parameters::minimum_temperature,
		       units::temperature.get_cgs_symbol(), filename.c_str(),
		       line);
    }

    if (assure_maximum_temperature(
	    data[t_data::ENERGY], data[t_data::DENSITY],
	    parameters::maximum_temperature *
		units::temperature.get_inverse_cgs_factor())) {
	logging::print(LOG_DEBUG "Found temperature < %g %s in %s: %d.\n",
		       parameters::maximum_temperature,
		       units::temperature.get_cgs_symbol(), filename.c_str(),
		       line);
    }
}

static void CalculateMonitorQuantitiesAfterHydroStep(t_data &data,
						     int nTimeStep, double dt)
{
    // mdcp = CircumPlanetaryMass(data);
    // exces_mdcp = mdcp - mdcp0;

    if (data[t_data::ADVECTION_TORQUE].get_write()) {
	gas_torques::calculate_advection_torque(data, dt / DT);
    }
    if (data[t_data::VISCOUS_TORQUE].get_write()) {
	gas_torques::calculate_viscous_torque(data, dt / DT);
    }
    if (data[t_data::GRAVITATIONAL_TORQUE_NOT_INTEGRATED].get_write()) {
	gas_torques::calculate_gravitational_torque(data, dt / DT);
    }

    if (data[t_data::ALPHA_GRAV_MEAN].get_write()) {
	quantities::calculate_alpha_grav_mean_sumup(data, nTimeStep, dt / DT);
    }
    if (data[t_data::ALPHA_REYNOLDS_MEAN].get_write()) {
	quantities::calculate_alpha_reynolds_mean_sumup(data, nTimeStep,
							dt / DT);
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
    bool is_dens = strcmp(dst.get_name(), "dens") == 0;

    for (unsigned int n_radial = 0; n_radial <= dst.get_max_radial();
	 ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= dst.get_max_azimuthal(); ++n_azimuthal) {
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

/**
	Assures a miminum temperature in each cell.

	\param energy energy polar grid
	\param minimum_value minimum temperature
*/
bool assure_minimum_temperature(t_polargrid &energy, t_polargrid &density,
				double minimum_value)
{
    bool found = false;

    for (unsigned int n_radial = 0; n_radial <= energy.get_max_radial();
	 ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= energy.get_max_azimuthal(); ++n_azimuthal) {
	    if (energy(n_radial, n_azimuthal) <
		minimum_value * density(n_radial, n_azimuthal) /
		    parameters::MU * constants::R / (ADIABATICINDEX - 1.0)) {
#ifndef NDEBUG
		logging::print(
		    LOG_DEBUG "assure_minimum_temperature: (%u,%u)=%g<%g\n",
		    n_radial, n_azimuthal,
		    energy(n_radial, n_azimuthal) *
			units::temperature.get_cgs_factor() /
			density(n_radial, n_azimuthal) * parameters::MU /
			constants::R * (ADIABATICINDEX - 1.0),
		    minimum_value * units::temperature.get_cgs_factor(),
		    minimum_value);
#endif
		energy(n_radial, n_azimuthal) =
		    minimum_value * density(n_radial, n_azimuthal) /
		    parameters::MU * constants::R / (ADIABATICINDEX - 1.0);
		found = true;
	    }
	}
    }

    return found;
}

/**
	Assures a miminum temperature in each cell.

	\param energy energy polar grid
	\param minimum_value minimum temperature
*/
bool assure_maximum_temperature(t_polargrid &energy, t_polargrid &density,
				double maximum_value)
{
    if (isnan(maximum_value))
	return false;

    bool found = false;

    for (unsigned int n_radial = 0; n_radial <= energy.get_max_radial();
	 ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= energy.get_max_azimuthal(); ++n_azimuthal) {
	    if (energy(n_radial, n_azimuthal) >
		maximum_value * density(n_radial, n_azimuthal) /
		    parameters::MU * constants::R / (ADIABATICINDEX - 1.0)) {
#ifndef NDEBUG
		logging::print(
		    LOG_DEBUG "assure_maximum_temperature: (%u,%u)=%g>%g\n",
		    n_radial, n_azimuthal,
		    energy(n_radial, n_azimuthal) *
			units::temperature.get_cgs_factor() /
			density(n_radial, n_azimuthal) * parameters::MU /
			constants::R * (ADIABATICINDEX - 1.0),
		    maximum_value * units::temperature.get_cgs_factor(),
		    maximum_value);
#endif
		energy(n_radial, n_azimuthal) =
		    maximum_value * density(n_radial, n_azimuthal) /
		    parameters::MU * constants::R / (ADIABATICINDEX - 1.0);
		found = true;
	    }
	}
    }

    return found;
}

void recalculate_derived_disk_quantities(t_data &data, bool force_update)
{

    if (parameters::Locally_Isothermal) {
	compute_pressure(data, force_update);
    }

    if (parameters::Adiabatic || parameters::Polytropic) {
	compute_temperature(data, force_update);
	compute_sound_speed(data, force_update);
	compute_aspect_ratio(data, force_update);
	compute_pressure(data, force_update);
    }

    viscosity::update_viscosity(data);
}

void init_euler(t_data &data)
{
    InitCellCenterCoordinates();
    InitTransport();

    if (parameters::Locally_Isothermal) {
	compute_sound_speed(data, false);
	compute_pressure(data, false);
	compute_temperature(data, false);
	compute_aspect_ratio(data, false);
    }

    if (parameters::Adiabatic || parameters::Polytropic) {
	compute_temperature(data, false);
	compute_sound_speed(data, false);
	compute_aspect_ratio(data, false);
	compute_pressure(data, false);
    }

    viscosity::update_viscosity(data);
}

static double CalculateHydroTimeStep(t_data &data, double dt, double force_calc)
{
    double local_gas_time_step_cfl = 1.0;
    double global_gas_time_step_cfl;

    if (!SloppyCFL || force_calc) {
	local_gas_time_step_cfl = 1.0;
	local_gas_time_step_cfl = condition_cfl(
	    data, data[t_data::V_RADIAL], data[t_data::V_AZIMUTHAL],
	    data[t_data::SOUNDSPEED], DT - dtemp);
	MPI_Allreduce(&local_gas_time_step_cfl, &global_gas_time_step_cfl, 1,
		      MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
	dt = (DT - dtemp) / global_gas_time_step_cfl;
    }
    return dt;
}

static void init_corotation(t_data &data, double &planet_corot_ref_old_x,
			    double &planet_corot_ref_old_y)
{
    if (Corotating == YES) {
	// save old planet positions
	const unsigned int n = parameters::corotation_reference_body;
	const auto &planet = data.get_planetary_system().get_planet(n);
	planet_corot_ref_old_x = planet.get_x();
	planet_corot_ref_old_y = planet.get_y();
    }
}

static void handle_corotation(t_data &data, const double dt,
			      const double corot_old_x,
			      const double corot_old_y)
{
    if (Corotating == YES) {
	unsigned int n = parameters::corotation_reference_body;
	auto &planet = data.get_planetary_system().get_planet(n);
	const double x = planet.get_x();
	const double y = planet.get_y();
	const double distance_new = std::sqrt(std::pow(x, 2) + std::pow(y, 2));
	const double distance_old =
	    std::sqrt(std::pow(corot_old_x, 2) + std::pow(corot_old_y, 2));
	const double cross = corot_old_x * y - x * corot_old_y;

	// new = r_new x r_old = distance_new * distance_old * sin(alpha*dt)
	const double OmegaNew =
	    asin(cross / (distance_new * distance_old)) / dt;

	const double domega = (OmegaNew - OmegaFrame);
	if (parameters::calculate_disk) {
	    correct_v_azimuthal(data[t_data::V_AZIMUTHAL], domega);
	}
	OmegaFrame = OmegaNew;
    }

    if (parameters::integrate_planets) {
	data.get_planetary_system().rotate(OmegaFrame * dt);
    }
    if (parameters::integrate_particles) {
	particles::rotate(OmegaFrame * dt);
    }

    FrameAngle += OmegaFrame * dt;
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
void copy_polargrid(t_polargrid &dst, t_polargrid &src)
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
*/
void move_polargrid(t_polargrid &dst, t_polargrid &src)
{
    assert((dst.get_size_radial() == src.get_size_radial()) &&
	   (dst.get_size_azimuthal() == src.get_size_azimuthal()));

    std::swap(dst.Field, src.Field);
}

/**
	switches polar grids

	\param dst destination polar grid
	\param src source polar grid
*/
void SwitchPolarGrid(t_polargrid *dst, t_polargrid *src)
{
    double *tmp;

    assert(dst->Nsec == src->Nsec);
    assert(dst->Nrad == dst->Nrad);

    tmp = dst->Field;
    dst->Field = src->Field;
    src->Field = tmp;
}

/**
	\param data
	\param sys
*/
void AlgoGas(unsigned int nTimeStep, t_data &data)
{
    // old coordinates of corotation body
    double planet_corot_ref_old_x = 0.0;
    double planet_corot_ref_old_y = 0.0;

    dtemp = 0.0;

    if (parameters::calculate_disk) {
	CommunicateBoundaries(&data[t_data::DENSITY], &data[t_data::V_RADIAL],
			      &data[t_data::V_AZIMUTHAL],
			      &data[t_data::ENERGY]);
    }
    // recalculate timestep, even for no_disk = true, so that particle drag has
    // reasonable timestep size
    double dt = CalculateHydroTimeStep(data, 0.0, true);

    boundary_conditions::apply_boundary_condition(data, dt, false);

    // keep mass constant
    //const double total_disk_mass_old = quantities::gas_total_mass(data);

    while (dtemp < DT) {
	logging::print_master(
	    LOG_VERBOSE
	    "AlgoGas: Total: %*i/%i (%5.2f %%) - Timestep: %#7f/%#7f (%5.2f %%)\n",
	    (int)ceil(log10(NTOT)), nTimeStep, NTOT,
	    (double)nTimeStep / (double)NTOT * 100.0, dtemp, DT,
	    dtemp / DT * 100.0);

	dtemp += dt;

	init_corotation(data, planet_corot_ref_old_x, planet_corot_ref_old_y);

	if (parameters::disk_feedback) {
	    ComputeDiskOnNbodyAccel(data);
	}
	/* Indirect term star's potential computed here */
	ComputeNbodyOnNbodyAccel(data.get_planetary_system());
	ComputeIndirectTerm(data);

	if (parameters::calculate_disk) {
	    /* Gravitational potential from star and planet(s) is computed and
	     * stored here*/
	    if (parameters::body_force_from_potential) {
		CalculateNbodyPotential(data);
	    } else {
		CalculateAccelOnGas(data);
	    }
	}

	/* Planets' velocities are updated here from gravitationnal
	 * interaction with disk */
	if (parameters::disk_feedback) {
	    UpdatePlanetVelocitiesWithDiskForce(data, dt);
	}

	if (parameters::integrate_particles) {
	    particles::integrate(data, dt);
	}

	/* Planets' positions and velocities are updated from gravitational
	 * interaction with star and other planets */
	if (parameters::integrate_planets) {
	    data.get_planetary_system().integrate(PhysicalTime, dt);
	}

	/* Below we correct v_azimuthal, planet's position and velocities if we
	 * work in a frame non-centered on the star. Same for dust particles. */
	handle_corotation(data, dt, planet_corot_ref_old_x,
			  planet_corot_ref_old_y);

	/* Now we update gas */
	if (parameters::calculate_disk) {
	    HandleCrash(data);

	    update_with_sourceterms(data, dt);

	    if (EXPLICIT_VISCOSITY) {
		// compute and add acceleartions due to disc viscosity as a
		// source term
		update_with_artificial_viscosity(data, dt);

		recalculate_derived_disk_quantities(data, true);

		SetTemperatureFloorCeilValues(data, __FILE__, __LINE__);

		ComputeViscousStressTensor(data);
		viscosity::update_velocities_with_viscosity(
		    data, data[t_data::V_RADIAL], data[t_data::V_AZIMUTHAL],
		    dt);
	    }

	    if (!EXPLICIT_VISCOSITY) {
		Sts(data, dt);
	    }

	    boundary_conditions::apply_boundary_condition(data, dt, false);

	    if (parameters::Adiabatic) {

		// ComputeViscousStressTensor(data);
		SubStep3(data, dt);

		SetTemperatureFloorCeilValues(data, __FILE__, __LINE__);

		if (parameters::radiative_diffusion_enabled) {
		    radiative_diffusion(data, dt);
		    SetTemperatureFloorCeilValues(data, __FILE__, __LINE__);
		}
	    }

	    Transport(data, &data[t_data::DENSITY], &data[t_data::V_RADIAL],
		      &data[t_data::V_AZIMUTHAL], &data[t_data::ENERGY], dt);

	    // assure minimum density after all substeps & transport
	    assure_minimum_value(data[t_data::DENSITY],
				 parameters::sigma_floor * parameters::sigma0);

	    if (parameters::Adiabatic) {
		// assure minimum temperature after all substeps & transport. it
		// is crucial the check minimum density before!
		SetTemperatureFloorCeilValues(data, __FILE__, __LINE__);
	    }
	    boundary_conditions::apply_boundary_condition(data, dt, true);

	    mdcp = CircumPlanetaryMass(data);
	    exces_mdcp = mdcp - mdcp0;

	    CalculateMonitorQuantitiesAfterHydroStep(data, nTimeStep, dt);

	    //const double total_disk_mass_new = quantities::gas_total_mass(data);

	    //data[t_data::DENSITY] *=
		//(total_disk_mass_old / total_disk_mass_new);
	}

	PhysicalTime += dt;
	N_iter = N_iter + 1;
	logging::print_runtime_info(nTimeStep / NINTERM, nTimeStep, dt);

	if (parameters::calculate_disk) {
	    CommunicateBoundaries(
		&data[t_data::DENSITY], &data[t_data::V_RADIAL],
		&data[t_data::V_AZIMUTHAL], &data[t_data::ENERGY]);

	    recalculate_derived_disk_quantities(data, true);
	    dt = CalculateHydroTimeStep(data, dt, false);

	    accretion::AccreteOntoPlanets(data, dt);
	}
    }
}

/**
	In this substep we take into account the source part of Euler equations.
   We evolve velocities with pressure gradients, gravitational forces and
   curvature terms
*/
void update_with_sourceterms(t_data &data, double dt)
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

		const double gamma = ADIABATICINDEX;
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
		(data[t_data::DENSITY](n_radial, n_azimuthal) +
		 data[t_data::DENSITY](n_radial - 1, n_azimuthal)) *
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
	    vt2 = 0.25 * vt2 + Rinf[n_radial] * OmegaFrame;
	    vt2 = vt2 * vt2;

	    // add all terms to new v_radial: v_radial_new = v_radial +
	    // dt*(source terms)
	    data[t_data::V_RADIAL](n_radial, n_azimuthal) =
		data[t_data::V_RADIAL](n_radial, n_azimuthal) +
		dt * (-gradp - gradphi + vt2 * InvRinf[n_radial]);
	}
    }

    // update v_azimuthal with source terms
    for (unsigned int n_radial = 0;
	 n_radial <= data[t_data::V_AZIMUTHAL].get_max_radial(); ++n_radial) {
	if (IMPOSEDDISKDRIFT != 0.0) {
	    supp_torque = IMPOSEDDISKDRIFT * 0.5 *
			  std::pow(Rmed[n_radial], -2.5 + SIGMASLOPE);
	}
	const double invdxtheta = 1.0 / (dphi * Rmed[n_radial]);

	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= data[t_data::V_AZIMUTHAL].get_max_azimuthal();
	     ++n_azimuthal) {

	    const double n_az_minus =
		(n_azimuthal == 0 ? data[t_data::PRESSURE].get_max_azimuthal()
				  : n_azimuthal - 1);
	    // 1/Sigma 1/r dP/dphi
	    const double gradp =
		2.0 /
		(data[t_data::DENSITY](n_radial, n_azimuthal) +
		 data[t_data::DENSITY](
		     n_radial, n_azimuthal == 0
				   ? data[t_data::DENSITY].get_max_azimuthal()
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

	    if (IMPOSEDDISKDRIFT != 0.0) {
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
void update_with_artificial_viscosity(t_data &data, double dt)
{

    const bool add_kep_inner =
	(parameters::boundary_inner !=
	 parameters::boundary_condition_evanescent) &&
	(parameters::boundary_inner !=
	 parameters::boundary_condition_boundary_layer) &&
	(parameters::boundary_inner !=
	 parameters::boundary_condition_precribed_time_variable);

    if (add_kep_inner) {
	ApplySubKeplerianBoundaryInner(data[t_data::V_AZIMUTHAL]);
    }

    if ((parameters::boundary_outer !=
	 parameters::boundary_condition_evanescent) &&
	(parameters::boundary_outer !=
	 parameters::boundary_condition_boundary_layer) &&
	(parameters::boundary_outer !=
	 parameters::boundary_condition_precribed_time_variable) &&
	(!parameters::massoverflow)) {
	ApplySubKeplerianBoundaryOuter(data[t_data::V_AZIMUTHAL],
				       add_kep_inner);
    }

    if (parameters::artificial_viscosity ==
	    parameters::artificial_viscosity_SN &&
	EXPLICIT_VISCOSITY) {

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
			data[t_data::DENSITY](n_radial, n_azimuthal) *
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
			data[t_data::DENSITY](n_radial, n_azimuthal) *
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
			(data[t_data::DENSITY](n_radial, n_azimuthal) +
			 data[t_data::DENSITY](n_radial - 1, n_azimuthal)) *
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
			(data[t_data::DENSITY](n_radial, n_azimuthal) +
			 data[t_data::DENSITY](
			     n_radial,
			     n_azimuthal == 0
				 ? data[t_data::DENSITY].get_max_azimuthal()
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

void calculate_qplus(t_data &data)
{

    const double *cell_center_x = CellCenterX->Field;
    const double *cell_center_y = CellCenterY->Field;

    if (EXPLICIT_VISCOSITY) {
	// clear up all Qplus terms
	data[t_data::QPLUS].clear();
    }

    if (parameters::heating_viscous_enabled && EXPLICIT_VISCOSITY) {
	/* We calculate the heating source term Qplus from i=1 to max-1 */
	for (unsigned int n_radial = 1;
	     n_radial <= data[t_data::QPLUS].get_max_radial() - 1; ++n_radial) {
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
			 data[t_data::DENSITY](n_radial, n_azimuthal)) *
			(std::pow(data[t_data::TAU_R_R](n_radial, n_azimuthal),
				  2) +
			 2 * std::pow(tau_r_phi, 2) +
			 std::pow(
			     data[t_data::TAU_PHI_PHI](n_radial, n_azimuthal),
			     2));
		    qplus +=
			(2.0 / 9.0) *
			data[t_data::VISCOSITY](n_radial, n_azimuthal) *
			data[t_data::DENSITY](n_radial, n_azimuthal) *
			std::pow(data[t_data::DIV_V](n_radial, n_azimuthal), 2);

		    qplus *= parameters::heating_viscous_factor;
		    data[t_data::QPLUS](n_radial, n_azimuthal) += qplus;
		}
	    }
	}

	/* We calculate the heating source term Qplus for i=max */
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= data[t_data::QPLUS].get_max_azimuthal();
	     ++n_azimuthal) {
	    if (data[t_data::VISCOSITY](data[t_data::QPLUS].get_max_radial(),
					n_azimuthal) != 0.0) {
		// power-law extrapolation
		double qplus =
		    data[t_data::QPLUS](
			data[t_data::QPLUS].get_max_radial() - 1, n_azimuthal) *
		    std::exp(
			std::log(data[t_data::QPLUS](
				     data[t_data::QPLUS].get_max_radial() - 1,
				     n_azimuthal) /
				 data[t_data::QPLUS](
				     data[t_data::QPLUS].get_max_radial() - 2,
				     n_azimuthal)) *
			std::log(
			    Rmed[data[t_data::QPLUS].get_max_radial()] /
			    Rmed[data[t_data::QPLUS].get_max_radial() - 1]) /
			std::log(
			    Rmed[data[t_data::QPLUS].get_max_radial() - 1] /
			    Rmed[data[t_data::QPLUS].get_max_radial() - 2]));

		data[t_data::QPLUS](data[t_data::QPLUS].get_max_radial(),
				    n_azimuthal) += qplus;
	    }
	}
	/* We calculate the heating source term Qplus for i=0 */
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= data[t_data::QPLUS].get_max_azimuthal();
	     ++n_azimuthal) {
	    if (data[t_data::VISCOSITY](0, n_azimuthal) != 0.0) {
		// power-law extrapolation
		double qplus =
		    data[t_data::QPLUS](1, n_azimuthal) *
		    std::exp(std::log(data[t_data::QPLUS](1, n_azimuthal) /
				      data[t_data::QPLUS](2, n_azimuthal)) *
			     std::log(Rmed[0] / Rmed[1]) /
			     std::log(Rmed[1] / Rmed[2]));

		data[t_data::QPLUS](0, n_azimuthal) += qplus;
	    }
	}
    }

    if (parameters::heating_star_enabled) {
	double ramping = 1.0;
	if (PhysicalTime < parameters::heating_star_ramping_time * DT) {
	    ramping =
		1.0 -
		std::pow(std::cos(PhysicalTime * M_PI / 2.0 /
				  (parameters::heating_star_ramping_time * DT)),
			 2);
	}

	if (parameters::heating_star_simple) {
	    if (!parameters::cooling_radiative_enabled) {
		die("Need to calulate Tau_eff first!\n"); // TODO: make it
							  // properly!
	    }
	    const double x_star =
		data.get_planetary_system().get_planet(0).get_x();
	    const double y_star =
		data.get_planetary_system().get_planet(0).get_y();

	    // Simple star heating (see Masterthesis Alexandros Ziampras)
	    for (unsigned int n_radial = 1;
		 n_radial <= data[t_data::QPLUS].get_max_radial() - 1;
		 ++n_radial) {
		for (unsigned int n_azimuthal = 0;
		     n_azimuthal <= data[t_data::QPLUS].get_max_azimuthal();
		     ++n_azimuthal) {

		    const unsigned int ncell =
			n_radial * data[t_data::DENSITY].get_size_azimuthal() +
			n_azimuthal;
		    const double xc = cell_center_x[ncell];
		    const double yc = cell_center_y[ncell];
		    const double distance = std::sqrt(std::pow(x_star - xc, 2) +
						      std::pow(y_star - yc, 2));
		    const double HoverR =
			data[t_data::ASPECTRATIO](n_radial, n_azimuthal);
		    const double sigma = constants::sigma.get_code_value();
		    const double T_star = parameters::star_temperature;
		    const double R_star = parameters::star_radius;
		    const double tau_eff =
			data[t_data::TAU_EFF](n_radial, n_azimuthal);
		    const double eps = 0.5; // TODO: add a parameter
		    // choose according to Chiang & Goldreich (1997)
		    const double dlogH_dlogr = 9.0 / 7.0;
		    // use eq. 7 from Menou & Goodman (2004) (rearranged), Qirr
		    // = 2*(1-eps)*L_star/(4 pi r^2)*(dlogH/dlogr - 1) * H/r *
		    // 1/Tau_eff here we use (1-eps) =
		    // parameters::heating_star_factor L_star = 4 pi R_star^2
		    // sigma_sb T_star^4
		    double qplus = 2 * (1 - eps); // 2*(1-eps)
		    qplus *=
			sigma * std::pow(T_star, 4) *
			std::pow(R_star / distance, 2); // *L_star/(4 pi r^2)
		    qplus *= dlogH_dlogr - 1;		// *(dlogH/dlogr - 1)
		    qplus *= HoverR;			// * H/r
		    qplus /= tau_eff;			// * 1/Tau_eff
		    data[t_data::QPLUS](n_radial, n_azimuthal) +=
			ramping * qplus;
		}
	    }
	} else {
	    unsigned int *zbuffer = (unsigned int *)malloc(
		parameters::zbuffer_size *
		data[t_data::QPLUS].get_size_azimuthal() *
		sizeof(unsigned int));

	    double dtheta = parameters::zbuffer_maxangle /
			    (double)(parameters::zbuffer_size - 1);

	    for (unsigned int n_azimuthal = 0;
		 n_azimuthal <= data[t_data::QPLUS].get_max_azimuthal();
		 ++n_azimuthal) {
		// init z-buffer
		for (unsigned int n_theta = 0;
		     n_theta < parameters::zbuffer_size; ++n_theta) {
		    zbuffer[n_azimuthal * parameters::zbuffer_size + n_theta] =
			UINT_MAX;
		}

		for (int n_radial =
			 data[t_data::QPLUS].get_max_radial() -
			 (CPU_Rank == CPU_Highest ? GHOSTCELLS_B : CPUOVERLAP);
		     n_radial >=
		     (int)((CPU_Rank == 0) ? GHOSTCELLS_B : CPUOVERLAP);
		     n_radial--) {
		    for (unsigned int n_theta = 0;
			 n_theta < parameters::zbuffer_size; ++n_theta) {
			double theta = (double)n_theta * dtheta;

			if (tan(theta) <
			    data[t_data::ASPECTRATIO](n_radial, n_azimuthal)) {
			    if (zbuffer[n_azimuthal * parameters::zbuffer_size +
					n_theta] > IMIN + n_radial) {
				zbuffer[n_azimuthal * parameters::zbuffer_size +
					n_theta] = IMIN + n_radial;
			    } else {
				break;
			    }
			}
		    }
		}
	    }

	    // sync
	    unsigned int *zbuffer_global = (unsigned int *)malloc(
		parameters::zbuffer_size *
		data[t_data::QPLUS].get_size_azimuthal() *
		sizeof(unsigned int));
	    MPI_Allreduce(zbuffer, zbuffer_global,
			  parameters::zbuffer_size *
			      data[t_data::QPLUS].get_size_azimuthal(),
			  MPI_UNSIGNED, MPI_MIN, MPI_COMM_WORLD);
	    free(zbuffer);
	    zbuffer = zbuffer_global;

	    // calculate visiblity
	    for (unsigned int n_radial = 1;
		 n_radial <= data[t_data::QPLUS].get_max_radial() - 1;
		 ++n_radial) {
		for (unsigned int n_azimuthal = 0;
		     n_azimuthal <= data[t_data::QPLUS].get_max_azimuthal();
		     ++n_azimuthal) {
		    // check for self-shadowing
		    unsigned int n_theta = 0;

		    // get next nt
		    while ((tan(n_theta * dtheta) <=
			    data[t_data::ASPECTRATIO](n_radial, n_azimuthal)) &&
			   (n_theta < parameters::zbuffer_size))
			n_theta++;

		    if (zbuffer[n_azimuthal * parameters::zbuffer_size +
				n_theta] >= IMIN + n_radial) {
			data[t_data::VISIBILITY](n_radial, n_azimuthal) = 1.0;
		    } else {
			data[t_data::VISIBILITY](n_radial, n_azimuthal) = 0.0;
		    }
		}
	    }

	    for (unsigned int n_radial = 1;
		 n_radial <= data[t_data::QPLUS].get_max_radial() - 1;
		 ++n_radial) {
		for (unsigned int n_azimuthal = 0;
		     n_azimuthal <= data[t_data::QPLUS].get_max_azimuthal();
		     ++n_azimuthal) {
		    // calculate "mean" visibility
		    double visibility = 0;

		    visibility += data[t_data::VISIBILITY](
			n_radial - 1,
			n_azimuthal == 0
			    ? data[t_data::VISIBILITY].get_max_azimuthal()
			    : n_azimuthal - 1);
		    visibility +=
			data[t_data::VISIBILITY](n_radial - 1, n_azimuthal);
		    visibility += data[t_data::VISIBILITY](
			n_radial - 1,
			n_azimuthal ==
				data[t_data::VISIBILITY].get_max_azimuthal()
			    ? 0
			    : n_azimuthal + 1);

		    visibility += data[t_data::VISIBILITY](
			n_radial,
			n_azimuthal == 0
			    ? data[t_data::VISIBILITY].get_max_azimuthal()
			    : n_azimuthal - 1);
		    visibility +=
			data[t_data::VISIBILITY](n_radial, n_azimuthal);
		    visibility += data[t_data::VISIBILITY](
			n_radial,
			n_azimuthal ==
				data[t_data::VISIBILITY].get_max_azimuthal()
			    ? 0
			    : n_azimuthal + 1);

		    visibility += data[t_data::VISIBILITY](
			n_radial + 1,
			n_azimuthal == 0
			    ? data[t_data::VISIBILITY].get_max_azimuthal()
			    : n_azimuthal - 1);
		    visibility +=
			data[t_data::VISIBILITY](n_radial + 1, n_azimuthal);
		    visibility += data[t_data::VISIBILITY](
			n_radial + 1,
			n_azimuthal ==
				data[t_data::VISIBILITY].get_max_azimuthal()
			    ? 0
			    : n_azimuthal + 1);

		    visibility /= 9.0;

		    // see Günther et. al (2004) or Phil Armitage "Astrophysics
		    // of Planet Format" p. 46
		    double alpha =
			(data[t_data::ASPECTRATIO](n_radial, n_azimuthal) *
			     Rmed[n_radial] -
			 data[t_data::ASPECTRATIO](n_radial - 1, n_azimuthal) *
			     Rmed[n_radial - 1]) *
			    InvDiffRmed[n_radial] -
			data[t_data::ASPECTRATIO](n_radial, n_azimuthal);

		    if (alpha < 0.0) {
			alpha = 0.0;
		    }

		    // primary star
		    double qplus =
			ramping * visibility * parameters::heating_star_factor *
			2.0 * alpha * constants::sigma.get_code_value() *
			std::pow(parameters::star_temperature, 4) *
			std::pow(parameters::star_radius / Rmed[n_radial], 2);

		    data[t_data::QPLUS](n_radial, n_azimuthal) += qplus;

		    /* // secondary star/plantes
		    for (unsigned int planet = 1; planet >
		    data.get_planetary_system().get_number_of_planets();
		    ++planet) { double planet_x =
		    data.get_planetary_system().get_planet(planet).get_x();
			    double planet_y =
		    data.get_planetary_system().get_planet(planet).get_y();

			    double cell_x =
		    Rmed[n_radial]*cos((double)n_azimuthal/(double)data[t_data::QPLUS].get_size_azimuthal()*2.0*PI);
			    double cell_y =
		    Rmed[n_radial]*sin((double)n_azimuthal/(double)data[t_data::QPLUS].get_size_azimuthal()*2.0*PI);

			    double distance =
		    sqrt(pow2(planet_x-cell_x)+pow2(planet_y-cell_y));

			    data[t_data::QPLUS](n_radial,n_azimuthal) +=
		    parameters::heating_star_factor*alpha*constants::sigma.get_code_value()*pow4(data.get_planetary_system().get_planet(planet).get_temperature())*pow2(data.get_planetary_system().get_planet(planet).get_radius()/distance);
		    }
		    */
		}
	    }

	    free(zbuffer);
	}
    }
}

void calculate_qminus(t_data &data)
{
    // clear up all Qminus terms
    data[t_data::QMINUS].clear();

    // beta cooling
    if (parameters::cooling_beta_enabled) {
	for (unsigned int n_radial = 0;
	     n_radial <= data[t_data::QMINUS].get_max_radial(); ++n_radial) {
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

		const double qminus = E * omega_k * beta_inv;

		data[t_data::QMINUS](n_radial, n_azimuthal) += qminus;
	    }
	}
    }

    // local radiative cooling
    if (parameters::cooling_radiative_enabled) {
	double kappaCGS;
	double temperatureCGS;
	double densityCGS;

	for (unsigned int n_radial = 0;
	     n_radial <= data[t_data::QMINUS].get_max_radial(); ++n_radial) {
	    for (unsigned int n_azimuthal = 0;
		 n_azimuthal <= data[t_data::QMINUS].get_max_azimuthal();
		 ++n_azimuthal) {
		// calculate Rosseland mean opacity kappa. opaclin needs values
		// in cgs units
		temperatureCGS =
		    data[t_data::TEMPERATURE](n_radial, n_azimuthal) *
		    units::temperature;

		// TODO: user aspect ratio
		densityCGS = data[t_data::DENSITY](n_radial, n_azimuthal) /
			     (parameters::density_factor *
			      data[t_data::SOUNDSPEED](n_radial, n_azimuthal) /
			      std::sqrt(ADIABATICINDEX) /
			      calculate_omega_kepler(Rmed[n_radial])) *
			     units::density;

		kappaCGS = opacity::opacity(densityCGS, temperatureCGS);

		data[t_data::KAPPA](n_radial, n_azimuthal) =
		    parameters::kappa_factor * kappaCGS *
		    units::opacity.get_inverse_cgs_factor();

		// mean vertical optical depth: tau = 1/2 kappa Sigma
		data[t_data::TAU](n_radial, n_azimuthal) =
		    parameters::tau_factor *
		    (1.0 / parameters::density_factor) *
		    data[t_data::KAPPA](n_radial, n_azimuthal) *
		    data[t_data::DENSITY](n_radial, n_azimuthal);

		//  tau_eff = 3/8 tau + sqrt(3)/4 + 1/(4*tau+tau_min)
		data[t_data::TAU_EFF](n_radial, n_azimuthal) =
		    3.0 / 8.0 * data[t_data::TAU](n_radial, n_azimuthal) +
		    std::sqrt(3.0) / 4.0 +
		    1.0 /
			(4.0 * data[t_data::TAU](n_radial, n_azimuthal) + 0.01);

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

		const double qminus = factor * 2 * sigma_sb * T4 / tau_eff;

		data[t_data::QMINUS](n_radial, n_azimuthal) += qminus;
	    }
	}
    }
}

/**
	In this substep we take into account the source part of energy equation.
   We evolve internal energy with compression/dilatation and heating terms
*/
void SubStep3(t_data &data, double dt)
{
    double num, den;

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
		double pdivv = (ADIABATICINDEX - 1.0) * dt *
			       data[t_data::DIV_V](n_radial, n_azimuthal) *
			       data[t_data::ENERGY](n_radial, n_azimuthal);
		data[t_data::P_DIVV](n_radial, n_azimuthal) = pdivv;

		sum_without_ghost_cells(data.pdivv_total, pdivv, n_radial);
	    }
	}
    }

    // Now we can update energy with source terms
    for (unsigned int n_radial = 0;
	 n_radial <= data[t_data::ENERGY].get_max_radial(); ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= data[t_data::ENERGY].get_max_azimuthal();
	     ++n_azimuthal) {

	    const double sigma_sb = constants::sigma;
	    const double c = constants::c;
	    const double mu = parameters::MU;
	    const double gamma = ADIABATICINDEX;
	    const double Rgas = constants::R;

	    const double R = Rmed[n_radial];
	    const double h = data[t_data::ASPECTRATIO](n_radial, n_azimuthal);
	    const double H = h * R;

	    const double sigma = data[t_data::DENSITY](n_radial, n_azimuthal);
	    const double energy = data[t_data::ENERGY](n_radial, n_azimuthal);
	    const double qplus = data[t_data::QPLUS](n_radial, n_azimuthal);
	    const double qminus = data[t_data::QMINUS](n_radial, n_azimuthal);

	    const double inv_pow4 =
		std::pow(mu * (gamma - 1.0) / (Rgas * sigma), 4);
	    double alpha = 1.0 + 2.0 * H * 4.0 * sigma_sb / c * inv_pow4 *
				     std::pow(energy, 3);

	    num = dt * qplus - dt * qminus + alpha * energy;
	    den = alpha;

	    data[t_data::ENERGY](n_radial, n_azimuthal) = num / den;
	}
    }
}

static inline double flux_limiter(double R)
{
    // flux limiter
    if (R <= 2) {
	return 2.0 / (3 + std::sqrt(9 + 10 * std::pow(R, 2)));
    } else {
	return 10.0 / (10 * R + 9 + std::sqrt(180 * R + 81));
    }
}

void radiative_diffusion(t_data &data, double dt)
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

    // update temperature, soundspeed and aspect ratio
    compute_temperature(data, true);
    compute_sound_speed(data, true);
    compute_aspect_ratio(data, true);

    // calcuate Ka for K(i/2,j)
    for (unsigned int n_radial = 1; n_radial <= Ka.get_max_radial() - 1;
	 ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= Ka.get_max_azimuthal(); ++n_azimuthal) {
	    unsigned int n_azimuthal_plus =
		(n_azimuthal == Ka.get_max_azimuthal() ? 0 : n_azimuthal + 1);
	    unsigned int n_azimuthal_minus =
		(n_azimuthal == 0 ? Ka.get_max_azimuthal() : n_azimuthal - 1);

	    // average temperature radially
	    double temperature =
		0.5 * (data[t_data::TEMPERATURE](n_radial - 1, n_azimuthal) +
		       data[t_data::TEMPERATURE](n_radial, n_azimuthal));
	    double density =
		0.5 * (data[t_data::DENSITY](n_radial - 1, n_azimuthal) +
		       data[t_data::DENSITY](n_radial, n_azimuthal));
	    double aspectratio =
		0.5 * (data[t_data::ASPECTRATIO](n_radial - 1, n_azimuthal) +
		       data[t_data::ASPECTRATIO](n_radial, n_azimuthal));

	    double temperatureCGS = temperature * units::temperature;
	    double H = aspectratio * Ra[n_radial];
	    double densityCGS =
		density / (parameters::density_factor * H) * units::density;

	    double kappaCGS = opacity::opacity(densityCGS, temperatureCGS);
	    double kappa = parameters::kappa_factor * kappaCGS *
			   units::opacity.get_inverse_cgs_factor();

	    double denom = 1.0 / (density * kappa);

	    // Levermore & Pomraning 1981
	    // R = 4 |nabla T\/T * 1/(rho kappa)
	    double dT_dr =
		(data[t_data::TEMPERATURE](n_radial, n_azimuthal) -
		 data[t_data::TEMPERATURE](n_radial - 1, n_azimuthal)) *
		InvDiffRmed[n_radial];
	    double dT_dphi =
		InvRinf[n_radial] *
		(0.5 * (data[t_data::TEMPERATURE](n_radial - 1,
						  n_azimuthal_plus) +
			data[t_data::TEMPERATURE](n_radial, n_azimuthal_plus)) -
		 0.5 *
		     (data[t_data::TEMPERATURE](n_radial - 1,
						n_azimuthal_minus) +
		      data[t_data::TEMPERATURE](n_radial, n_azimuthal_minus))) /
		(2 * dphi);

	    double nabla_T =
		std::sqrt(std::pow(dT_dr, 2) + std::pow(dT_dphi, 2));

	    double R;

	    if ((n_radial > 1) && (n_radial < Ka.get_max_radial() - 1)) {
		R = 4.0 * nabla_T / temperature * denom * H *
		    parameters::density_factor;
	    } else {
		unsigned int n_radial_adjusted;

		if (n_radial == 1) {
		    n_radial_adjusted = n_radial + 1;
		} else {
		    n_radial_adjusted = n_radial - 1;
		}
		double temperature =
		    0.5 *
		    (data[t_data::TEMPERATURE](n_radial_adjusted - 1,
					       n_azimuthal) +
		     data[t_data::TEMPERATURE](n_radial_adjusted, n_azimuthal));
		double density =
		    0.5 *
		    (data[t_data::DENSITY](n_radial_adjusted - 1, n_azimuthal) +
		     data[t_data::DENSITY](n_radial_adjusted, n_azimuthal));
		double aspectratio =
		    0.5 *
		    (data[t_data::ASPECTRATIO](n_radial_adjusted - 1,
					       n_azimuthal) +
		     data[t_data::ASPECTRATIO](n_radial_adjusted, n_azimuthal));

		double temperatureCGS = temperature * units::temperature;
		double H = aspectratio * Ra[n_radial_adjusted];
		double densityCGS =
		    density / (parameters::density_factor * H) * units::density;

		double kappaCGS = opacity::opacity(densityCGS, temperatureCGS);
		double kappa = parameters::kappa_factor * kappaCGS *
			       units::opacity.get_inverse_cgs_factor();

		double denom = 1.0 / (density * kappa);

		// Levermore & Pomraning 1981
		// R = 4 |nabla T\/T * 1/(rho kappa)
		double dT_dr =
		    (data[t_data::TEMPERATURE](n_radial_adjusted, n_azimuthal) -
		     data[t_data::TEMPERATURE](n_radial_adjusted - 1,
					       n_azimuthal)) *
		    InvDiffRmed[n_radial];
		double dT_dphi =
		    InvRinf[n_radial] *
		    (0.5 * (data[t_data::TEMPERATURE](n_radial_adjusted - 1,
						      n_azimuthal_plus) +
			    data[t_data::TEMPERATURE](n_radial_adjusted,
						      n_azimuthal_plus)) -
		     0.5 * (data[t_data::TEMPERATURE](n_radial_adjusted - 1,
						      n_azimuthal_minus) +
			    data[t_data::TEMPERATURE](n_radial_adjusted,
						      n_azimuthal_minus))) /
		    (2 * dphi);

		double nabla_T =
		    std::sqrt(std::pow(dT_dr, 2) + std::pow(dT_dphi, 2));

		R = 4.0 * nabla_T / temperature * denom * H *
		    parameters::density_factor;
	    }

	    double lambda = flux_limiter(R);

	    Ka(n_radial, n_azimuthal) =
		8.0 * 4.0 * constants::sigma.get_code_value() * lambda * H *
		std::pow(temperature, 3) * denom;
	    // Ka(n_radial, n_azimuthal)
	    // = 16.0*parameters::density_factor*constants::sigma.get_code_value()*lambda*H*pow3(temperature)*denom;
	}
    }

    // calcuate Kb for K(i,j/2)
    for (unsigned int n_radial = 1; n_radial <= Kb.get_max_radial() - 1;
	 ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= Kb.get_max_azimuthal(); ++n_azimuthal) {
	    // unsigned int n_azimuthal_plus = (n_azimuthal ==
	    // Kb.get_max_azimuthal() ? 0 : n_azimuthal + 1);
	    unsigned int n_azimuthal_minus =
		(n_azimuthal == 0 ? Kb.get_max_azimuthal() : n_azimuthal - 1);

	    // average temperature azimuthally
	    double temperature =
		0.5 * (data[t_data::TEMPERATURE](n_radial, n_azimuthal_minus) +
		       data[t_data::TEMPERATURE](n_radial, n_azimuthal));
	    double density =
		0.5 * (data[t_data::DENSITY](n_radial, n_azimuthal_minus) +
		       data[t_data::DENSITY](n_radial, n_azimuthal));
	    double aspectratio =
		0.5 * (data[t_data::ASPECTRATIO](n_radial, n_azimuthal_minus) +
		       data[t_data::ASPECTRATIO](n_radial, n_azimuthal));

	    double temperatureCGS = temperature * units::temperature;
	    double H = aspectratio * Rb[n_radial];
	    double densityCGS =
		density / (parameters::density_factor * H) * units::density;

	    double kappaCGS = opacity::opacity(densityCGS, temperatureCGS);
	    double kappa = parameters::kappa_factor * kappaCGS *
			   units::opacity.get_inverse_cgs_factor();

	    double denom = 1.0 / (density * kappa);

	    // Levermore & Pomraning 1981
	    // R = 4 |nabla T\/T * 1/(rho kappa)
	    double dT_dr =
		(0.5 * (data[t_data::TEMPERATURE](n_radial - 1,
						  n_azimuthal_minus) +
			data[t_data::TEMPERATURE](n_radial - 1, n_azimuthal)) -
		 0.5 * (data[t_data::TEMPERATURE](n_radial + 1,
						  n_azimuthal_minus) +
			data[t_data::TEMPERATURE](n_radial + 1, n_azimuthal))) /
		(Ra[n_radial - 1] - Ra[n_radial + 1]);
	    double dT_dphi =
		InvRmed[n_radial] *
		(data[t_data::TEMPERATURE](n_radial, n_azimuthal) -
		 data[t_data::TEMPERATURE](n_radial, n_azimuthal_minus)) /
		dphi;

	    double nabla_T =
		std::sqrt(std::pow(dT_dr, 2) + std::pow(dT_dphi, 2));

	    double R = 4.0 * nabla_T / temperature * denom * H *
		       parameters::density_factor;

	    double lambda = flux_limiter(R);
	    /*if (n_radial == 4) {
		    printf("kb:
	    phi=%lg\tR=%lg\tlambda=%lg\tdphi=%lg\tdr=%lg\tnabla=%lg\tT=%lg\tH=%lg\n",
	    dphi*n_azimuthal, R, lambda,dT_dphi,dT_dr,nabla_T,temperature,H);
	    }*/

	    Kb(n_radial, n_azimuthal) =
		8 * 4 * constants::sigma.get_code_value() * lambda * H *
		std::pow(temperature, 3) * denom;
	    // Kb(n_radial, n_azimuthal)
	    // = 16.0*parameters::density_factor*constants::sigma.get_code_value()*lambda*H*pow3(temperature)*denom;
	}
    }

    double c_v = constants::R / (parameters::MU * (ADIABATICINDEX - 1.0));

    // calculate A,B,C,D,E
    for (unsigned int n_radial = 1;
	 n_radial <= data[t_data::TEMPERATURE].get_max_radial() - 1;
	 ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= data[t_data::TEMPERATURE].get_max_azimuthal();
	     ++n_azimuthal) {
	    double H =
		data[t_data::ASPECTRATIO](n_radial, n_azimuthal) * Rb[n_radial];
	    // -dt H /(Sigma * c_v)
	    double common_factor =
		-dt * parameters::density_factor * H /
		(data[t_data::DENSITY](n_radial, n_azimuthal) * c_v);

	    // 2/(dR^2)
	    double common_AC =
		common_factor * 2.0 /
		(std::pow(Ra[n_radial + 1], 2) - std::pow(Ra[n_radial], 2));
	    A(n_radial, n_azimuthal) = common_AC * Ka(n_radial, n_azimuthal) *
				       Ra[n_radial] * InvDiffRmed[n_radial];
	    C(n_radial, n_azimuthal) =
		common_AC * Ka(n_radial + 1, n_azimuthal) * Ra[n_radial + 1] *
		InvDiffRmed[n_radial + 1];

	    // 1/(r^2 dphi^2)
	    double common_DE =
		common_factor / (std::pow(Rb[n_radial], 2) * std::pow(dphi, 2));
	    D(n_radial, n_azimuthal) = common_DE * Kb(n_radial, n_azimuthal);
	    E(n_radial, n_azimuthal) =
		common_DE * Kb(n_radial, n_azimuthal == Kb.get_max_azimuthal()
					     ? 0
					     : n_azimuthal + 1);

	    B(n_radial, n_azimuthal) =
		-A(n_radial, n_azimuthal) - C(n_radial, n_azimuthal) -
		D(n_radial, n_azimuthal) - E(n_radial, n_azimuthal) + 1.0;

	    Told(n_radial, n_azimuthal) =
		data[t_data::TEMPERATURE](n_radial, n_azimuthal);

	    /*double energy_change = dt*data[t_data::QPLUS](n_radial,
	    n_azimuthal)
		- dt*data[t_data::QMINUS](n_radial, n_azimuthal)
		- dt*data[t_data::P_DIVV](n_radial, n_azimuthal);

	    double temperature_change =
	    MU/R*(ADIABATICINDEX-1.0)*energy_change/data[t_data::DENSITY](n_radial,n_azimuthal);
	    Told(n_radial, n_azimuthal) += temperature_change;

	    if (Told(n_radial, n_azimuthal) <
	    parameters::minimum_temperature*units::temperature.get_inverse_cgs_factor())
	    { data[t_data::TEMPERATURE](n_radial, n_azimuthal) =
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

    int l = CPUOVERLAP * NAzimuthal;
    int oo = (data[t_data::TEMPERATURE].Nrad - CPUOVERLAP) * NAzimuthal;
    int o = (data[t_data::TEMPERATURE].Nrad - 2 * CPUOVERLAP) * NAzimuthal;

    // do SOR
    while ((norm_change > 1e-12) &&
	   (parameters::radiative_diffusion_max_iterations > iterations)) {
	// if ((CPU_Rank == CPU_Highest) && parameters::boundary_outer ==
	// parameters::boundary_condition_open) {
	// 	// set temperature to T_min in outermost cells
	// 	for (unsigned int n_azimuthal = 0; n_azimuthal <=
	// data[t_data::TEMPERATURE].get_max_azimuthal(); ++n_azimuthal) {
	// 		data[t_data::TEMPERATURE](data[t_data::TEMPERATURE].get_max_radial(),
	// n_azimuthal) =
	// parameters::minimum_temperature*units::temperature.get_inverse_cgs_factor();
	// 	}
	// }

	// if ((CPU_Rank == 0) && parameters::boundary_inner ==
	// parameters::boundary_condition_open) {
	// 	// set temperature to T_min in innermost cells
	// 	for (unsigned int n_azimuthal = 0; n_azimuthal <=
	// data[t_data::TEMPERATURE].get_max_azimuthal(); ++n_azimuthal) {
	// 		data[t_data::TEMPERATURE](0, n_azimuthal) =
	// parameters::minimum_temperature*units::temperature.get_inverse_cgs_factor();
	// 	}
	// }
	boundary_conditions::apply_boundary_condition(data, dt, false);

	norm_change = absolute_norm;
	absolute_norm = 0.0;

	for (unsigned int n_radial = 1;
	     n_radial <= data[t_data::TEMPERATURE].get_max_radial() - 1;
	     ++n_radial) {
	    for (unsigned int n_azimuthal = 0;
		 n_azimuthal <= data[t_data::TEMPERATURE].get_max_azimuthal();
		 ++n_azimuthal) {
		double old_value =
		    data[t_data::TEMPERATURE](n_radial, n_azimuthal);
		unsigned int n_azimuthal_plus =
		    (n_azimuthal ==
			     data[t_data::TEMPERATURE].get_max_azimuthal()
			 ? 0
			 : n_azimuthal + 1);
		unsigned int n_azimuthal_minus =
		    (n_azimuthal == 0
			 ? data[t_data::TEMPERATURE].get_max_azimuthal()
			 : n_azimuthal - 1);

		data[t_data::TEMPERATURE](n_radial, n_azimuthal) =
		    (1.0 - omega) *
			data[t_data::TEMPERATURE](n_radial, n_azimuthal) -
		    omega / B(n_radial, n_azimuthal) *
			(A(n_radial, n_azimuthal) *
			     data[t_data::TEMPERATURE](n_radial - 1,
						       n_azimuthal) +
			 C(n_radial, n_azimuthal) *
			     data[t_data::TEMPERATURE](n_radial + 1,
						       n_azimuthal) +
			 D(n_radial, n_azimuthal) *
			     data[t_data::TEMPERATURE](n_radial,
						       n_azimuthal_minus) +
			 E(n_radial, n_azimuthal) *
			     data[t_data::TEMPERATURE](n_radial,
						       n_azimuthal_plus) -
			 Told(n_radial, n_azimuthal));

		// only non ghostcells to norm and don't count overlap cell's
		// twice
		bool isnot_ghostcell_rank_0 =
		    n_radial > ((CPU_Rank == 0) ? GHOSTCELLS_B : CPUOVERLAP);
		bool isnot_ghostcell_rank_highest =
		    (n_radial <
		     (data[t_data::TEMPERATURE].get_max_radial() -
		      ((CPU_Rank == CPU_Highest) ? GHOSTCELLS_B : CPUOVERLAP)));

		if (isnot_ghostcell_rank_0 && isnot_ghostcell_rank_highest) {
		    absolute_norm +=
			std::pow(old_value - data[t_data::TEMPERATURE](
						 n_radial, n_azimuthal),
				 2);
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
	memcpy(SendInnerBoundary, data[t_data::TEMPERATURE].Field + l,
	       l * sizeof(double));
	memcpy(SendOuterBoundary, data[t_data::TEMPERATURE].Field + o,
	       l * sizeof(double));

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
	    memcpy(data[t_data::TEMPERATURE].Field, RecvInnerBoundary,
		   l * sizeof(double));
	}

	if (CPU_Rank != CPU_Highest) {
	    MPI_Wait(&req3, &global_MPI_Status);
	    MPI_Wait(&req4, &global_MPI_Status);
	    memcpy(data[t_data::TEMPERATURE].Field + oo, RecvOuterBoundary,
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
	omega = 2.0;
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
    for (unsigned int n_radial = 0;
	 n_radial <= data[t_data::ENERGY].get_max_radial(); ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= data[t_data::ENERGY].get_max_azimuthal();
	     ++n_azimuthal) {
	    data[t_data::ENERGY](n_radial, n_azimuthal) =
		data[t_data::TEMPERATURE](n_radial, n_azimuthal) *
		data[t_data::DENSITY](n_radial, n_azimuthal) /
		(ADIABATICINDEX - 1.0) / parameters::MU * constants::R;
	}
    }
}

/**
	\param VRadial radial velocity polar grid
	\param VAzimuthal azimuthal velocity polar grid
	\param SoundSpeed sound speed polar grid
	\param deltaT
*/
double condition_cfl(t_data &data, t_polargrid &v_radial,
		     t_polargrid &v_azimuthal, t_polargrid &soundspeed,
		     double deltaT)
{
    dt_parabolic_local = 1e300;
    std::vector<double> v_mean(v_radial.get_size_radial());
    std::vector<double> v_residual(v_radial.get_size_azimuthal());
    double dtGlobal, dtLocal;

    // debugging variables
    double viscRadial = 0.0, viscAzimuthal = 0.0;
    unsigned int n_azimuthal_debug = 0, n_radial_debug = 0;
    double itdbg1 = DBL_MAX, itdbg2 = DBL_MAX, itdbg3 = DBL_MAX,
	   itdbg4 = DBL_MAX, itdbg5 = DBL_MAX, mdtdbg = DBL_MAX;

    dtGlobal = DBL_MAX;

    // Calculate and fill VMean array
    for (unsigned int n_radial = 0; n_radial <= v_azimuthal.get_max_radial();
	 ++n_radial) {
	v_mean[n_radial] = 0.0;
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= v_azimuthal.get_max_azimuthal(); ++n_azimuthal) {
	    v_mean[n_radial] += v_azimuthal(n_radial, n_azimuthal);
	}
	v_mean[n_radial] /= (double)(v_azimuthal.get_size_azimuthal());
    }

    for (unsigned int n_radial = One_or_active; n_radial < Max_or_active;
	 ++n_radial) {
	// cell sizes in radial & azimuthal direction
	double dxRadial = Rsup[n_radial] - Rinf[n_radial];
	double dxAzimuthal = Rmed[n_radial] * 2.0 * M_PI /
			     (double)(v_radial.get_size_azimuthal());

	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= v_radial.get_max_azimuthal(); ++n_azimuthal) {
	    if (FastTransport) {
		// FARGO algorithm
		v_residual[n_azimuthal] =
		    v_azimuthal(n_radial, n_azimuthal) - v_mean[n_radial];
	    } else {
		// Standard algorithm
		v_residual[n_azimuthal] = v_azimuthal(n_radial, n_azimuthal);
	    }
	}

	// there is no v_residual[v_radial.Nsec]
	// v_residual[v_radial.Nsec]=v_residual[0];

	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= v_radial.get_max_azimuthal(); ++n_azimuthal) {
	    double invdt1, invdt2, invdt3, invdt4, invdt5;

	    // velocity differences in radial & azimuthal direction
	    double dvRadial = v_radial(n_radial + 1, n_azimuthal) -
			      v_radial(n_radial, n_azimuthal);
	    double dvAzimuthal =
		v_azimuthal(n_radial,
			    n_azimuthal == v_radial.get_max_azimuthal()
				? 0
				: n_azimuthal + 1) -
		v_azimuthal(n_radial, n_azimuthal);

	    // sound speed limit
	    invdt1 = soundspeed(n_radial, n_azimuthal) /
		     (std::min(dxRadial, dxAzimuthal));

	    // radial motion limit
	    invdt2 = fabs(v_radial(n_radial, n_azimuthal)) / dxRadial;

	    // residual circular motion limit
	    invdt3 = fabs(v_residual[n_azimuthal]) / dxAzimuthal;

	    // artificial viscosity limit
	    if (parameters::artificial_viscosity ==
		parameters::artificial_viscosity_SN) {
		// TODO: Change to sizes defined by constants of compiler
		if (dvRadial >= 0.0) {
		    dvRadial = 1e-10;
		} else {
		    dvRadial = -dvRadial;
		}

		if (dvAzimuthal >= 0.0) {
		    dvAzimuthal = 1e-10;
		} else {
		    dvAzimuthal = -dvAzimuthal;
		}

		invdt4 =
		    4.0 * std::pow(parameters::artificial_viscosity_factor, 2) *
		    std::max(dvRadial / dxRadial, dvAzimuthal / dxAzimuthal);
	    } else {
		invdt4 = 0.0;
	    }

	    // kinematic viscosity limit
	    // TODO: Factor 4 on errors!
	    invdt5 = 4.0 * data[t_data::VISCOSITY](n_radial, n_azimuthal) *
		     std::max(1 / std::pow(dxRadial, 2),
			      1 / std::pow(dxAzimuthal, 2));

	    if (EXPLICIT_VISCOSITY) {
		// calculate new dt based on different limits
		dtLocal = parameters::CFL /
			  std::sqrt(std::pow(invdt1, 2) + std::pow(invdt2, 2) +
				    std::pow(invdt3, 2) + std::pow(invdt4, 2) +
				    std::pow(invdt5, 2));
	    } else {
		// viscous timestep
		dt_parabolic_local =
		    std::min(dt_parabolic_local,
			     parameters::CFL / std::sqrt(std::pow(invdt4, 2) +
							 std::pow(invdt5, 2)));

		// calculate new dt based on different limits
		dtLocal = parameters::CFL /
			  std::sqrt(std::pow(invdt1, 2) + std::pow(invdt2, 2) +
				    std::pow(invdt3, 2));

		dtLocal = std::min(dtLocal, 3.0 * dt_parabolic_local);
	    }

	    if (StabilizeViscosity == 2) {
		const double cphi =
		    data[t_data::VISCOSITY_CORRECTION_FACTOR_PHI](n_radial,
								  n_azimuthal);
		const double cr = data[t_data::VISCOSITY_CORRECTION_FACTOR_R](
		    n_radial, n_azimuthal);
		const double c =
		    std::min(cphi, cr); // c < 0.0 is negative, so take min to
					// get 'larger' negative number

		if (c != 0.0) {
		    const double dtStable = -parameters::CFL / c;
		    dtLocal = std::min(dtLocal, dtStable);
		}
	    }

	    if (dtLocal < dtGlobal) {
		dtGlobal = dtLocal;
		if (debug) {
		    n_radial_debug = n_radial;
		    n_azimuthal_debug = n_azimuthal;
		    if (invdt1 != 0)
			itdbg1 = 1.0 / invdt1;
		    if (invdt2 != 0)
			itdbg2 = 1.0 / invdt2;
		    if (invdt3 != 0)
			itdbg3 = 1.0 / invdt3;
		    if (invdt4 != 0)
			itdbg4 = 1.0 / invdt4;
		    if (invdt5 != 0)
			itdbg5 = 1.0 / invdt5;
		    mdtdbg = dtGlobal;
		    if ((parameters::artificial_viscosity ==
			 parameters::artificial_viscosity_SN) &&
			(parameters::artificial_viscosity_factor > 0)) {
			viscRadial =
			    dxRadial / dvRadial / 4.0 /
			    std::pow(parameters::artificial_viscosity_factor,
				     2);
			viscAzimuthal =
			    dxAzimuthal / dvAzimuthal / 4.0 /
			    std::pow(parameters::artificial_viscosity_factor,
				     2);
		    }
		}
	    }
	}
    }

    for (unsigned int n_radial = 1 + (CPUOVERLAP - 1) * (CPU_Rank > 0 ? 1 : 0);
	 n_radial <
	 NRadial - 2 - (CPUOVERLAP - 1) * (CPU_Rank < CPU_Number - 1 ? 1 : 0);
	 ++n_radial) {
	dtLocal = 2.0 * M_PI * parameters::CFL / (double)NAzimuthal /
		  fabs(v_mean[n_radial] * InvRmed[n_radial] -
		       v_mean[n_radial + 1] * InvRmed[n_radial + 1]);

	if (dtLocal < dtGlobal)
	    dtGlobal = dtLocal;
    }

    if (debug) {
	double dtGlobalLocal;
	MPI_Allreduce(&mdtdbg, &dtGlobalLocal, 1, MPI_DOUBLE, MPI_MIN,
		  MPI_COMM_WORLD);

	logging::print(LOG_DEBUG "Timestep control information for CPU %d: \n",
		       CPU_Rank);
	logging::print(
	    LOG_DEBUG "Most restrictive cell at nRadial=%d and nAzimuthal=%d\n",
	    n_radial_debug, n_azimuthal_debug);
	logging::print(LOG_DEBUG "located at radius Rmed         : %g\n",
		       Rmed[n_radial_debug]);
	logging::print(LOG_DEBUG "Sound speed limit              : %g\n",
		       itdbg1);
	logging::print(LOG_DEBUG "Radial motion limit            : %g\n",
		       itdbg2);
	logging::print(LOG_DEBUG "Residual circular motion limit : %g\n",
		       itdbg3);

	if (parameters::artificial_viscosity_factor > 0) {
	    logging::print(LOG_DEBUG "Articifial Viscosity limit     : %g\n",
			   itdbg4);
	    logging::print(LOG_DEBUG "   Arise from r with limit     : %g\n",
			   viscRadial);
	    logging::print(LOG_DEBUG "   and from theta with limit   : %g\n",
			   viscAzimuthal);
	} else {
	    logging::print(LOG_DEBUG
			   "Articifial Viscosity limit     : disabled\n");
	}
	logging::print(LOG_DEBUG "Kinematic viscosity limit      : %g\n",
		       itdbg5);
	logging::print(LOG_DEBUG "Limit time step for this cell  : %g\n",
		       mdtdbg);
	logging::print(LOG_DEBUG "Limit time step adopted        : %g\n",
			   dtGlobalLocal);
	if (dtGlobal < mdtdbg) {
	    logging::print(LOG_DEBUG "Discrepancy arise either from shear.\n");
	    logging::print(LOG_DEBUG "or from the imposed DT interval.\n");
	}
    }

    return std::max(deltaT / dtGlobal, 1.0);
}

/**
	computes soundspeed
*/
void compute_sound_speed(t_data &data, bool force_update)
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
		data[t_data::SOUNDSPEED](n_radial, n_azimuthal) =
		    std::sqrt(ADIABATICINDEX * (ADIABATICINDEX - 1.0) *
			      data[t_data::ENERGY](n_radial, n_azimuthal) /
			      data[t_data::DENSITY](n_radial, n_azimuthal));
	    } else if (parameters::Polytropic) {
		data[t_data::SOUNDSPEED](n_radial, n_azimuthal) =
		    std::sqrt(ADIABATICINDEX * constants::R / parameters::MU *
			      data[t_data::TEMPERATURE](n_radial, n_azimuthal));
	    } else { // isothermal
		// This follows from: cs/v_Kepler = H/r
		data[t_data::SOUNDSPEED](n_radial, n_azimuthal) =
		    ASPECTRATIO_REF *
		    std::sqrt(constants::G * hydro_center_mass / Rb[n_radial]) *
		    std::pow(Rb[n_radial], FLARINGINDEX);
	    }
	}
    }
}

/**
	computes aspect ratio
*/
void compute_aspect_ratio(t_data &data, bool force_update)
{
    static double last_physicaltime_calculated = -1;

    if ((!force_update) && (last_physicaltime_calculated == PhysicalTime)) {
	return;
    }
    last_physicaltime_calculated = PhysicalTime;

    for (unsigned int n_radial = 0;
	 n_radial <= data[t_data::ASPECTRATIO].get_max_radial(); ++n_radial) {
	double inv_v_kepler =
	    1.0 / (calculate_omega_kepler(Rb[n_radial]) * Rb[n_radial]);

	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= data[t_data::ASPECTRATIO].get_max_azimuthal();
	     ++n_azimuthal) {
	    if (parameters::Adiabatic || parameters::Polytropic) {
		// h = H/r = c_s,iso / v_k = c_s/sqrt(gamma) / v_k
		data[t_data::ASPECTRATIO](n_radial, n_azimuthal) =
		    data[t_data::SOUNDSPEED](n_radial, n_azimuthal) /
		    (std::sqrt(ADIABATICINDEX)) * inv_v_kepler;
	    } else {
		// h = H/r = c_s/v_k
		data[t_data::ASPECTRATIO](n_radial, n_azimuthal) =
		    data[t_data::SOUNDSPEED](n_radial, n_azimuthal) *
		    inv_v_kepler;
	    }
	}
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
		data[t_data::PRESSURE](n_radial, n_azimuthal) =
		    (ADIABATICINDEX - 1.0) *
		    data[t_data::ENERGY](n_radial, n_azimuthal);
	    } else if (parameters::Polytropic) {
		data[t_data::PRESSURE](n_radial, n_azimuthal) =
		    data[t_data::DENSITY](n_radial, n_azimuthal) *
		    std::pow(data[t_data::SOUNDSPEED](n_radial, n_azimuthal),
			     2) /
		    ADIABATICINDEX;
	    } else { // Isothermal
		// since SoundSpeed is not update from initialization, cs
		// remains axisymmetric
		data[t_data::PRESSURE](n_radial, n_azimuthal) =
		    data[t_data::DENSITY](n_radial, n_azimuthal) *
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
		data[t_data::TEMPERATURE](n_radial, n_azimuthal) =
		    parameters::MU / constants::R * (ADIABATICINDEX - 1.0) *
		    data[t_data::ENERGY](n_radial, n_azimuthal) /
		    data[t_data::DENSITY](n_radial, n_azimuthal);
	    } else if (parameters::Polytropic) {
		data[t_data::TEMPERATURE](n_radial, n_azimuthal) =
		    parameters::MU / constants::R * POLYTROPIC_CONSTANT *
		    std::pow(data[t_data::DENSITY](n_radial, n_azimuthal),
			     ADIABATICINDEX - 1.0);
	    } else { // Isothermal
		data[t_data::TEMPERATURE](n_radial, n_azimuthal) =
		    parameters::MU / constants::R *
		    data[t_data::PRESSURE](n_radial, n_azimuthal) /
		    data[t_data::DENSITY](n_radial, n_azimuthal);
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

    double H;

    compute_aspect_ratio(data, force_update);

    for (unsigned int n_radial = 0;
	 n_radial <= data[t_data::RHO].get_max_radial(); ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal <= data[t_data::RHO].get_max_azimuthal();
	     ++n_azimuthal) {
	    if (parameters::Adiabatic) {
		H = data[t_data::ASPECTRATIO](n_radial, n_azimuthal) *
		    Rb[n_radial];
	    } else {
		H = ASPECTRATIO_REF *
		    std::pow(Rb[n_radial], 1.0 + FLARINGINDEX);
	    }
	    data[t_data::RHO](n_radial, n_azimuthal) =
		data[t_data::DENSITY](n_radial, n_azimuthal) /
		(parameters::density_factor * H);
	}
    }
}
/**
	Calculates the gas mass inside the planet's Roche lobe
*/
double CircumPlanetaryMass(t_data &data)
{
    double xpl, ypl;
    double dist, mdcplocal, mdcptotal;

    /* if there's no planet, there is no mass inside its Roche lobe ;) */
    if (data.get_planetary_system().get_number_of_planets() == 0)
	return 0;

    // TODO: non global
    const double *cell_center_x = CellCenterX->Field;
    const double *cell_center_y = CellCenterY->Field;

    xpl = data.get_planetary_system().get_planet(0).get_x();
    ypl = data.get_planetary_system().get_planet(0).get_y();

    mdcplocal = 0.0;
    mdcptotal = 0.0;

    for (unsigned int n_radial = Zero_or_active; n_radial < Max_or_active;
	 ++n_radial) {
	for (unsigned int n_azimuthal = 0;
	     n_azimuthal < data[t_data::DENSITY].get_max_azimuthal();
	     ++n_azimuthal) {
	    unsigned int cell =
		n_radial * data[t_data::DENSITY].get_size_azimuthal() +
		n_azimuthal;
	    dist = std::sqrt(
		(cell_center_x[cell] - xpl) * (cell_center_x[cell] - xpl) +
		(cell_center_y[cell] - ypl) * (cell_center_y[cell] - ypl));
	    if (dist < HillRadius) {
		mdcplocal += Surf[n_radial] *
			     data[t_data::DENSITY](n_radial, n_azimuthal);
	    }
	}
    }

    MPI_Allreduce(&mdcplocal, &mdcptotal, 1, MPI_DOUBLE, MPI_SUM,
		  MPI_COMM_WORLD);

    return mdcptotal;
}
