#pragma once

#include "../types.h"
#include <string>

#define ACCRETION_TYPE_KLEY 0
#define ACCRETION_TYPE_VISCOUS 1
#define ACCRETION_TYPE_SINKHOLE 2
#define ACCRETION_TYPE_NONE 3

struct planet_member_variables {
    unsigned int timestep;
    double m_mass;
    double m_x;
    double m_y;
    double m_vx;
    double m_vy;

    double m_cubic_smoothing_factor;
    /// accretion times^-1
    double m_acc;
    double m_accreted_mass;
    unsigned int m_planet_number;
    double m_temperature;
    double m_radius;
    bool m_irradiate;
    double m_irradiation_rampuptime;
    double m_rampuptime;
    Pair m_disk_on_planet_acceleration;
    Pair m_nbody_on_planet_acceleration;

    double m_distance_to_primary;
    double m_dimensionless_roche_radius;
    double m_circumplanetary_mass;

    /// orbital elements
    double m_semi_major_axis;
    double m_eccentricity;
    double m_mean_anomaly;
    double m_true_anomaly;
    double m_eccentric_anomaly;
    double m_pericenter_angle;

    double m_torque;
    double m_gas_torque_acc;
    double m_accretion_torque_acc;
    double m_indirect_torque_acc;
};

class t_planet
{

    friend class t_planetary_system;

  private:
    double m_mass;
    double m_x;
    double m_y;
    double m_vx;
    double m_vy;
    /// dimensionless, will be multiplied by the radius of the L1 point for smoothing
    double m_cubic_smoothing_factor;
    /// accretion times^-1
    double m_accretion_efficiency;
    int m_accretion_type;
    double m_accreted_mass;
    std::string m_name;
    unsigned int m_planet_number;
    double m_temperature;
    double m_radius;
    double m_irradiation_rampuptime;
    double m_rampuptime;
    Pair m_disk_on_planet_acceleration;
    Pair m_nbody_on_planet_acceleration;

    double m_circumplanetary_mass;
    double m_distance_to_primary;
    double m_dimensionless_roche_radius;

    /// orbital elements
	double m_omega;
	double m_orbital_period;
    double m_semi_major_axis;
    double m_eccentricity;
    double m_mean_anomaly;
    double m_true_anomaly;
    double m_eccentric_anomaly;
    double m_pericenter_angle;

    double m_torque;
    double m_gas_torque_acc;
    double m_accretion_torque_acc;
    double m_indirect_torque_acc;

    // variables not written to disk.

    double m_r;
    double m_phi;

    void update_rphi();

  public:
    void print();
    inline void add_accreted_mass(double value) { m_accreted_mass += value; }
    inline void reset_accreted_mass() { m_accreted_mass = 0.0; }
    inline void reset_torque_acc() { m_gas_torque_acc = 0.0; }
    inline void reset_accretion_torque_acc() { m_accretion_torque_acc = 0.0; }
    inline void reset_indirect_torque_acc() { m_indirect_torque_acc = 0.0; }
    // setter
    inline void set_mass(const double value) { m_mass = value; }
    inline void set_x(const double value) { m_x = value; update_rphi(); }
    inline void set_y(const double value) { m_y = value; update_rphi(); }
    inline void set_vx(const double value) { m_vx = value; }
    inline void set_vy(const double value) { m_vy = value; }
    inline void set_cubic_smoothing_factor(const double value) { m_cubic_smoothing_factor = value; }
    inline void set_accretion_efficiency(const double value) { m_accretion_efficiency = value; }
    inline void set_accretion_type(const int value) { m_accretion_type = value; }

