#include "fld.h"

#ifdef _OPENMP
#include <omp.h>
#endif

#include "parameters.h"
#include "nongnu.h"
#include "constants.h"
#include "units.h"
#include "opacity.h"
#include "pvte_law.h"
#include "SourceEuler.h"
#include "boundary_conditions.h"
#include "logging.h"


namespace fld {

static inline double flux_limiter(const double R)
{
	return 1.0/3;
    // flux limiter
    // if (R <= 2) {
	// return 2.0 / (3 + std::sqrt(9 + 10 * std::pow(R, 2)));
    // } else {
	// return 10.0 / (10 * R + 9 + std::sqrt(180 * R + 81));
    // }
}

void radiative_diffusion(t_data &data, const double current_time, const double dt)
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

	const unsigned int Nphi = Kb.get_size_azimuthal();

	// We set minimum Temperature here such that H (thru Cs) is also computed with the minimum Temperature
	#pragma omp parallel for
	for (unsigned int naz = 0; naz < Nphi; ++naz) {
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
	compute_temperature(data);
	compute_sound_speed(data, current_time);
	compute_scale_height(data, current_time);

	const unsigned int NrKa = Ka.get_size_radial();
    // calcuate Ka for K(i/2,j)
	#pragma omp parallel for collapse(2)
	for (unsigned int nr = 1; nr < NrKa - 1; ++nr) {
	for (unsigned int naz = 0; naz < Nphi; ++naz) {
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

	    Ka(nr, naz) = 8.0 * 4.0	 * constants::sigma.get_code_value() *
			  lambda * H * H * std::pow(temperature, 3) * denom;
	}
    }

	#pragma omp parallel for
	for (unsigned int naz = 0; naz < Nphi; ++naz) {
		const unsigned int nr_max = Ka.get_max_radial();
		if(CPU_Rank == CPU_Highest && parameters::boundary_outer == parameters::boundary_condition_reflecting){
		Ka(nr_max-1, naz) = 0.0;
		}


		if(CPU_Rank == 0 && parameters::boundary_inner == parameters::boundary_condition_reflecting){
		Ka(1, naz) = 0.0;
		}
	}

	#pragma omp parallel for
	// Similar to Tobi's original implementation
	for (unsigned int naz = 0; naz < Ka.get_size_azimuthal(); ++naz) {
		const unsigned int nr_max = Ka.get_max_radial();
		if(CPU_Rank == CPU_Highest && !(parameters::boundary_outer == parameters::boundary_condition_reflecting || parameters::boundary_outer == parameters::boundary_condition_open)){
		Ka(nr_max-1, naz) = Ka(nr_max-2, naz);
		}


		if(CPU_Rank == 0 && !(parameters::boundary_inner == parameters::boundary_condition_reflecting || parameters::boundary_inner == parameters::boundary_condition_open)){
		Ka(1, naz) = Ka(2, naz);
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
	const unsigned int NrKb = Kb.get_size_radial();

	#pragma omp parallel for collapse(2)
	for (unsigned int nr = 1; nr < NrKb-1; ++nr) {
	for (unsigned int naz = 0; naz < Nphi; ++naz) {
	    // unsigned int n_azimuthal_plus = (n_azimuthal ==
	    // Kb.get_max_azimuthal() ? 0 : n_azimuthal + 1);
		const unsigned int naz_prev =
		(naz == 0 ? Nphi-1 : naz - 1);

	    // average temperature azimuthally
	    const double temperature =
		0.5 * (Temperature(nr, naz_prev) + Temperature(nr, naz));
		const double density = 0.5 * (Sigma(nr, naz_prev) + Sigma(nr, naz));
	    const double scale_height =
		0.5 * (Scale_height(nr, naz_prev) + Scale_height(nr, naz));

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
		(0.5 * (Temperature(nr - 1, naz_prev) + Temperature(nr - 1, naz)) -
		 0.5 * (Temperature(nr + 1, naz_prev) + Temperature(nr + 1, naz))) /
		(Ra[nr - 1] - Ra[nr + 1]);
	    const double dT_dphi =
		InvRmed[nr] * (Temperature(nr, naz) - Temperature(nr, naz_prev)) /
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
	const unsigned int Nr = Temperature.get_size_radial();
	#pragma omp parallel for collapse(2)
	for (unsigned int nr = 1; nr < Nr - 1; ++nr) {
	for (unsigned int naz = 0; naz < Nphi; ++naz) {
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
	double absolute_norm = std::numeric_limits<double>::max();
	double norm_change = std::numeric_limits<double>::max();

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
	boundary_conditions::apply_boundary_condition(data, current_time, 0.0, false);

	norm_change = absolute_norm;
	absolute_norm = 0.0;
    /// TODO: Cannot be OpenMP parallelized due to Temperature being iteratively computed ??
#pragma omp parallel for collapse(2) reduction(+ : absolute_norm)
	for (unsigned int nr = 1; nr < Nr - 1; ++nr) {
		for (unsigned int naz = 0; naz < Nphi; ++naz) {

		const double old_value = Temperature(nr, naz);
		const unsigned int naz_next =
		    (naz == Temperature.get_max_azimuthal() ? 0 : naz + 1);
		const unsigned int naz_prev =
		    (naz == 0 ? Temperature.get_max_azimuthal() : naz - 1);

		Temperature(nr, naz) =
		    (1.0 - omega) * Temperature(nr, naz) -
		    omega / B(nr, naz) *
			(A(nr, naz) * Temperature(nr - 1, naz) +
			 C(nr, naz) * Temperature(nr + 1, naz) +
			 D(nr, naz) * Temperature(nr, naz_prev) +
			 E(nr, naz) * Temperature(nr, naz_next) - Told(nr, naz));


        if(Temperature(nr, naz) < parameters::minimum_temperature){
            Temperature(nr, naz) = parameters::minimum_temperature;
        }

        if(Temperature(nr, naz) > parameters::maximum_temperature){
            Temperature(nr, naz) = parameters::maximum_temperature;
        }

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

    const double tmp = absolute_norm;
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
	#pragma omp parallel for collapse(2)
	for (unsigned int nr = 1; nr < Nr - 1; ++nr) {
	for (unsigned int naz = 0; naz < Nphi; ++naz) {
		Energy(nr, naz) = Temperature(nr, naz) *
						Sigma(nr, naz) / (parameters::ADIABATICINDEX - 1.0) /
					    parameters::MU * constants::R;
	}
    }

	SetTemperatureFloorCeilValues(data, __FILE__, __LINE__);
}


}