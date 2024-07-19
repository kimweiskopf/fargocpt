#include "planetary_system.h"
#include "../LowTasks.h"
#include "../Theo.h"
#include "../constants.h"
#include "../global.h"
#include "../logging.h"
#include "../parameters.h"
#include "../types.h"
#include "../output.h"
#include "../boundary_conditions/boundary_conditions.h"
#include <cstring>
#include <ctype.h>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <cassert>
#include <iostream>
#include <filesystem>

extern boolean CICPlanet;

t_planetary_system::t_planetary_system() {
}

t_planetary_system::~t_planetary_system()
{
    for (unsigned int i = 0; i < m_planets.size(); ++i) {
	delete m_planets.at(i);
    }

    m_planets.clear();
    reb_free_simulation(m_rebound);
}

void t_planetary_system::init_rebound()
{
    m_rebound = reb_create_simulation();
    m_rebound->G = constants::G;
    m_rebound->dt = 1e-6;
    m_rebound->softening = 0.0; // 5e-4; // Jupiter radius in au
    m_rebound->integrator = reb_simulation::REB_INTEGRATOR_IAS15;
	m_rebound->exact_finish_time = 1;
    // m_rebound->integrator = reb_simulation::REB_INTEGRATOR_MERCURIUS;
    // m_rebound->integrator = reb_simulation::REB_INTEGRATOR_WHFAST; // crashes
    for (unsigned int i = 0; i < get_number_of_planets(); ++i) {
	auto &planet = get_planet(i);
	struct reb_particle p;
	p.x = planet.get_x();
	p.y = planet.get_y();
	p.z = 0;
	p.vx = planet.get_vx();
	p.vy = planet.get_vy();
	p.vz = 0;
	p.ax = 0;
	p.ay = 0;
	p.az = 0;
	p.m = planet.get_mass();
	p.r = 0;
	p.lastcollision = 0;
	p.c = nullptr;
	p.hash = 0;
	p.ap = nullptr;
	p.sim = nullptr;
	reb_add(m_rebound, p);
    }
}

void t_planetary_system::init_system()
{

	config::Config &cfg = config::cfg;

    std::vector<config::Config> planet_configs = cfg.get_nbody_config();
    for (auto &planet_cfg : planet_configs) {
		init_planet(planet_cfg);
    }
	
	if (config::cfg.get_flag("WriteDefaultValues", "no")) {
		planet_configs[0].write_default(output::outdir + "default_config_nbody.yml");
	}

	if (CPU_Master) {
		for (auto &planet_cfg : planet_configs) {
			planet_cfg.exit_on_unknown_key();
		}
	}

    config_consistency_checks();

    init_hydro_frame_center();

    /// handle old Klahr & Kley smoothing parameter

    const double KlahrSmoothingRadius =
	config::cfg.get<double>("KlahrSmoothingRadius", 0.0);
    if(KlahrSmoothingRadius > 0.0){
	logging::print_master(
	    LOG_WARNING
	    "Deprecation Warning: KlahrSmoothingRadius is now a "
	    "Nbody parameter 'cubic smoothing factor'.\n");

	const unsigned int N_planets =
	    get_number_of_planets();

	for (unsigned int k = 0; k < N_planets; k++) {
	    t_planet &planet = get_planet(k);
	    const double x = planet.get_x();
	    const double y = planet.get_y();
	    const double r = std::sqrt(x*x + y*y);

	    if (r > 1.0e-10 && planet.get_cubic_smoothing_factor() == 0.0) { // only for non central objects
		planet.set_cubic_smoothing_factor(KlahrSmoothingRadius);
	    }
	}
    }

    init_corotation_body();

    init_rebound();

	derive_config();

	unsigned int n = get_number_of_planets();
    logging::print_master(LOG_INFO "%u planet(s) initialized.\n", n);

		// activate irradiation if its enable for any of the planets

	compute_dist_to_primary();
    init_roche_radii();
    list_planets();

	// ensure planet monitor files exist
	create_planet_files();
	MPI_Barrier(MPI_COMM_WORLD);
}

