#include "planetary_system.h"
#include "logging.h"
#include "parameters.h"
#include "LowTasks.h"
#include "constants.h"
#include "global.h"
#include <math.h>
#include <stdio.h>
#include <ctype.h>
#include <cstring>
#include "types.h"

extern boolean CICPlanet;
extern int Corotating;

t_planetary_system::t_planetary_system()
{
}

t_planetary_system::~t_planetary_system()
{
	for (unsigned int i = 0; i < m_planets.size(); ++i) {
		delete m_planets.at(i);
	}

	m_planets.clear();
}

void t_planetary_system::read_from_file(char *filename) {
	FILE *fd;

	// check if a filename was specified
	if (filename == NULL) {
		logging::print_master(LOG_INFO "No planetfile specified.\n");
		return;
	}

	// open fill
	fd = fopen(filename, "r");

	// check if file was readable
	if (fd == NULL) {
		logging::print_master(LOG_ERROR "Error : can't find '%s'.\n", filename);
		PersonalExit(1);
		return;
	}

	char buffer[512];

	// read line by line
	while (fgets(buffer, sizeof(buffer), fd) != NULL) {
		char name[80], feeldisk[5], feelother[5], irradiate[5];
		double semi_major_axis, mass, acc, eccentricity = 0.0, temperature, radius, phi, rampuptime;
		int num_args;

		// check if this line is a comment
		if ((strlen(buffer) > 0) && (buffer[0] == '#'))
			continue;

		// try to cut line into pieces
		num_args = sscanf(buffer, "%80s %lf %lf %lf %5s %5s %lf %lf %lf %5s %lf %lf", name, &semi_major_axis, &mass, &acc, feeldisk, feelother, &eccentricity, &radius, &temperature, irradiate, &phi, &rampuptime);
		if (num_args < 6)
			continue;

		if (num_args < 7) {
			eccentricity = 0.0;
		}

		if (num_args < 8) {
			radius = 0.009304813;
		}

		if (num_args < 9) {
			temperature = 5778.0;
		}

		if (num_args < 10) {
			irradiate[0] = 'n';
		}

		if (num_args < 11) {
			phi = 0.0;
		}

		if (num_args < 12) {
			rampuptime = 0;
		}

		if (CICPlanet) {
			// initialization puts centered-in-cell planets (with excentricity = 0 only)
			unsigned int j = 0;
			while ( GlobalRmed[j] < semi_major_axis)
				j++;
			semi_major_axis = Radii[j+1];
		}

		t_planet* planet = new t_planet();

		if (parameters::no_default_star) {
			// planets starts at Apastron
			double nu = PI;
			double pericenter_angle = 0.0;
			initialize_planet_jacobi(planet, mass, semi_major_axis, eccentricity, pericenter_angle, nu);
		} else {
			initialize_planet_legacy(planet, mass, semi_major_axis, eccentricity, phi);
		}
		planet->set_name(name);
		planet->set_acc(acc);

		if (tolower(feeldisk[0]) == 'y') {
			planet->set_feeldisk(true);
		} else {
			if (parameters::disk_feedback  == YES) {
				logging::print_master("\n\n\n");
				logging::print_master(LOG_WARNING "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
				logging::print_master((std::string(LOG_WARNING) + "UNPHYSICAL SETTING! Disk feedback is activated but disk interaction is disabled for planet " + std::string(planet->get_name()) + "!\n").c_str());
				logging::print_master(LOG_WARNING "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n\n");
			}
			planet->set_feeldisk(false);
		}

		if (tolower(feelother[0]) == 'y') {
			planet->set_feelother(true);
		} else {
			planet->set_feelother(false);
		}

		planet->set_radius(radius);
		planet->set_temperature(temperature/units::temperature);
		planet->set_irradiate(tolower(irradiate[0]) == 'y');
		planet->set_rampuptime(rampuptime);

		planet->set_disk_on_planet_acceleration( Pair() ); // initialize to zero
		planet->set_nbody_on_planet_acceleration( Pair() );

		add_planet(planet);
	}

	// close file
	fclose(fd);

	logging::print_master(LOG_INFO "%d planet(s) found.\n", get_number_of_planets());

	if (get_number_of_planets() > 0) {
		HillRadius = get_planet(0).get_x() * pow(get_planet(0).get_mass()/3.,1./3.);
	} else {
		HillRadius = 0;
	}

	// set up barycenter mode
	if (parameters::no_default_star && get_number_of_planets() == 0) {
		die("NoDefaultStar is True but number of bodies is 0!");
	}
	if (parameters::n_bodies_for_barycenter == 0) {
		// use all bodies to calculate barycenter
		parameters::n_bodies_for_barycenter = get_number_of_planets();
	}
	if (parameters::n_bodies_for_barycenter > get_number_of_planets()) {
		// use as many bodies to calculate barycenter as possible
		parameters::n_bodies_for_barycenter = get_number_of_planets();
	}
	logging::print_master(LOG_INFO "The first %d planets are used to calculate the frame center.\n", parameters::n_bodies_for_barycenter);
	if (Corotating == YES && parameters::corotation_reference_body > get_number_of_planets() -1) {
		die("Id of reference planet for corotation is not valid. Is '%d' but must be <= '%d'.", parameters::corotation_reference_body, get_number_of_planets() -1);
	}

}

void t_planetary_system::list_planets()
{
	if (!CPU_Master)
		return;

	if (get_number_of_planets() == 0) {
		//logging::print(LOG_INFO "Planet overview: No planets specified.\n");
		return;
	}

	logging::print(LOG_INFO "Planet overview:\n");
	logging::print(LOG_INFO "\n");
	logging::print(LOG_INFO " #   | name                    | mass [m0]  | x [l0]     | y [l0]     | vx         | vy         |\n");
	logging::print(LOG_INFO "-----+-------------------------+------------+------------+------------+------------+------------+\n");

	for (unsigned int i = 0; i < get_number_of_planets(); ++i) {
		logging::print(LOG_INFO " %3i | %-23s | % 10.7g | % 10.7g | % 10.7g | % 10.7g | % 10.7g |\n",i , get_planet(i).get_name(), get_planet(i).get_mass(), get_planet(i).get_x(), get_planet(i).get_y(), get_planet(i).get_vx(), get_planet(i).get_vy());
	}

	logging::print(LOG_INFO "\n");
	logging::print(LOG_INFO " #   | e          | a          | T [t0]     | T [a]      | accreting  | feels disk | feels plan.|\n");
	logging::print(LOG_INFO "-----+------------+------------+------------+------------+------------+------------+------------+\n");

	for (unsigned int i = 0; i < get_number_of_planets(); ++i) {
		logging::print(LOG_INFO " %3i | % 10.7g | % 10.7g | % 10.7g | % 10.6g | % 10.7g |          %c |          %c |\n",i ,get_planet(i).get_eccentricity(), get_planet(i).get_semi_major_axis(), get_planet(i).get_period(),get_planet(i).get_period()*units::time.get_cgs_factor()/(24*60*60*365.2425), get_planet(i).get_acc(), (get_planet(i).get_feeldisk()) ? 'X' : '-', (get_planet(i).get_feelother()) ? 'X' : '-');
	}

	logging::print(LOG_INFO "\n");
	logging::print(LOG_INFO " #   | Temp [K]   | R [l0]     | irradiates | rampuptime |\n");
	logging::print(LOG_INFO "-----+------------+------------+------------+------------+\n");

	for (unsigned int i = 0; i < get_number_of_planets(); ++i) {
		logging::print(LOG_INFO " %3i | % 10.7g | % 10.7g |          %c | % 10.7g |\n",i ,get_planet(i).get_temperature()*units::temperature, get_planet(i).get_radius(), (get_planet(i).get_irradiate()) ? 'X' : '-', get_planet(i).get_rampuptime());
	}

logging::print(LOG_INFO "\n");
}

void t_planetary_system::rotate(double angle)
{
	for (unsigned int i = 0; i < get_number_of_planets(); ++i) {
		// rotate positions
		double old_x = get_planet(i).get_x();
		double old_y = get_planet(i).get_y();
		get_planet(i).set_x( old_x*cos(angle)+old_y*sin(angle));
		get_planet(i).set_y(-old_x*sin(angle)+old_y*cos(angle));

		// rotate velocities
		double old_vx = get_planet(i).get_vx();
		double old_vy = get_planet(i).get_vy();
		get_planet(i).set_vx( old_vx*cos(angle)+old_vy*sin(angle));
		get_planet(i).set_vy(-old_vx*sin(angle)+old_vy*cos(angle));
	}
}

void t_planetary_system::restart(unsigned int timestep)
{
	for (unsigned int i = 0; i < get_number_of_planets(); ++i) {
		get_planet(i).restart(timestep);
	}
}

void t_planetary_system::create_planet_files()
{
	for (unsigned int i = 0; i < get_number_of_planets(); ++i) {
		get_planet(i).create_planet_file();
	}
}

void t_planetary_system::write_planets(unsigned int timestep, bool big_file)
{
	for (unsigned int i = 0; i < get_number_of_planets(); ++i) {
		get_planet(i).write(timestep, big_file);
	}
}

/**
   Initialize the planets position and velocity in the legacy way
*/
void t_planetary_system::initialize_planet_legacy(t_planet *planet, double mass, double semi_major_axis, double eccentricity, double phi)
{
	planet->set_mass(mass);
	// planets starts at Apastron
	double r = semi_major_axis*(1.0+eccentricity);
	planet->set_x(r*cos(phi));
	planet->set_y(r*sin(phi));
	double v=0.0;
	if (semi_major_axis != 0.0) {
		v = sqrt(constants::G*(1.0+mass)/semi_major_axis)*sqrt( (1.0-eccentricity)/(1.0+eccentricity) );
	}
	planet->set_vx(-v*sin(phi));
	planet->set_vy(v*cos(phi));
}

/**
   Initialize the planets position and velocity using jacobian coordinates
*/
void t_planetary_system::initialize_planet_jacobi(t_planet *planet, double mass, double semi_major_axis, double eccentricity, double omega, double true_anomaly)
{
	planet->set_mass(mass);
	Pair com = get_center_of_mass(); // of all previously added planets
	double com_mass = get_mass();    // of all previously added planets

	// some temporary variables for optimization and legibility
	double cos_ota = cos(omega + true_anomaly);
	double sin_ota = sin(omega + true_anomaly);
	double cos_o   = cos(omega);
	double sin_o   = sin(omega);
	double cos_ta  = cos(true_anomaly);
	double sin_ta  = sin(true_anomaly);

	double r = semi_major_axis*(1-eccentricity*eccentricity)/(1+eccentricity*cos_ta);
	double x = com.x + r*cos_ota;
	double y = com.y + r*sin_ota;

	double v = 0.0;
	if (semi_major_axis > 0.0) {
		v = sqrt( constants::G*(com_mass + mass) / (semi_major_axis*(1-eccentricity*eccentricity)) );
	}

	double vx = v*( -cos_o*sin_ta - sin_o*(eccentricity + cos_ta) );
	double vy = v*( -sin_o*sin_ta + cos_o*(eccentricity + cos_ta) );

	planet->set_x(x);
	planet->set_y(y);
	planet->set_vx(vx);
	planet->set_vy(vy);
}

/**
   Get the sum of masses of the first n particles
*/
double t_planetary_system::get_mass(unsigned int n)
{
	double mass = 0.0;
	for (unsigned int i=0; i<n; i++) {
		mass += get_planet(i).get_mass();
	}
	return mass;
}

/**
   Get the sum of masses of all particles
*/
double t_planetary_system::get_mass()
{
	return get_mass(get_number_of_planets());
}

/**
   Get the center of mass of the first n particles
*/
Pair t_planetary_system::get_center_of_mass(unsigned int n)
{
	double x = 0.0;
	double y = 0.0;
	double mass = 0.0;
	for (unsigned int i=0; i<n; i++) {
		t_planet &planet = get_planet(i);
		mass += planet.get_mass();
		x += planet.get_x()*planet.get_mass();
		y += planet.get_y()*planet.get_mass();
	}
	Pair com;
	if (mass > 0) {
		com.x = x/mass;
		com.y = y/mass;
	} else {
		com.x = 0.0;
		com.y = 0.0;
	}
	return com;
}

/**
   Get the center of mass of all particles
*/
Pair t_planetary_system::get_center_of_mass()
{
	return get_center_of_mass(get_number_of_planets());
}
