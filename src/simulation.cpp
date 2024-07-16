#include "simulation.h"
#include "logging.h"
#include "parameters.h"
#include "output.h"
#include "circumplanetary_mass.h"
#include "Pframeforce.h"
#include "start_mode.h"
#include "SourceEuler.h"
#include "boundary_conditions/boundary_conditions.h"
#include "commbound.h"
#include "frame_of_reference.h"
#include "particles/particles.h"
#include "viscosity/viscosity.h"
#include "viscosity/artificial_viscosity.h"
#include "TransportEuler.h"
#include "accretion.h"
#include "cfl.h"
#include "quantities.h"
#include "pvte_law.h"
#include "fld.h"
#include "options.h"
#include "global.h"

namespace sim {

double last_dt;

double time, timeInitial;
unsigned int N_snapshot = 0;
unsigned int N_monitor = 0;
unsigned long int N_hydro_iter = 0;

double total_disk_mass_old;


static void write_snapshot(t_data &data) {
	// Outputs are done here
	output::last_snapshot_dir = output::snapshot_dir;
	output::write_full_output(data, std::to_string(N_snapshot));
	output::cleanup_autosave();

	if (N_snapshot == 0 && boundary_conditions::initial_values_needed()) {
	// Write damping data as a reference.
	const std::string snapshot_dir_old = output::snapshot_dir;
	output::write_full_output(data, "reference", false);
	output::snapshot_dir = snapshot_dir_old;
	}
}

void handle_outputs(t_data &data) {
	bool need_update_for_output = true;
	N_snapshot = (N_monitor / parameters::Nmonitor); // note: integer division
	
	const bool to_write_snapshot = (parameters::Nmonitor * N_snapshot == N_monitor);
	const bool to_write_monitor = to_write_snapshot || parameters::write_at_every_timestep;


	/// asure planet torques are computed
	if (!parameters::disk_feedback && to_write_monitor) {
	    ComputeDiskOnNbodyAccel(data);
	}

	if (to_write_snapshot) {
	    need_update_for_output = false;
		write_snapshot(data);
	}

	if (to_write_snapshot && parameters::write_torques) {
		/// Write torques needs to be called after write_snapshots
		/// because it depends on output::last_snapshot_dir
		/// which is set inside write_snapshots
		need_update_for_output = true;
		output::write_torques(data, need_update_for_output);
		need_update_for_output = false;
	}

	if (to_write_monitor) {
		dt_logger.write(N_snapshot, N_monitor);
		if(ECC_GROWTH_MONITOR){
			output::write_ecc_peri_changes(sim::N_snapshot, sim::N_monitor);
		}
		output::write_monitor_time();
	    ComputeCircumPlanetaryMasses(data);
	    data.get_planetary_system().write_planets(1);

		if (parameters::write_lightcurves) {
			output::write_lightcurves(data, N_snapshot, need_update_for_output);
		}

		fld::write_logfile(output::outdir + "/monitor/fld.log");
	}

	// write disk quantities like eccentricity, ...
	if ((to_write_monitor) && parameters::write_disk_quantities) {
	    output::write_quantities(data, need_update_for_output);
	}

}

double CalculateTimeStep(t_data &data)
{
	double rv = last_dt;

	if (parameters::calculate_disk) {
		const double cfl_dt = cfl::condition_cfl(data);
		rv = std::min(parameters::CFL_max_var * last_dt, cfl_dt);
		last_dt = rv;

		if(PRINT_SIG_INFO){
			cfl::condition_cfl(data, cfl_dt);
            PRINT_SIG_INFO = false;
		}

	}
	dt_logger.update(rv);

	return rv;
}


// static double PlanNextTimestepSize(t_data &data, double dt, double force_calc)
// {

//     if (!SloppyCFL || force_calc) {
// 	sim::last_dt = dt;
// 	const double dt_cfl = cfl::condition_cfl(data, 0.0);

// 	if(PRINT_SIG_INFO){
// 		cfl::condition_cfl(data, dt_cfl);
// 	}

// 	// don't let dt grow too fast
// 	const double limited_dt = std::min(parameters::CFL_max_var * last_dt, dt_cfl);

// 	// Limit dt un such a way, that we precisely end up on DT
// 	const double deltaT = DT - dtemp; // time till full DT
// 	const double inverse_dt_limited = std::max(deltaT / limited_dt, 1.0);
// 	dt = deltaT / inverse_dt_limited;
//     }
//     return dt;
// }

/*
Do one step of the integration.

Returns the time covered.
*/
static void step_Euler(t_data &data, const double dt) {

	if (parameters::calculate_disk){
		// minimum density is assured inside AccreteOntoPlanets
	    accretion::AccreteOntoPlanets(data, dt);
	}

	if (parameters::disk_feedback) {
	    ComputeDiskOnNbodyAccel(data);
	    UpdatePlanetVelocitiesWithDiskForce(data, dt);
	}

	refframe::ComputeIndirectTermDisk(data);
	refframe::ComputeIndirectTermNbody(data, time, dt);
	refframe::ComputeIndirectTermFully();

	data.get_planetary_system().apply_indirect_term_on_Nbody(
		refframe::IndirectTerm, dt);

	if (parameters::calculate_disk) {
		/** Gravitational potential from star and planet(s) is computed and
		 * stored here*/
		if (parameters::body_force_from_potential) {
		CalculateNbodyPotential(data, time);
		} else {
		CalculateAccelOnGas(data, time);
		}
	}

	if (parameters::integrate_particles) {
		particles::update_velocities_from_indirect_term(dt);
		particles::integrate(data, time, dt);
	}

	/* Below we correct v_azimuthal, planet's position and velocities if we
	 * work in a frame non-centered on the star. Same for dust particles. */
	refframe::handle_corotation(data, dt);

	/* Now we update gas */
	if (parameters::calculate_disk) {
		//HandleCrash(data);

	    update_with_sourceterms(data, dt);

	    // compute and add acceleartions due to disc viscosity as a
	    // source term
	    art_visc::update_with_artificial_viscosity(data, dt);

	    recalculate_viscosity(data, sim::time);
	    viscosity::compute_viscous_stress_tensor(data);
	    viscosity::update_velocities_with_viscosity(data, dt);


	    if (parameters::Adiabatic) {
			SubStep3(data, time, dt);
		}
	}

	/* Do radiative transport. This can be done independent of the hydro simulation. */
	if (parameters::Adiabatic && fld::radiative_diffusion_enabled) {
		    fld::radiative_diffusion(data, time, dt);
	}
	    
	/* Continue with hydro simulation */
	if (parameters::calculate_disk) {
		boundary_conditions::apply_boundary_condition(data, time, 0.0, false);

		Transport(data, &data[t_data::SIGMA], &data[t_data::V_RADIAL],
				&data[t_data::V_AZIMUTHAL], &data[t_data::ENERGY],
				dt);
	}

	/** Planets' positions and velocities are updated from gravitational
	 * interaction with star and other planets */
	data.get_planetary_system().integrate(time, dt);
	data.get_planetary_system().copy_data_from_rebound();
	data.get_planetary_system().move_to_hydro_center_and_update_orbital_parameters();

	time += dt;
	N_hydro_iter = N_hydro_iter + 1;
	logging::print_runtime_info();

	if (parameters::calculate_disk) {
	    CommunicateBoundaries(&data[t_data::SIGMA], &data[t_data::V_RADIAL],
				  &data[t_data::V_AZIMUTHAL],
				  &data[t_data::ENERGY]);

	    // We only recompute once, assuming that cells hit by planet
	    // accretion are not also hit by viscous accretion at inner
	    // boundary.
	    if (parameters::VISCOUS_ACCRETION) {
        compute_sound_speed(data, time);
        compute_scale_height(data, time);
		viscosity::update_viscosity(data);
	    }

	    boundary_conditions::apply_boundary_condition(data, time, dt, true);

	    if(parameters::keep_mass_constant){
		const double total_disk_mass_new =
		    quantities::gas_total_mass(data, RMAX);
		data[t_data::SIGMA] *=
		    (total_disk_mass_old / total_disk_mass_new);
	    }

	    quantities::CalculateMonitorQuantitiesAfterHydroStep(data, N_monitor,
						     dt);

	    if (parameters::variableGamma &&
		!parameters::VISCOUS_ACCRETION) { // If VISCOUS_ACCRETION is active,
				      // scale_height is already updated
		// Recompute scale height after Transport to update the 3D
		// density
        compute_sound_speed(data, time);
        compute_scale_height(data, time);
	    }
	    // this must be done after CommunicateBoundaries
        recalculate_derived_disk_quantities(data, time);
	}
}

///
/// \brief step_LeapFrog
/// Gas:	kick drift kick
/// Nbody:	drift kick drift
/// \param data
/// \param step_dt
///
[[maybe_unused]] static void step_LeapFrog(t_data &data, const double step_dt)
{
	const double frog_dt = step_dt/2;
	const double start_time = time;
	const double midstep_time = time + frog_dt;
	const double end_time = time + step_dt;

	//////////////// Leapfrog compute v_i+1/2 /////////////////////

	/// Compute indirect Term is forward looking (computes acceleration from 'dt' to 'dt + frog_dt'
	/// so it must be done while Nbody is still at 'dt'
	refframe::ComputeIndirectTermNbody(data, start_time, frog_dt);
	//// Nbody drift / 2
	refframe::init_corotation(data);
	data.get_planetary_system().integrate(start_time, frog_dt);
	data.get_planetary_system().copy_data_from_rebound();
	data.get_planetary_system().move_to_hydro_center_and_update_orbital_parameters();

	if (parameters::disk_feedback) {
		ComputeDiskOnNbodyAccel(data);
	}
	refframe::ComputeIndirectTermDisk(data);

	refframe::ComputeIndirectTermFully();

	/// Nbody Kick 1 / 2
	// minimum density is assured inside AccreteOntoPlanets
	accretion::AccreteOntoPlanets(data, frog_dt);
	if (parameters::disk_feedback) {
		UpdatePlanetVelocitiesWithDiskForce(data, frog_dt);
	}
	data.get_planetary_system().apply_indirect_term_on_Nbody(refframe::IndirectTerm, frog_dt);

	if (parameters::integrate_particles) {
		particles::integrate(data, start_time, frog_dt);
		particles::update_velocities_from_indirect_term(frog_dt);
	}

	refframe::handle_corotation(data, frog_dt);

	if (parameters::calculate_disk) {
		/// Gas Kick 1/2
		if (parameters::body_force_from_potential) {
		CalculateNbodyPotential(data, start_time);
		} else {
		CalculateAccelOnGas(data, start_time);
		}

		update_with_sourceterms(data, frog_dt);

		art_visc::update_with_artificial_viscosity(data, frog_dt);

		recalculate_viscosity(data, start_time);
		viscosity::compute_viscous_stress_tensor(data);
		viscosity::update_velocities_with_viscosity(data, frog_dt);

		if (parameters::Adiabatic) {
		SubStep3(data, start_time, frog_dt);
		if (fld::radiative_diffusion_enabled) {
			fld::radiative_diffusion(data, start_time, frog_dt);
		}
		}
		//////////////// END /// Gas Kick 1/2 /////////////////////

		//////////////// Gas drift 1/1 /////////////////////
		boundary_conditions::apply_boundary_condition(data, start_time, 0.0, false);

		Transport(data, &data[t_data::SIGMA], &data[t_data::V_RADIAL],
			  &data[t_data::V_AZIMUTHAL], &data[t_data::ENERGY],
			  step_dt);
		//////////////// END Gas drift 1/1   /////////////////////

	}

	//////////////// Gas kick 2/2   /////////////////////
	/// planets positions still at x_i+1/2 for gas interaction
	if (parameters::disk_feedback) {
		ComputeDiskOnNbodyAccel(data);
	}
	refframe::ComputeIndirectTermDisk(data);
	refframe::ComputeIndirectTermNbody(data, midstep_time, frog_dt);
	refframe::ComputeIndirectTermFully();

	/// update gas while Nbody positions are still at x_i+1/2
	if (parameters::calculate_disk) {

		if (parameters::body_force_from_potential) {
		CalculateNbodyPotential(data, midstep_time);
		} else {
		CalculateAccelOnGas(data, midstep_time);
		}

		if (parameters::variableGamma) {
		compute_sound_speed(data, midstep_time);
		compute_scale_height(data, midstep_time);
		pvte::compute_gamma_mu(data);
		}
		if(parameters::self_gravity || parameters::variableGamma){
			compute_sound_speed(data, midstep_time);
			compute_scale_height(data, midstep_time);
		}

        compute_pressure(data);
		update_with_sourceterms(data, frog_dt);

		art_visc::update_with_artificial_viscosity(data, frog_dt);

		recalculate_viscosity(data, midstep_time);
		viscosity::compute_viscous_stress_tensor(data);
		viscosity::update_velocities_with_viscosity(data, frog_dt);

		if (parameters::Adiabatic) {
		SubStep3(data, midstep_time, frog_dt);
		if (fld::radiative_diffusion_enabled) {
			fld::radiative_diffusion(data, midstep_time, frog_dt);
		}
		}
	}

	/// We update particles with Nbody at x_i+1/2
	/// and gas at x_i/v_i, so we use gas at x_i+1/v_i+1 to finish the update step
	if (parameters::integrate_particles) {
	particles::update_velocities_from_indirect_term(frog_dt);
	particles::integrate(data, midstep_time, frog_dt);
	}

	/// Finish timestep of the planets but do not update Nbody system yet
	// minimum density is assured inside AccreteOntoPlanets
	accretion::AccreteOntoPlanets(data, frog_dt);

	       /// Nbody kick 2/2
	if (parameters::disk_feedback) {
	UpdatePlanetVelocitiesWithDiskForce(data, frog_dt);
	}
	data.get_planetary_system().apply_indirect_term_on_Nbody(refframe::IndirectTerm, frog_dt);

	       /// Nbody drift 2/2
	refframe::init_corotation(data);
	data.get_planetary_system().integrate(midstep_time, frog_dt);
	data.get_planetary_system().copy_data_from_rebound();
	data.get_planetary_system().move_to_hydro_center_and_update_orbital_parameters();

	/* Below we correct v_azimuthal, planet's position and velocities if we
	 * work in a frame non-centered on the star. Same for dust particles. */
	refframe::handle_corotation(data, frog_dt);
	///////////// END Nbody update  ///////////////////

	//////////////// END Leapfrog compute v_i+1   /////////////////////

	time = end_time;
	N_hydro_iter += 1;
	logging::print_runtime_info();

	if (parameters::calculate_disk) {
		CommunicateBoundaries(
		&data[t_data::SIGMA], &data[t_data::V_RADIAL],
		&data[t_data::V_AZIMUTHAL], &data[t_data::ENERGY]);

		// We only recompute once, assuming that cells hit by planet
		// accretion are not also hit by viscous accretion at inner
		// boundary.
		if (parameters::VISCOUS_ACCRETION) {
		compute_sound_speed(data, end_time);
		compute_scale_height(data, end_time);
		viscosity::update_viscosity(data);
		}

		boundary_conditions::apply_boundary_condition(data, end_time, step_dt, true);

		if(parameters::keep_mass_constant){
			const double total_disk_mass_new =
			quantities::gas_total_mass(data, RMAX);
			 data[t_data::SIGMA] *=
			(total_disk_mass_old / total_disk_mass_new);
		}

		quantities::CalculateMonitorQuantitiesAfterHydroStep(data, N_monitor,
							 step_dt);

		// this must be done after CommunicateBoundaries
		recalculate_derived_disk_quantities(data, end_time);

	}
}


void init(t_data &data) {
	boundary_conditions::apply_boundary_condition(data, time, 0.0, false);
	refframe::init_corotation(data);

	if (start_mode::mode != start_mode::mode_restart) {
		CalculateTimeStep(data);
	}

	if (parameters::calculate_disk) {
	CommunicateBoundaries(&data[t_data::SIGMA], &data[t_data::V_RADIAL],
			      &data[t_data::V_AZIMUTHAL],
			      &data[t_data::ENERGY]);
    }

	total_disk_mass_old = 1.0;
	if(parameters::keep_mass_constant){
	total_disk_mass_old =
	quantities::gas_total_mass(data, RMAX);
	}


}

static void step(t_data &data, const double step_dt) {
	switch (parameters::hydro_integrator) {
		case EULER_INTEGRATOR:
			step_Euler(data, step_dt);
			break;
		case LEAPFROG_INTEGRATOR:
			step_LeapFrog(data, step_dt);
			break;
		default:
			step_Euler(data, step_dt);
	}
}

static bool exit_on_signal() {
	if (!SIGTERM_RECEIVED) {
		return false;
	}
	return true;
}

void run(t_data &data) {

	double step_dt = last_dt;
	double cfl_dt = last_dt;

	init(data);

	const double t_final = parameters::Nsnap * parameters::Nmonitor * parameters::monitor_timestep;
	const bool iteration_restriction = options::max_iteration_number >= 0;

    for (; time < t_final;) {

		if (iteration_restriction &&  N_hydro_iter >= (long unsigned int) options::max_iteration_number) {
			break;
		}

		if (exit_on_signal()) {
			output::write_full_output(data, "autosave");
			break;
		}

		cfl_dt = CalculateTimeStep(data);

		const double time_next_monitor = (N_monitor+1)*parameters::monitor_timestep;
		const double time_left_till_write = time_next_monitor - time;

		const bool overshoot = cfl_dt > time_left_till_write;
		
		const double dt_stretch_factor = 0.05;
		const bool almost_there = time_left_till_write < cfl_dt*(1+dt_stretch_factor);

		if (overshoot || almost_there) {
			step_dt = time_left_till_write;
		} else {
			step_dt = cfl_dt;
		}

		step(data, step_dt);

		const double towrite = std::fabs(time_next_monitor - time) < 1e-6*cfl_dt;
		// TODO: document behaviour
		if (towrite) {
			N_monitor++;
			handle_outputs(data);
			logging::print_runtime_info();
		}


    }


	logging::print_runtime_final();

}





} // close namespace sim