void t_planetary_system::derive_config() {
	parameters::heating_star_enabled = false;
	for (unsigned int n=0; n < get_number_of_planets(); n++) {
		const auto & planet = get_planet(n);
		if (planet.get_irradiate()) {
			parameters::heating_star_enabled = true;
			break;
		}
	}
}

// find the cell center radius in which r lies.
static double find_cell_center_radius(const double r)
{
    if (r < RMIN || r > RMAX) {
	die("Can not find cell center radius outside the grid at r = %f!", r);
    }
    unsigned int j = 0;
    while (Radii[j] < r) {
	j++;
    }
    return GlobalRmed[j - 1];
}


void t_planetary_system::init_planet(config::Config &cfg)
{
    // check if all needed quantities are present
    if (!(cfg.contains("semi-major axis") && cfg.contains("mass"))) {
	die("One of the planets does not have all of: semi-major axis and mass!");
    }

    double semi_major_axis = cfg.get<double>("semi-major axis", units::L0);
    const double mass = cfg.get<double>("mass", units::M0);

    const double eccentricity = cfg.get<double>("eccentricity", 0.0);

    const double cubic_smoothing_factor =
	cfg.get<double>("cubic smoothing factor", 0.0);

    const double accretion_efficiency =
	cfg.get<double>("accretion efficiency", 0.0);

    const double radius = cfg.get<double>("radius", "0.009304813 au", units::L0);

    const double temperature = cfg.get<double>("temperature", "0.0 K", units::Temp0);

	const double irrad_rampup = cfg.get<double>("irradiation ramp-up time", 0.0, units::T0);

	const double phi = cfg.get<double>("trueanomaly", 0.0);

    const double argument_of_pericenter =
	cfg.get<double>("argument of pericenter", 0.0);

    const double ramp_up_time = cfg.get<double>("ramp-up time", 0.0);

    std::string name = "planet" + std::to_string(get_number_of_planets());
    if (cfg.contains("name")) {
	name = cfg.get<std::string>("name");
    }

	    if (CICPlanet) {
		// initialization puts centered-in-cell planets
		if (eccentricity > 0) {
			die("Centering planet in cell and eccentricity > 0 are not supported at the same time.");
		}
		semi_major_axis = find_cell_center_radius(semi_major_axis);
	    }

    t_planet *planet = new t_planet();

	// planets starts at Periastron
	const double nu = phi;
	if (get_number_of_planets() < 2) {
	    initialize_planet_jacobi_adjust_first_two(
		planet, mass, semi_major_axis, eccentricity,
		argument_of_pericenter, nu);
	} else {
	    initialize_planet_jacobi(planet, mass, semi_major_axis,
				     eccentricity, argument_of_pericenter, nu);
	}


	if(cfg.contains("accretion method")){
	    std::string acc_method = cfg.get<std::string>("accretion method", "kley");

		if (acc_method == "sinkhole") {
			planet->set_accretion_type(ACCRETION_TYPE_SINKHOLE);
		} else if (acc_method == "viscous") {
			planet->set_accretion_type(ACCRETION_TYPE_VISCOUS);
		} else if (acc_method == "kley") {
			planet->set_accretion_type(ACCRETION_TYPE_KLEY);
		} else if (acc_method == "no" || acc_method == "none") {
			planet->set_accretion_type(ACCRETION_TYPE_NONE);
		} else {
			throw std::runtime_error("Unknown Nbody accretion mode: " + acc_method);
		}

	}

    planet->set_name(name.c_str());
    planet->set_cubic_smoothing_factor(cubic_smoothing_factor);
    planet->set_accretion_efficiency(accretion_efficiency);

    if(planet->get_accretion_efficiency() <= 0.0){
	planet->set_accretion_type(ACCRETION_TYPE_NONE);
    }

    planet->set_planet_radial_extend(radius);
    planet->set_temperature(temperature);
	planet->set_irradiation_rampuptime(irrad_rampup);
    planet->set_rampuptime(ramp_up_time);

    planet->set_disk_on_planet_acceleration(Pair()); // initialize to zero
    planet->set_nbody_on_planet_acceleration(Pair());

    add_planet(planet);

	if (planet->get_accretion_type() == ACCRETION_TYPE_VISCOUS) {
		parameters::VISCOUS_ACCRETION = true;
	}
}