    inline void set_torque(const double value) { m_torque = value; }
    inline void add_torque(const double dt) { m_gas_torque_acc += m_torque*dt; }
    inline void add_accretion_torque(const double value) {m_accretion_torque_acc += value;}
    inline void add_indirect_torque(const double value) {m_indirect_torque_acc += value;}
    void set_name(const std::string value);
    inline void set_planet_number(const unsigned int value)
    {
	m_planet_number = value;
    }
    inline void set_temperature(const double value) { m_temperature = value; }
    inline void set_planet_radial_extend(const double value)
    {
	m_radius = value;
    }
    inline void set_irradiation_rampuptime(const double value) { m_irradiation_rampuptime = value; }
    inline void set_rampuptime(const double value) { m_rampuptime = value; }
    inline void set_disk_on_planet_acceleration(const Pair value)
    {
	m_disk_on_planet_acceleration = value;
    }
    inline void set_nbody_on_planet_acceleration(const Pair value)
    {
	m_nbody_on_planet_acceleration = value;
    }
    inline void set_nbody_on_planet_acceleration_x(const double value)
    {
	m_nbody_on_planet_acceleration.x = value;
    }
    inline void set_nbody_on_planet_acceleration_y(const double value)
    {
	m_nbody_on_planet_acceleration.y = value;
    }
    inline void set_dimensionless_roche_radius(const double value)
    {
	m_dimensionless_roche_radius = value;
    }
    inline void set_distance_to_primary(const double value)
    {
	m_distance_to_primary = value;
    }
    inline void set_circumplanetary_mass(const double value)
    {
	m_circumplanetary_mass = value;
    }

    // getter
    inline double get_mass(void) const { return m_mass; }
	double get_rampup_mass(const double current_time) const;
    inline double get_x(void) const { return m_x; }
    inline double get_y(void) const { return m_y; }
    inline double get_vx(void) const { return m_vx; }
    inline double get_vy(void) const { return m_vy; }
    inline double get_cubic_smoothing_factor(void) const { return m_cubic_smoothing_factor; }
    inline double get_accretion_efficiency(void) const { return m_accretion_efficiency; }
    inline int get_accretion_type(void) const { return m_accretion_type; }

    inline const std::string &get_name(void) const { return m_name; }
    inline unsigned int get_planet_number(void) const
    {
	return m_planet_number;
    }
    inline double get_temperature(void) const { return m_temperature; }
    inline double get_planet_radial_extend(void) const { return m_radius; }
    inline double get_irradiate(void) const { return m_temperature > 0; }
    inline double get_irradiation_rampuptime(void) const { return m_irradiation_rampuptime; }
    inline double get_rampuptime(void) const { return m_rampuptime; }
    inline const Pair get_disk_on_planet_acceleration(void) const
    {
	return m_disk_on_planet_acceleration;
    }
    inline const Pair get_nbody_on_planet_acceleration(void) const
    {
	return m_nbody_on_planet_acceleration;
    }

    inline double get_dimensionless_roche_radius() const
    {
	return m_dimensionless_roche_radius;
    }
    inline double get_distance_to_primary() const
    {
	return m_distance_to_primary;
    }
    inline double get_circumplanetary_mass() const
    {
	return m_circumplanetary_mass;
    }
    inline double get_semi_major_axis() const { return m_semi_major_axis; }
    inline double get_eccentricity() const { return m_eccentricity; }
    inline double get_mean_anomaly() const { return m_mean_anomaly; }
    inline double get_true_anomaly() const { return m_true_anomaly; }
    inline double get_eccentric_anomaly() const { return m_eccentric_anomaly; }
    inline double get_pericenter_angle() const { return m_pericenter_angle; }
    inline double get_torque() const { return m_torque; }
    inline double get_gas_torque_acc() const { return m_gas_torque_acc; }
    inline double get_accretion_torque_acc() const { return m_accretion_torque_acc; }
    inline double get_indirect_torque_acc() const { return m_indirect_torque_acc; }
    inline double get_accreted_mass() const { return m_accreted_mass; }

    double get_r(void) const { return m_r; };
    double get_phi(void) const { return m_phi; };
    double get_angular_momentum() const;
    double get_orbital_period() const;
    double get_omega() const;
    double get_rhill() const;

    void calculate_orbital_elements(double x, double y, double vx, double vy,
				    double com_mass);
    void copy_orbital_elements(const t_planet &other);
    void set_orbital_elements_zero();

    void copy(const planet_member_variables &other);
    void create_planet_file() const;
    void write(const unsigned int file_type);
    void write_ascii(const std::string &filename) const;
    void write_binary(const std::string &filename) const;
    void restart();

    std::string get_monitor_filename() const;

    ~t_planet();
    t_planet();
};