void t_planetary_system::config_consistency_checks()
{
    if (get_number_of_planets() == 0) {
	die("No stars or planets!");
    }

    if ((get_number_of_planets() <= 1) && (parameters::corotating)) {
	logging::print_master(LOG_ERROR
	    "Error: Corotating frame is not possible with 0 or 1 planets.\n");
	PersonalExit(1);
    }
}

void t_planetary_system::init_corotation_body()
{
    if (parameters::corotating &&
	parameters::corotation_reference_body > get_number_of_planets() - 1) {
	die("Id of reference planet for corotation is not valid. Is '%d' but must be <= '%d'.",
	    parameters::corotation_reference_body, get_number_of_planets() - 1);
    }
}

void t_planetary_system::init_hydro_frame_center()
{
    if (parameters::n_bodies_for_hydroframe_center == 0) {
	// use all bodies to calculate hydro frame center
	parameters::n_bodies_for_hydroframe_center = get_number_of_planets();
    }
    if (parameters::n_bodies_for_hydroframe_center > get_number_of_planets()) {
	// use as many bodies to calculate hydro frame center as possible
	parameters::n_bodies_for_hydroframe_center = get_number_of_planets();
    }
    logging::print_master(LOG_INFO
	"The first %d planets are used to calculate the hydro frame center.\n",
	parameters::n_bodies_for_hydroframe_center);

	move_to_hydro_frame_center();

    update_global_hydro_frame_center_mass();
    logging::print_master(LOG_INFO
	"The mass of the planets used as hydro frame center is %e.\n",
	hydro_center_mass);
}



void t_planetary_system::list_planets()
{
    calculate_orbital_elements();

    if (!CPU_Master)
	return;

    if (get_number_of_planets() == 0) {
	// logging::print(LOG_INFO "Planet overview: No planets specified.\n");
	return;
    }

    logging::print(LOG_INFO "Planet overview:\n");
    logging::print(LOG_INFO "\n");
    logging::print(
	LOG_INFO
	" #   | name                    | mass [m0]  | x [l0]     | y [l0]     | vx         | vy         |\n");
    logging::print(
	LOG_INFO
	"-----+-------------------------+------------+------------+------------+------------+------------+\n");

    for (unsigned int i = 0; i < get_number_of_planets(); ++i) {
	logging::print(
	    LOG_INFO
	    " %3i | %-23s | %10.5g | % 10.7g | % 10.7g | % 10.7g | % 10.7g |\n",
	    i, get_planet(i).get_name().c_str(), get_planet(i).get_mass(),
	    get_planet(i).get_x(), get_planet(i).get_y(),
	    get_planet(i).get_vx(), get_planet(i).get_vy());
    }

    logging::print(LOG_INFO "\n");
    logging::print(
	LOG_INFO
	" #   | e          | a          | T [t0]     | T [a]      | accreting  | Accretion Type | Cubic Smoothing |\n");
    logging::print(
	LOG_INFO
	"-----+------------+------------+------------+------------+------------+----------------+-----------------+\n");

    for (unsigned int i = 0; i < get_number_of_planets(); ++i) {
	std::string accretion_method;
	switch (get_planet(i).get_accretion_type()){
	case ACCRETION_TYPE_KLEY:
	accretion_method = "Kley Accret.";
	break;
	case ACCRETION_TYPE_SINKHOLE:
	accretion_method = "Sinkhole Accret.";
	break;
	case ACCRETION_TYPE_VISCOUS:
	accretion_method = "Viscous Accret.";
	break;
	default:
	accretion_method = "No Accretion";
	}
	std::string cubic_smoothing;
	if (get_planet(i).get_cubic_smoothing_factor() == 0.0){
	    cubic_smoothing = "Disabled";
	} else {
	    double cubic_smoothing_factor = get_planet(i).get_cubic_smoothing_factor();
	    int precisionVal = 2;
	    cubic_smoothing = std::to_string(cubic_smoothing_factor).substr(0, std::to_string(cubic_smoothing_factor).find(".") + precisionVal + 1);
	    cubic_smoothing = cubic_smoothing + " x R_L1";
	}

	logging::print(
	    LOG_INFO
	    " %3i | % 10.7g | % 10.7g | % 10.7g | % 10.6g | % 10.7g | %14.14s | %15.15s |\n",
	    i, get_planet(i).get_eccentricity(),
	    get_planet(i).get_semi_major_axis(),
	    get_planet(i).get_orbital_period(),
	    get_planet(i).get_orbital_period() * units::time.get_code_to_cgs_factor() /
		units::cgs_Year,
	    get_planet(i).get_accretion_efficiency(), accretion_method.c_str(), cubic_smoothing.c_str());
    }

    logging::print(LOG_INFO "\n");
    logging::print(
	LOG_INFO
	" #   | Temp [K]   | R [l0]      | irradiates | rampuptime |\n");
    logging::print(
	LOG_INFO
	"-----+------------+-------------+------------+------------+\n");

    for (unsigned int i = 0; i < get_number_of_planets(); ++i) {
	logging::print(LOG_INFO
		       " %3i | % 10.7g | % 11.5g |        %s | % 10.7g |\n",
		       i, get_planet(i).get_temperature() * units::temperature,
		       get_planet(i).get_planet_radial_extend(),
		       (get_planet(i).get_irradiate()) ? "yes" : " no",
		       get_planet(i).get_rampuptime());
    }

    for (unsigned int i = 0; i < get_number_of_planets(); ++i) {
	if(get_planet(i).get_accretion_efficiency() > 0.0){
	if(get_planet(i).get_orbital_period() <= 0.0){
	    die("Planet %d: %s cannot accret without an orbital period!", i, get_planet(i).get_name().c_str());
	}
	}
    }

    logging::print(LOG_INFO "\n");
}

void t_planetary_system::rotate(double angle)
{
    for (unsigned int i = 0; i < get_number_of_planets(); ++i) {
	auto &planet = get_planet(i);
	const double x = planet.get_x();
	const double y = planet.get_y();
	const double vx = planet.get_vx();
	const double vy = planet.get_vy();

	// rotate positions
	const double new_x = x * std::cos(angle) + y * std::sin(angle);
	const double new_y = -x * std::sin(angle) + y * std::cos(angle);

	// rotate velocities
	const double new_vx = vx * std::cos(angle) + vy * std::sin(angle);
	const double new_vy = -vx * std::sin(angle) + vy * std::cos(angle);

	// save new values
	planet.set_x(new_x);
	planet.set_y(new_y);
	planet.set_vx(new_vx);
	planet.set_vy(new_vy);
    }
}

void t_planetary_system::restart()
{

    logging::print_master(LOG_INFO "Loading planets ...");
    for (unsigned int i = 0; i < get_number_of_planets(); ++i) {
	get_planet(i).restart();
    }
    logging::print_master(LOG_INFO " done\n");

    logging::print_master(LOG_INFO "Loading rebound ...");

    
	reb_free_simulation(m_rebound);
	std::string rebound_filename = output::snapshot_dir + "/rebound.bin";
	m_rebound = reb_create_simulation_from_binary(
	    (char *)rebound_filename.c_str());
    
    logging::print_master(LOG_INFO " done\n");
}

void t_planetary_system::create_planet_files()
{
    for (unsigned int i = 0; i < get_number_of_planets(); ++i) {
		auto & p = get_planet(i);
		if (!std::filesystem::exists(p.get_monitor_filename())) {
			p.create_planet_file();
		}
    }
}

void t_planetary_system::write_planets(int file_type)
{
    for (unsigned int i = 0; i < get_number_of_planets(); ++i) {
	get_planet(i).write(file_type);
    }

	if (CPU_Master && file_type == 0) {
	const std::string rebound_filename = output::snapshot_dir + "/rebound.bin";
	reb_output_binary(m_rebound, rebound_filename.c_str());
    }
}

/**
   Set the position and velocity of the first planet according
   to the second planets position and velocity.

   This is done because the orbital elements of a single object are meaningless
   and the first two bodies have to be initialized together.
*/
void t_planetary_system::initialize_planet_jacobi_adjust_first_two(
    t_planet *planet, double mass, double semi_major_axis, double eccentricity,
    double omega, double true_anomaly)
{
    if (get_number_of_planets() == 0) {
	// first planet always goes to origin
	planet->set_mass(mass);
	planet->set_x(0.0);
	planet->set_y(0.0);
	planet->set_vx(0.0);
	planet->set_vy(0.0);
    } else {
	// initialize the second planet around the origin, such that the two
	// bodies have the correct separation

	// Flip pericenter angle such that heavier component sits at the
	// center in the beginning and shift from there.
	if(mass > m_planets[0]->get_mass()){
		omega += M_PI;
	}

	initialize_planet_jacobi(planet, mass, semi_major_axis, eccentricity,
				 omega, true_anomaly);
	t_planet *planet1 = m_planets[0];
	t_planet *planet2 = planet;

	double m1 = planet1->get_mass();
	double m2 = planet2->get_mass();

	// values are now such that the distance between the first two bodies
	// match

	double x = planet2->get_x();
	double y = planet2->get_y();
	double vx = planet2->get_vx();
	double vy = planet2->get_vy();

	// move both bodies into barycenter

	double k1 = m2 / (m1 + m2);
	planet1->set_x(-k1 * x);
	planet1->set_y(-k1 * y);
	planet1->set_vx(-k1 * vx);
	planet1->set_vy(-k1 * vy);

	double k2 = m1 / (m1 + m2);
	planet2->set_x(k2 * x);
	planet2->set_y(k2 * y);
	planet2->set_vx(k2 * vx);
	planet2->set_vy(k2 * vy);
    }
}

/**
   Initialize the planets position and velocity using jacobian coordinates
*/
void t_planetary_system::initialize_planet_jacobi(t_planet *planet, double mass,
						  double semi_major_axis,
						  double eccentricity,
						  double omega,
						  double true_anomaly)
{
    planet->set_mass(mass);
    Pair com = get_center_of_mass(); // of all previously added planets
    double com_mass = get_mass();    // of all previously added planets

    // some temporary variables for optimization and legibility
    double cos_ota = std::cos(omega + true_anomaly);
    double sin_ota = std::sin(omega + true_anomaly);
    double cos_o = std::cos(omega);
    double sin_o = std::sin(omega);
    double cos_ta = std::cos(true_anomaly);
    double sin_ta = std::sin(true_anomaly);

    double r = semi_major_axis * (1 - eccentricity * eccentricity) /
	       (1 + eccentricity * cos_ta);
    double x = com.x + r * cos_ota;
    double y = com.y + r * sin_ota;

    double v = 0.0;
    if (semi_major_axis > 0.0) {
	v = sqrt(constants::G * (com_mass + mass) /
		 (semi_major_axis * (1 - eccentricity * eccentricity)));
    }

    double vx = v * (-cos_o * sin_ta - sin_o * (eccentricity + cos_ta));
    double vy = v * (-sin_o * sin_ta + cos_o * (eccentricity + cos_ta));

    planet->set_x(x);
    planet->set_y(y);
    planet->set_vx(vx);
    planet->set_vy(vy);
}

/**
   Get the sum of masses of the first n particles
*/
double t_planetary_system::get_mass(unsigned int n) const
{
    double mass = 0.0;
    for (unsigned int i = 0; i < n; i++) {
	mass += get_planet(i).get_mass();
    }
    return mass;
}

/**
   Get the sum of masses of all particles
*/
double t_planetary_system::get_mass() const
{
    return get_mass(get_number_of_planets());
}

/**
   Get the center of mass of the first n particles
*/
Pair t_planetary_system::get_center_of_mass(unsigned int n) const
{
    double x = 0.0;
    double y = 0.0;
    double mass = 0.0;
    for (unsigned int i = 0; i < n; i++) {
	t_planet &planet = get_planet(i);
	mass += planet.get_mass();
	x += planet.get_x() * planet.get_mass();
	y += planet.get_y() * planet.get_mass();
    }
    Pair com;
    if (mass > 0) {
	com.x = x / mass;
	com.y = y / mass;
    } else {
	com.x = 0.0;
	com.y = 0.0;
    }
    return com;
}

/**
   Get the velocity of the center of mass of the first n particles
*/
Pair t_planetary_system::get_center_of_mass_velocity(unsigned int n) const
{
    double vx = 0.0;
    double vy = 0.0;
    double mass = 0.0;
    for (unsigned int i = 0; i < n; i++) {
	t_planet &planet = get_planet(i);
	mass += planet.get_mass();
	vx += planet.get_vx() * planet.get_mass();
	vy += planet.get_vy() * planet.get_mass();
    }
    Pair vcom;
    if (mass > 0) {
	vcom.x = vx / mass;
	vcom.y = vy / mass;
    } else {
	vcom.x = 0.0;
	vcom.y = 0.0;
    }
    return vcom;
}

Pair t_planetary_system::get_center_of_mass_velocity() const
{
    return get_center_of_mass_velocity(get_number_of_planets());
}

/**
   Get the center of mass of all particles
*/
Pair t_planetary_system::get_center_of_mass() const
{
    return get_center_of_mass(get_number_of_planets());
}

/**
   Get the center of coordinate system as chosen by
   parameters::n_bodies_for_hydroframe_center.
   This is the function to be called later in the code
   whenever the distance to the center is used.
*/
Pair t_planetary_system::get_hydro_frame_center_position() const
{
    return get_center_of_mass(parameters::n_bodies_for_hydroframe_center);
}

Pair t_planetary_system::get_hydro_frame_center_delta_vel_rebound_predictor(const double dt) const
{


    struct reb_simulation *rebound_predictor = reb_copy_simulation(m_rebound);
    reb_integrate(rebound_predictor, m_rebound->t + dt);

	double vx_old = 0.0;
	double vy_old = 0.0;
	double vx_new = 0.0;
	double vy_new = 0.0;
	double mass = 0.0;
	for (unsigned int i = 0; i < parameters::n_bodies_for_hydroframe_center; i++) {
    const double new_vx = rebound_predictor->particles[i].vx;
    const double new_vy = rebound_predictor->particles[i].vy;
	const double planet_m = m_rebound->particles[i].m;
	const double old_vx = m_rebound->particles[i].vx;
	const double old_vy = m_rebound->particles[i].vy;
	mass += planet_m;
	vx_old += old_vx * planet_m;
	vy_old += old_vy * planet_m;
	vx_new += new_vx * planet_m;
	vy_new += new_vy * planet_m;
	}
	Pair delta_vel;
	if (mass > 0) {
	delta_vel.x = (vx_new - vx_old) / mass;
	delta_vel.y = (vy_new - vy_old) / mass;
	} else {
	delta_vel.x = 0.0;
	delta_vel.y = 0.0;
	}
    reb_free_simulation(rebound_predictor);
	return delta_vel;
}


Pair t_planetary_system::get_hydro_frame_center_velocity() const
{
    return get_center_of_mass_velocity(
	parameters::n_bodies_for_hydroframe_center);
}

double t_planetary_system::compute_hydro_frame_center_mass() const
{
	const double Nbody_mass = get_mass(parameters::n_bodies_for_hydroframe_center);
	return Nbody_mass;
}

/**
   Update the global variable hydro_center_mass.
*/

void t_planetary_system::update_global_hydro_frame_center_mass()
{
	hydro_center_mass = compute_hydro_frame_center_mass();
}


void t_planetary_system::apply_indirect_term_on_Nbody(const pair accel, const double dt)
{

	for (unsigned int k = 0;
	 k < get_number_of_planets(); k++) {
		t_planet &planet = get_planet(k);
		const double new_vx =
		planet.get_vx() + dt * accel.x;
		const double new_vy =
		planet.get_vy() + dt * accel.y;

		planet.set_vx(new_vx);
		planet.set_vy(new_vy);
	}
}

/**
 * @brief t_planetary_system::shift_to_hydro_frame_center
 * move positions and velocities to hydro_frame_center
 */
void t_planetary_system::move_to_hydro_frame_center()
{
	const Pair center = get_hydro_frame_center_position();
	const Pair vcenter = get_hydro_frame_center_velocity();

    for (unsigned int i = 0; i < get_number_of_planets(); i++) {
	t_planet &planet = get_planet(i);
	const double x = planet.get_x();
	const double y = planet.get_y();
	const double vx = planet.get_vx();
	const double vy = planet.get_vy();

	planet.set_x(x - center.x);
	planet.set_y(y - center.y);
	planet.set_vx(vx - vcenter.x);
	planet.set_vy(vy - vcenter.y);
    }
}


/**
   Calculate orbital elements of all planets.
 */
void t_planetary_system::calculate_orbital_elements()
{
    double x, y, vx, vy, M;
    for (unsigned int i = 0; i < get_number_of_planets(); i++) {
	auto &planet = get_planet(i);
	if (i == 0 && parameters::n_bodies_for_hydroframe_center == 1) {
	    get_planet(0).set_orbital_elements_zero();
	    continue;
	}

	Pair com_pos = get_center_of_mass(i);
	Pair com_vel = get_center_of_mass_velocity(i);

	M = get_mass(i);
	x = planet.get_x() - com_pos.x;
	y = planet.get_y() - com_pos.y;
	vx = planet.get_vx() - com_vel.x;
	vy = planet.get_vy() - com_vel.y;

	planet.calculate_orbital_elements(x, y, vx, vy, M);
    }

	// Binary, both stars have same orbital elements
	if(get_number_of_planets() == 2){
		auto &primary = get_planet(0);
		const auto &secondary = get_planet(1);
		primary.copy_orbital_elements(secondary);
	}
}

/**
   Copy positions, velocities and masses
   from planetary system to rebound.
*/
void t_planetary_system::copy_data_to_rebound()
{
    for (unsigned int i = 0; i < get_number_of_planets(); i++) {
	auto &planet = get_planet(i);
	m_rebound->particles[i].x = planet.get_x();
	m_rebound->particles[i].y = planet.get_y();
	m_rebound->particles[i].vx = planet.get_vx();
	m_rebound->particles[i].vy = planet.get_vy();
	m_rebound->particles[i].m = planet.get_mass();
    }
}

/**
   Copy positions, velocities and masses back
   from rebound to planetary system.
*/
void t_planetary_system::copy_data_from_rebound_update_orbital_parameters()
{
    for (unsigned int i = 0; i < get_number_of_planets(); i++) {
	auto &planet = get_planet(i);
	planet.set_x(m_rebound->particles[i].x);
	planet.set_y(m_rebound->particles[i].y);
	planet.set_vx(m_rebound->particles[i].vx);
	planet.set_vy(m_rebound->particles[i].vy);
    }

	move_to_hydro_frame_center();

	/// Needed for Aspectratio mode = 1
	/// and to correctly compute circumplanetary disk mass
	compute_dist_to_primary();
	/// Needed if they can change and massoverflow or planet accretion
	/// is on
	calculate_orbital_elements();
}

/**
   Copy positions, velocities and masses back
   from rebound to planetary system.
*/
void t_planetary_system::copy_data_from_rebound()
{
	for (unsigned int i = 0; i < get_number_of_planets(); i++) {
	auto &planet = get_planet(i);
	planet.set_x(m_rebound->particles[i].x);
	planet.set_y(m_rebound->particles[i].y);
	planet.set_vx(m_rebound->particles[i].vx);
	planet.set_vy(m_rebound->particles[i].vy);
	}
}

/**
   Move Nbody system to hydro center and recompute orbital elements
*/
void t_planetary_system::move_to_hydro_center_and_update_orbital_parameters()
{

	move_to_hydro_frame_center();

	/// Needed for Aspectratio mode = 1
	/// and to correctly compute circumplanetary disk mass
	compute_dist_to_primary();
	/// Needed if they can change and massoverflow or planet accretion
	/// is on
	calculate_orbital_elements();
}


/**
   Integrate the nbody system forward in time using rebound.
*/
void t_planetary_system::integrate(const double time, const double dt)
{
    if (get_number_of_planets() < 2) {
	// don't integrate a single particle that doesn't move
	return;
    }

	copy_data_to_rebound();
	m_rebound->t = time;

    reb_integrate(m_rebound, time + dt);
}

/**
	Updates planets velocities due to disk influence if "DiskFeedback" is
   set.
*/
void t_planetary_system::correct_velocity_for_disk_accel()
{

    if (!parameters::disk_feedback) {
	return;
    }

    for (unsigned int k = 0; k < get_number_of_planets(); k++) {

	/*
	 * from centrifugal balance folows
	 * v_new**2 / r = a_disk + v_old**2 / r
	 * v_new = sqrt(v_old**2 - r * a_disk)
	 */
	t_planet &planet = get_planet(k);

	const Pair gas_accel = planet.get_disk_on_planet_acceleration();
	const double vx_old = planet.get_vx();
	const double vy_old = planet.get_vy();
	const double v_old =
	    std::sqrt(std::pow(vx_old, 2.0) + std::pow(vy_old, 2.0));

	if (v_old == 0.0) {
	    continue;
	}

	const double x = planet.get_x();
	const double y = planet.get_y();
	const double specific_torque_gas =
	    gas_accel.x * x + gas_accel.y * y; // = a_disk * r

	if (specific_torque_gas > std::pow(v_old, 2.0)) {
	    continue;
	}

	const double v_new =
	    std::sqrt(std::pow(v_old, 2.0) - specific_torque_gas);

	const double new_vx = v_new / v_old * vx_old;
	const double new_vy = v_new / v_old * vy_old;

	planet.set_vx(new_vx);
	planet.set_vy(new_vy);
    }
}

void t_planetary_system::compute_dist_to_primary()
{

    if (get_number_of_planets() < 2) {
	return;
    }

    auto &primary = get_planet(0);
    const double x = primary.get_x();
    const double y = primary.get_y();

    for (unsigned int i = 1; i < get_number_of_planets(); ++i) {
	auto &planet = get_planet(i);
	const double dx = planet.get_x() - x;
	const double dy = planet.get_y() - y;

	const double dist = std::sqrt(std::pow(dx, 2) + std::pow(dy, 2));
	planet.set_distance_to_primary(dist);

	if (i == 1) { // primary looks at secondary
	    primary.set_distance_to_primary(dist);
	}
    }
}

void t_planetary_system::init_roche_radii()
{

    auto &primary = get_planet(0);
    if (get_number_of_planets() < 2) {
	primary.set_dimensionless_roche_radius(1.0);
    primary.set_distance_to_primary(RMAX);
	return;
    }

    const double M = primary.get_mass();
    for (unsigned int i = 1; i < get_number_of_planets(); ++i) {
	auto &planet = get_planet(i);
	const double m = planet.get_mass();

	if(m == 0){
		planet.set_dimensionless_roche_radius(0.0);
		primary.set_dimensionless_roche_radius(1.0);
		return;
	}
	if(M == 0){
		primary.set_dimensionless_roche_radius(0.0);
		planet.set_dimensionless_roche_radius(1.0);
		return;
	}

	double x = 0.0;
	if (M > m) {
	    x = init_l1(M, m);
	} else {
	    x = 1.0 - init_l1(m, M);
	}
	planet.set_dimensionless_roche_radius(x);

	if (i == boundary_conditions::rof_planet) {
	    primary.set_dimensionless_roche_radius(1.0 - x);
	}
    }
}

void t_planetary_system::update_roche_radii()
{

    if (get_number_of_planets() < 2) {
	return;
    }

    auto &primary = get_planet(0);
    const double M = primary.get_mass();
    for (unsigned int i = 1; i < get_number_of_planets(); ++i) {
	auto &planet = get_planet(i);
	const double m = planet.get_mass();
	double x = planet.get_dimensionless_roche_radius();

	if (M > m) {
	    x = update_l1(M, m, x);
	    planet.set_dimensionless_roche_radius(x);
	} else {
	    x = 1.0 - x;
	    x = update_l1(m, M, x);
	    x = 1.0 - x;
	}

	if (i == 1) {
	    primary.set_dimensionless_roche_radius(1.0 - x);
	}
    }
}

/**
 * @brief t_planetary_system::correct_planet_accretion
 * the planet accretion area scales with planet radius^2
 * thus we scale the planet accretion rate with a^2 / r_current^2
 * the scaling with 1 / r_current^2 happens in AccreteOntoSinglePlanet.
 */
/*
void t_planetary_system::correct_planet_accretion()
{
for (unsigned int i = 0; i < get_number_of_planets(); ++i) {

auto &planet = get_planet(i);
double acc = planet.get_acc();
const double a = planet.get_semi_major_axis();

acc *= a*a;
planet.set_acc(acc);
}
}*/
